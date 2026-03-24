// ws_server.c - WebSocket server minimal (no external deps)
// - Accepts ws://0.0.0.0:<port>
// - Broadcast text frames to all clients
// - Receives {"type":"history","last":N} and responds with {"type":"history","items":[...]}
// - Sends {"type":"status","status":"online"} upon connect

#include "ws_server.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define HTTP_MAX 8192
#define WS_MAX_PAYLOAD (1024 * 1024) // 1MB

// ---------------- SHA1 (public domain style minimal) ----------------

typedef struct {
    uint32_t state[5];
    uint64_t count;     // bits
    uint8_t buffer[64];
} sha1_ctx_t;

static uint32_t rol32(uint32_t x, uint32_t n) { return (x << n) | (x >> (32 - n)); }

static void sha1_transform(uint32_t state[5], const uint8_t block[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4 + 0] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8)  |
               ((uint32_t)block[i * 4 + 3]);
    }
    for (int i = 16; i < 80; i++) {
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];

    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }

        uint32_t temp = rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol32(b, 30);
        b = a;
        a = temp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

static void sha1_init(sha1_ctx_t *ctx) {
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->count = 0;
    memset(ctx->buffer, 0, sizeof(ctx->buffer));
}

static void sha1_update(sha1_ctx_t *ctx, const uint8_t *data, size_t len) {
    size_t idx = (size_t)((ctx->count >> 3) & 63);
    ctx->count += (uint64_t)len << 3;

    size_t part = 64 - idx;
    size_t i = 0;

    if (len >= part) {
        memcpy(&ctx->buffer[idx], &data[0], part);
        sha1_transform(ctx->state, ctx->buffer);
        i = part;
        while (i + 63 < len) {
            sha1_transform(ctx->state, &data[i]);
            i += 64;
        }
        idx = 0;
    }

    if (i < len) {
        memcpy(&ctx->buffer[idx], &data[i], len - i);
    }
}

static void sha1_final(sha1_ctx_t *ctx, uint8_t out[20]) {
    uint8_t pad[64];
    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;

    uint8_t len_be[8];
    uint64_t bits = ctx->count;
    for (int i = 0; i < 8; i++) {
        len_be[7 - i] = (uint8_t)(bits & 0xFF);
        bits >>= 8;
    }

    size_t idx = (size_t)((ctx->count >> 3) & 63);
    size_t pad_len = (idx < 56) ? (56 - idx) : (120 - idx);

    sha1_update(ctx, pad, pad_len);
    sha1_update(ctx, len_be, 8);

    for (int i = 0; i < 5; i++) {
        out[i * 4 + 0] = (uint8_t)((ctx->state[i] >> 24) & 0xFF);
        out[i * 4 + 1] = (uint8_t)((ctx->state[i] >> 16) & 0xFF);
        out[i * 4 + 2] = (uint8_t)((ctx->state[i] >> 8) & 0xFF);
        out[i * 4 + 3] = (uint8_t)((ctx->state[i]) & 0xFF);
    }
}

// ---------------- Base64 ----------------

static int base64_encode(const uint8_t *in, size_t inlen, char *out, size_t outcap) {
    static const char b64_tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    size_t olen = 4 * ((inlen + 2) / 3);
    if (outcap < olen + 1) return -1;

    size_t i = 0, j = 0;
    while (i < inlen) {
        size_t rem = inlen - i;

        uint32_t octet_a = in[i++];
        uint32_t octet_b = (rem > 1) ? in[i++] : 0;
        uint32_t octet_c = (rem > 2) ? in[i++] : 0;

        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        out[j++] = b64_tbl[(triple >> 18) & 0x3F];
        out[j++] = b64_tbl[(triple >> 12) & 0x3F];
        out[j++] = (rem > 1) ? b64_tbl[(triple >> 6) & 0x3F] : '=';
        out[j++] = (rem > 2) ? b64_tbl[triple & 0x3F] : '=';
    }

    out[j] = '\0';
    return (int)j;
}

// ---------------- Small helpers ----------------

static int set_timeouts(int fd, int ms) {
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) return -1;
    return 0;
}

static int env_int_local(const char *name, int defv, int minv, int maxv) {
    const char *v = getenv(name);
    if (!v || !v[0]) return defv;
    long x = strtol(v, NULL, 10);
    if (x < (long)minv) x = (long)minv;
    if (x > (long)maxv) x = (long)maxv;
    return (int)x;
}

static void env_str_local(const char *name, char *dst, size_t cap, const char *fallback) {
    const char *v = getenv(name);
    const char *src = (v && v[0]) ? v : fallback;
    if (!src) src = "";
    if (cap == 0) return;
    snprintf(dst, cap, "%s", src);
}

static int64_t now_monotonic_ns_local(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static int sleep_ms_interruptible(int ms) {
    if (ms <= 0) return 0;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0) {
        if (errno != EINTR) return -1;
    }
    return 0;
}

static int recv_all(int fd, void *buf, size_t n) {
    uint8_t *p = (uint8_t*)buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, p + got, n - got, 0);
        if (r == 0) return -1;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        got += (size_t)r;
    }
    return 0;
}

static int send_all(int fd, const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t*)buf;
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = send(fd, p + sent, n - sent, MSG_NOSIGNAL);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            return -1;
        }
        sent += (size_t)w;
    }
    return 0;
}

static int icase_starts_with(const char *s, const char *prefix) {
    while (*prefix && *s) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return 0;
        s++; prefix++;
    }
    return *prefix == '\0';
}

static char *find_header_value(char *http, const char *name) {
    // returns pointer to value (in-place), null-terminated by trimming CRLF
    size_t nlen = strlen(name);
    char *p = http;
    while (p && *p) {
        char *line_end = strstr(p, "\r\n");
        if (!line_end) break;
        if (line_end == p) break;

        if ((size_t)(line_end - p) > nlen + 1 && icase_starts_with(p, name) && p[nlen] == ':') {
            char *v = p + nlen + 1;
            while (*v == ' ' || *v == '\t') v++;
            *line_end = '\0';
            return v;
        }
        p = line_end + 2;
    }
    return NULL;
}

static int read_http_request(int fd, char *buf, size_t cap, size_t *out_len) {
    size_t len = 0;
    while (len + 1 < cap) {
        ssize_t r = recv(fd, buf + len, cap - len - 1, 0);
        if (r == 0) return -1;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        len += (size_t)r;
        buf[len] = '\0';
        if (strstr(buf, "\r\n\r\n")) {
            if (out_len) *out_len = len;
            return 0;
        }
        if (len >= cap - 1) break;
    }
    return -1;
}

static int compute_accept_key(const char *sec_key, char out_b64[64]) {
    char cat[256];
    snprintf(cat, sizeof(cat), "%s%s", sec_key, WS_GUID);

    sha1_ctx_t ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, (const uint8_t*)cat, strlen(cat));
    uint8_t digest[20];
    sha1_final(&ctx, digest);

    if (base64_encode(digest, 20, out_b64, 64) < 0) return -1;
    return 0;
}

// ---------------- WebSocket framing ----------------

static int ws_send_text(int fd, const char *msg, size_t len) {
    uint8_t hdr[14];
    size_t hlen = 0;

    hdr[0] = 0x81; // FIN=1, opcode=1
    if (len <= 125) {
        hdr[1] = (uint8_t)len;
        hlen = 2;
    } else if (len <= 65535) {
        hdr[1] = 126;
        hdr[2] = (uint8_t)((len >> 8) & 0xFF);
        hdr[3] = (uint8_t)(len & 0xFF);
        hlen = 4;
    } else {
        hdr[1] = 127;
        uint64_t L = (uint64_t)len;
        for (int i = 0; i < 8; i++) hdr[2 + i] = (uint8_t)((L >> (56 - 8 * i)) & 0xFF);
        hlen = 10;
    }

    if (send_all(fd, hdr, hlen) != 0) return -1;
    if (len > 0 && send_all(fd, msg, len) != 0) return -1;
    return 0;
}

static int ws_send_control(int fd, uint8_t opcode, const uint8_t *payload, size_t len) {
    if (len > 125) len = 125;
    uint8_t hdr[2];
    hdr[0] = (uint8_t)(0x80 | (opcode & 0x0F));
    hdr[1] = (uint8_t)(len & 0x7F);
    if (send_all(fd, hdr, 2) != 0) return -1;
    if (len && payload && send_all(fd, payload, len) != 0) return -1;
    return 0;
}

static int ws_recv_frame(int fd, uint8_t *out_opcode, uint8_t **out_payload, size_t *out_len) {
    uint8_t h2[2];
    if (recv_all(fd, h2, 2) != 0) return -1;

    uint8_t fin = (h2[0] >> 7) & 1;
    uint8_t opcode = h2[0] & 0x0F;
    uint8_t masked = (h2[1] >> 7) & 1;
    uint64_t len = (uint64_t)(h2[1] & 0x7F);

    (void)fin; // MVP: ignore fragmentation, expect FIN=1

    if (len == 126) {
        uint8_t ext[2];
        if (recv_all(fd, ext, 2) != 0) return -1;
        len = ((uint64_t)ext[0] << 8) | (uint64_t)ext[1];
    } else if (len == 127) {
        uint8_t ext[8];
        if (recv_all(fd, ext, 8) != 0) return -1;
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | ext[i];
    }

    if (len > WS_MAX_PAYLOAD) return -1;

    uint8_t mask[4] = {0};
    if (masked) {
        if (recv_all(fd, mask, 4) != 0) return -1;
    }

    uint8_t *payload = NULL;
    if (len > 0) {
        payload = (uint8_t*)malloc((size_t)len + 1);
        if (!payload) return -1;
        if (recv_all(fd, payload, (size_t)len) != 0) {
            free(payload);
            return -1;
        }
        if (masked) {
            for (uint64_t i = 0; i < len; i++) {
                payload[i] ^= mask[i % 4];
            }
        }
        payload[len] = '\0';
    } else {
        payload = (uint8_t*)malloc(1);
        if (!payload) return -1;
        payload[0] = '\0';
    }

    *out_opcode = opcode;
    *out_payload = payload;
    *out_len = (size_t)len;
    return 0;
}

// ---------------- Server / Clients ----------------

typedef struct ws_client {
    int fd;
    pthread_t th;
    struct ws_client *next;
    struct ws_server *srv;
    volatile int alive;
} ws_client_t;

struct ws_server {
    int port;
    int server_fd;
    pthread_t accept_th;
    volatile int running;

    pthread_mutex_t mu;
    ws_client_t *clients;

    db_reader_t *db; // not owned

    int delay_ms;
    int jitter_ms;
    int drop_pct;
    int cut_start_s;
    int cut_duration_s;
    int reconnect_mode_close;

    int64_t start_mono_ns;
    int experiment_anchor_set;

    int cut_close_fired;
    uint64_t dropped_msgs;
    uint64_t delayed_msgs;
    uint64_t cut_msgs;
    char exp_tag[64];
};

static int impairment_cut_active(ws_server_t *s) {
    if (!s) return 0;
    if (s->cut_duration_s <= 0) return 0;
    if (!s->experiment_anchor_set) return 0;

    int64_t elapsed_ns = now_monotonic_ns_local() - s->start_mono_ns;
    if (elapsed_ns < 0) elapsed_ns = 0;

    int64_t start_ns = (int64_t)s->cut_start_s * 1000000000LL;
    int64_t end_ns = start_ns + (int64_t)s->cut_duration_s * 1000000000LL;
    return (elapsed_ns >= start_ns && elapsed_ns < end_ns) ? 1 : 0;
}

static int impairment_should_drop(ws_server_t *s) {
    if (!s || s->drop_pct <= 0) return 0;
    int r = rand() % 100;
    return r < s->drop_pct;
}

static int impairment_delay_for_msg_ms(ws_server_t *s) {
    if (!s) return 0;
    int delay = s->delay_ms;
    if (s->jitter_ms > 0) {
        int span = s->jitter_ms * 2 + 1;
        int off = (rand() % span) - s->jitter_ms;
        delay += off;
        if (delay < 0) delay = 0;
    }
    return delay;
}

static void ws_close_all_clients(ws_server_t *s) {
    if (!s) return;
    ws_client_t *c = s->clients;
    while (c) {
        c->alive = 0;
        if (c->fd >= 0) {
            shutdown(c->fd, SHUT_RDWR);
            close(c->fd);
            c->fd = -1;
        }
        c = c->next;
    }
}

static void client_remove(ws_server_t *s, ws_client_t *c) {
    pthread_mutex_lock(&s->mu);
    ws_client_t **pp = &s->clients;
    while (*pp) {
        if (*pp == c) {
            *pp = c->next;
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&s->mu);
}

static int ws_handshake(int fd) {
    char http[HTTP_MAX];
    size_t hlen = 0;
    if (read_http_request(fd, http, sizeof(http), &hlen) != 0) return -1;

    // Basic validation
    if (strncmp(http, "GET ", 4) != 0) return -1;

    // Find Sec-WebSocket-Key
    char *key = find_header_value(http, "Sec-WebSocket-Key");
    if (!key || !key[0]) return -1;

    char accept[64];
    if (compute_accept_key(key, accept) != 0) return -1;

    char resp[256];
    int n = snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n",
        accept);

    if (n <= 0 || (size_t)n >= sizeof(resp)) return -1;
    if (send_all(fd, resp, (size_t)n) != 0) return -1;

    return 0;
}

static int json_find_int(const char *json, const char *key, int defv) {
    // naive parse: looks for "key": <int>
    if (!json || !key) return defv;
    const char *p = strstr(json, key);
    if (!p) return defv;
    p = strchr(p, ':');
    if (!p) return defv;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    int sign = 1;
    if (*p == '-') { sign = -1; p++; }
    if (!isdigit((unsigned char)*p)) return defv;
    long v = 0;
    while (isdigit((unsigned char)*p)) {
        v = v * 10 + (*p - '0');
        p++;
        if (v > 1000000) break;
    }
    v *= sign;
    return (int)v;
}

static int is_history_request(const char *json) {
    if (!json) return 0;
    // accept: {"type":"history"...} or {"type": "history"...}
    const char *p = strstr(json, "\"type\"");
    if (!p) p = strstr(json, "type");
    if (!p) return 0;
    const char *h = strstr(p, "history");
    return h != NULL;
}

static void *client_thread(void *arg) {
    ws_client_t *c = (ws_client_t*)arg;
    ws_server_t *s = c->srv;

    if (ws_handshake(c->fd) != 0) {
        c->alive = 0;
        if (c->fd >= 0) {
            close(c->fd);
            c->fd = -1;
        }
        client_remove(s, c);
        free(c);
        return NULL;
    }

    // anchor experiment time to the first successful UI connection
    pthread_mutex_lock(&s->mu);
    if (!s->experiment_anchor_set) {
        s->start_mono_ns = now_monotonic_ns_local();
        s->experiment_anchor_set = 1;
        fprintf(stderr, "[WS][%s] experiment anchor set on first client connect\n", s->exp_tag);
    }
    pthread_mutex_unlock(&s->mu);

    // send status including exp_tag so the UI can use it as run_tag
    char status[256];
    int sn = snprintf(status, sizeof(status),
                      "{\"type\":\"status\",\"status\":\"online\",\"exp_tag\":\"%s\"}",
                      s->exp_tag);
    if (sn > 0 && (size_t)sn < sizeof(status)) {
        ws_send_text(c->fd, status, (size_t)sn);
    }

    // loop frames
    while (s->running && c->alive) {
        uint8_t opcode = 0;
        uint8_t *payload = NULL;
        size_t plen = 0;

        if (ws_recv_frame(c->fd, &opcode, &payload, &plen) != 0) {
            free(payload);
            break;
        }

        if (opcode == 0x8) { // close
            free(payload);
            ws_send_control(c->fd, 0x8, NULL, 0);
            break;
        } else if (opcode == 0x9) { // ping
            ws_send_control(c->fd, 0xA, payload, plen);
            free(payload);
            continue;
        } else if (opcode == 0xA) { // pong
            free(payload);
            continue;
        } else if (opcode != 0x1) { // only text
            free(payload);
            continue;
        }

        // text
        const char *txt = (const char*)payload;

        if (is_history_request(txt)) {
            int last = json_find_int(txt, "\"last\"", 300);
            char *json = NULL;
            size_t jlen = 0;

            if (!s->db) {
                const char *err = "{\"type\":\"error\",\"message\":\"db_not_available\"}";
                ws_send_text(c->fd, err, strlen(err));
            } else if (db_reader_history_last_json(s->db, last, &json, &jlen) == 0) {
                ws_send_text(c->fd, json, jlen);
                free(json);
            } else {
                const char *err = "{\"type\":\"error\",\"message\":\"history_query_failed\"}";
                ws_send_text(c->fd, err, strlen(err));
            }
        }

        free(payload);
    }

    c->alive = 0;
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    client_remove(s, c);
    free(c);
    return NULL;
}

static void *accept_thread(void *arg) {
    ws_server_t *s = (ws_server_t*)arg;

    while (s->running) {
        struct sockaddr_in cli;
        socklen_t slen = sizeof(cli);
        int cfd = accept(s->server_fd, (struct sockaddr*)&cli, &slen);
        if (cfd < 0) {
            if (!s->running) break;
            if (errno == EINTR) continue;
            // accept may fail transiently
            usleep(50000);
            continue;
        }

        set_timeouts(cfd, 60000);

        ws_client_t *c = (ws_client_t*)calloc(1, sizeof(*c));
        if (!c) {
            close(cfd);
            continue;
        }

        c->fd = cfd;
        c->srv = s;
        c->alive = 1;

        pthread_mutex_lock(&s->mu);
        c->next = s->clients;
        s->clients = c;
        pthread_mutex_unlock(&s->mu);

        if (pthread_create(&c->th, NULL, client_thread, c) != 0) {
            c->alive = 0;
            close(cfd);
            client_remove(s, c);
            free(c);
            continue;
        }
        pthread_detach(c->th);
    }

    return NULL;
}

int ws_server_start(ws_server_t **out, int port, db_reader_t *db_reader) {
    if (!out) return -1;
    *out = NULL;

    ws_server_t *s = (ws_server_t*)calloc(1, sizeof(*s));
    if (!s) return -1;

    s->port = port;
    s->db = db_reader;
    s->running = 1;
    pthread_mutex_init(&s->mu, NULL);

    s->delay_ms = env_int_local("WS_DELAY_MS", 0, 0, 60000);
    s->jitter_ms = env_int_local("WS_JITTER_MS", 0, 0, 60000);
    s->drop_pct = env_int_local("WS_DROP_PCT", 0, 0, 100);
    s->cut_start_s = env_int_local("WS_CUT_START_S", 0, 0, 86400);
    s->cut_duration_s = env_int_local("WS_CUT_DURATION_S", 0, 0, 86400);
    {
        char reconnect_mode[32];
        env_str_local("WS_RECONNECT_MODE", reconnect_mode, sizeof(reconnect_mode), "close");
        s->reconnect_mode_close = (strcmp(reconnect_mode, "close") == 0) ? 1 : 0;
    }
    env_str_local("WS_EXP_TAG", s->exp_tag, sizeof(s->exp_tag), "default");
    s->start_mono_ns = 0;
    s->experiment_anchor_set = 0;
    s->cut_close_fired = 0;

    srand((unsigned)(time(NULL) ^ (unsigned)port));

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        free(s);
        return -2;
    }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "[WS] bind failed: %s\n", strerror(errno));
        close(fd);
        pthread_mutex_destroy(&s->mu);
        free(s);
        return -3;
    }

    if (listen(fd, 16) != 0) {
        fprintf(stderr, "[WS] listen failed: %s\n", strerror(errno));
        close(fd);
        pthread_mutex_destroy(&s->mu);
        free(s);
        return -4;
    }

    s->server_fd = fd;
    set_timeouts(fd, 60000);

    if (pthread_create(&s->accept_th, NULL, accept_thread, s) != 0) {
        close(fd);
        pthread_mutex_destroy(&s->mu);
        free(s);
        return -5;
    }

    fprintf(stderr,
            "[WS] exp tag=%s delay_ms=%d jitter_ms=%d drop_pct=%d cut_start_s=%d cut_duration_s=%d reconnect_mode=%s\n",
            s->exp_tag, s->delay_ms, s->jitter_ms, s->drop_pct,
            s->cut_start_s, s->cut_duration_s,
            s->reconnect_mode_close ? "close" : "drop");

    *out = s;
    return 0;
}

void ws_server_stop(ws_server_t *s) {
    if (!s) return;

    s->running = 0;

    if (s->server_fd > 0) {
        close(s->server_fd);
        s->server_fd = -1;
    }

    if (s->accept_th) pthread_join(s->accept_th, NULL);

    // close all clients
    pthread_mutex_lock(&s->mu);
    ws_client_t *c = s->clients;
    s->clients = NULL;
    pthread_mutex_unlock(&s->mu);

    while (c) {
        ws_client_t *n = c->next;
        c->alive = 0;
        if (c->fd >= 0) close(c->fd);
        free(c);
        c = n;
    }

    pthread_mutex_destroy(&s->mu);
    free(s);
}

int ws_server_broadcast(ws_server_t *s, const char *msg, size_t len) {
    if (!s || !msg) return -1;

    pthread_mutex_lock(&s->mu);

    if (impairment_cut_active(s)) {
        s->cut_msgs++;
        if (s->reconnect_mode_close && !s->cut_close_fired) {
            fprintf(stderr, "[WS][%s] cut active: closing all clients\n", s->exp_tag);
            ws_close_all_clients(s);
            s->cut_close_fired = 1;
        }
        pthread_mutex_unlock(&s->mu);
        return 0;
    }

    if (s->cut_close_fired) {
        fprintf(stderr, "[WS][%s] cut window ended\n", s->exp_tag);
        s->cut_close_fired = 0;
    }

    if (impairment_should_drop(s)) {
        s->dropped_msgs++;
        pthread_mutex_unlock(&s->mu);
        return 0;
    }

    int delay_ms = impairment_delay_for_msg_ms(s);
    if (delay_ms > 0) {
        s->delayed_msgs++;
        pthread_mutex_unlock(&s->mu);
        sleep_ms_interruptible(delay_ms);
        pthread_mutex_lock(&s->mu);
    }

    ws_client_t **pp = &s->clients;
    while (*pp) {
        ws_client_t *c = *pp;
        if (!c->alive) {
            *pp = c->next;
            if (c->fd >= 0) close(c->fd);
            free(c);
            continue;
        }

        if (ws_send_text(c->fd, msg, len) != 0) {
            c->alive = 0;
            *pp = c->next;
            if (c->fd >= 0) close(c->fd);
            free(c);
            continue;
        }

        pp = &(*pp)->next;
    }

    pthread_mutex_unlock(&s->mu);
    return 0;
}