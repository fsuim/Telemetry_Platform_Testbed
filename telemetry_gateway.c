#include "telemetry_gateway.h"

#include "db_sqlite.h"
#include "mqtt_client.h"
#include "telemetry_types.h"
#include "ws_server.h"

#include "robot_telemetry.pb-c.h"

#include <mosquitto.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TOPIC_TELEMETRY_STATE "/robot/v1/telemetry/state"

typedef struct qnode {
    telemetry_state_t s;
    struct qnode *next;
} qnode_t;

struct telemetry_gateway {
    volatile int running;

    mqtt_client_t mqtt;

    db_writer_t dbw;
    db_reader_t dbr;

    ws_server_t *ws;

    pthread_t writer_th;
    pthread_t stats_th;

    pthread_mutex_t q_mu;
    pthread_cond_t  q_cv;
    qnode_t *q_head;
    qnode_t *q_tail;
    size_t q_len;

    // Experiment knobs
    int batch_size;
    int stats_enabled;
    int stats_interval_ms;

    // Stats (protected by stats_mu)
    pthread_mutex_t stats_mu;
    int64_t start_mono_ns;
    uint64_t rx_msgs;
    uint64_t rx_bytes;
    uint64_t db_rows;
    uint64_t commit_count;
    int64_t  commit_ns_sum;
    int64_t  commit_ns_max;
    size_t   q_len_max;
};

static int64_t now_realtime_ns(void){
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static int64_t now_monotonic_ns(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static int env_int(const char *name, int def, int minv, int maxv){
    const char *v = getenv(name);
    if(!v || !v[0]) return def;
    long x = strtol(v, NULL, 10);
    if(x < (long)minv) x = (long)minv;
    if(x > (long)maxv) x = (long)maxv;
    return (int)x;
}

static void safe_copy(char *dst, size_t cap, const char *src, const char *fallback){
    if(!dst || cap == 0) return;
    const char *s = (src && src[0]) ? src : fallback;
    if(!s) s = "";
    snprintf(dst, cap, "%s", s);
}

static int telemetry_state_to_json_alloc(const telemetry_state_t *s, char **out, size_t *out_len){
    if(!s || !out || !out_len) return -1;

    double t_sec = (double)s->received_at_ns / 1e9;

    const char *fmt =
        "{\"type\":\"telemetry\",\"t\":%.6f,\"t_ns\":%lld,\"seq\":%u,\"msg_ts_ns\":%llu,"
        "\"robot_id\":\"%s\",\"imu_id\":\"%s\",\"tilt_id\":\"%s\",\"tilt_status\":%u,"
        "\"accel\":{\"x\":%.6f,\"y\":%.6f,\"z\":%.6f},"
        "\"gyro\":{\"x\":%.6f,\"y\":%.6f,\"z\":%.6f},"
        "\"tilt\":{\"x\":%.6f,\"y\":%.6f,\"z\":%.6f},"
        "\"motors\":{"
          "\"motor1\":{\"id\":\"%s\",\"tics\":%u,\"rpm\":%d,\"temperature_c\":%d,\"voltage_power_stage_mv\":%d,\"current_power_stage_ma\":%d},"
          "\"motor2\":{\"id\":\"%s\",\"tics\":%u,\"rpm\":%d,\"temperature_c\":%d,\"voltage_power_stage_mv\":%d,\"current_power_stage_ma\":%d}"
        "}}";

    int n = snprintf(NULL, 0, fmt,
        t_sec, (long long)s->received_at_ns, (unsigned)s->seq, (unsigned long long)s->msg_timestamp_ns,
        s->robot_id, s->imu_id, s->tilt_id, (unsigned)s->tilt_status,
        s->acc_g[0], s->acc_g[1], s->acc_g[2],
        s->gyro_dps[0], s->gyro_dps[1], s->gyro_dps[2],
        s->tilt_deg[0], s->tilt_deg[1], s->tilt_deg[2],
        s->motor_id[0], (unsigned)s->motor_tics[0], (int)s->motor_rpm[0], (int)s->motor_temperature_c[0],
        (int)s->motor_voltage_power_stage_mv[0], (int)s->motor_current_power_stage_ma[0],
        s->motor_id[1], (unsigned)s->motor_tics[1], (int)s->motor_rpm[1], (int)s->motor_temperature_c[1],
        (int)s->motor_voltage_power_stage_mv[1], (int)s->motor_current_power_stage_ma[1]);

    if(n < 0) return -2;

    size_t need = (size_t)n;
    char *buf = (char*)malloc(need + 1);
    if(!buf) return -3;

    snprintf(buf, need + 1, fmt,
        t_sec, (long long)s->received_at_ns, (unsigned)s->seq, (unsigned long long)s->msg_timestamp_ns,
        s->robot_id, s->imu_id, s->tilt_id, (unsigned)s->tilt_status,
        s->acc_g[0], s->acc_g[1], s->acc_g[2],
        s->gyro_dps[0], s->gyro_dps[1], s->gyro_dps[2],
        s->tilt_deg[0], s->tilt_deg[1], s->tilt_deg[2],
        s->motor_id[0], (unsigned)s->motor_tics[0], (int)s->motor_rpm[0], (int)s->motor_temperature_c[0],
        (int)s->motor_voltage_power_stage_mv[0], (int)s->motor_current_power_stage_ma[0],
        s->motor_id[1], (unsigned)s->motor_tics[1], (int)s->motor_rpm[1], (int)s->motor_temperature_c[1],
        (int)s->motor_voltage_power_stage_mv[1], (int)s->motor_current_power_stage_ma[1]);

    *out = buf;
    *out_len = need;
    return 0;
}

static void queue_push(telemetry_gateway_t *g, telemetry_state_t *s){
    qnode_t *n = (qnode_t*)calloc(1, sizeof(*n));
    if(!n){
        if(s->raw) free(s->raw);
        return;
    }
    n->s = *s; // copia struct (raw pointer ownership transfer)
    n->next = NULL;

    pthread_mutex_lock(&g->q_mu);
    if(g->q_tail) g->q_tail->next = n;
    else g->q_head = n;
    g->q_tail = n;
    g->q_len++;
    if(g->q_len > g->q_len_max) g->q_len_max = g->q_len;
    pthread_cond_signal(&g->q_cv);
    pthread_mutex_unlock(&g->q_mu);
}

static int queue_pop_batch(telemetry_gateway_t *g, telemetry_state_t *out, int max){
    int n = 0;
    pthread_mutex_lock(&g->q_mu);
    while(g->running && g->q_head == NULL){
        pthread_cond_wait(&g->q_cv, &g->q_mu);
    }

    while(g->q_head && n < max){
        qnode_t *q = g->q_head;
        g->q_head = q->next;
        if(!g->q_head) g->q_tail = NULL;
        g->q_len--;

        out[n++] = q->s;
        free(q);
    }

    pthread_mutex_unlock(&g->q_mu);
    return n;
}

static void *writer_thread(void *arg){
    telemetry_gateway_t *g = (telemetry_gateway_t*)arg;

    const int BATCH = (g->batch_size > 0) ? g->batch_size : 50;
    telemetry_state_t *batch = (telemetry_state_t*)calloc((size_t)BATCH, sizeof(*batch));
    if(!batch){
        fprintf(stderr, "[GW] OOM allocating batch=%d; falling back to batch=1\n", BATCH);
    }

    while(g->running){
        telemetry_state_t tmp_one;
        telemetry_state_t *buf = batch ? batch : &tmp_one;
        int maxn = batch ? BATCH : 1;
        int n = queue_pop_batch(g, buf, maxn);
        if(n <= 0) continue;

        int64_t t0 = now_monotonic_ns();
        if(db_writer_begin(&g->dbw) != 0){
            fprintf(stderr, "[GW] DB begin failed (dropping %d samples)\n", n);
            for(int i=0; i<n; i++) if(buf[i].raw) free(buf[i].raw);
            continue;
        }

        int ok = 1;
        for(int i=0; i<n; i++){
            if(db_writer_insert_state(&g->dbw, &buf[i]) != 0){
                ok = 0;
            }
        }

        if(ok) db_writer_commit(&g->dbw);
        else sqlite3_exec(g->dbw.db, "ROLLBACK;", NULL, NULL, NULL);

        int64_t t1 = now_monotonic_ns();
        int64_t dt = (t1 > t0) ? (t1 - t0) : 0;

        pthread_mutex_lock(&g->stats_mu);
        g->db_rows += (uint64_t)n;
        g->commit_count += 1u;
        g->commit_ns_sum += dt;
        if(dt > g->commit_ns_max) g->commit_ns_max = dt;
        pthread_mutex_unlock(&g->stats_mu);

        for(int i=0; i<n; i++){
            char *json = NULL;
            size_t jlen = 0;
            if(telemetry_state_to_json_alloc(&buf[i], &json, &jlen) == 0){
                ws_server_broadcast(g->ws, json, jlen);
                free(json);
            }
            if(buf[i].raw) free(buf[i].raw);
        }
    }

    free(batch);

    return NULL;
}

static void *stats_thread(void *arg){
    telemetry_gateway_t *g = (telemetry_gateway_t*)arg;

    uint64_t last_rx_msgs = 0;
    uint64_t last_rx_bytes = 0;
    uint64_t last_db_rows = 0;
    uint64_t last_commit_count = 0;
    int64_t  last_commit_ns_sum = 0;
    int64_t  last_mono_ns = g->start_mono_ns;

    while(g->running){
        usleep((useconds_t)(g->stats_interval_ms * 1000));
        if(!g->running) break;

        int64_t now_ns = now_monotonic_ns();
        double dt_s = (now_ns > last_mono_ns) ? (double)(now_ns - last_mono_ns) / 1e9 : 1.0;
        if(dt_s <= 0.0) dt_s = 1.0;

        uint64_t rx_msgs, rx_bytes, db_rows, commit_count;
        int64_t commit_ns_sum, commit_ns_max;
        size_t q_len, q_max;

        pthread_mutex_lock(&g->stats_mu);
        rx_msgs = g->rx_msgs;
        rx_bytes = g->rx_bytes;
        db_rows = g->db_rows;
        commit_count = g->commit_count;
        commit_ns_sum = g->commit_ns_sum;
        commit_ns_max = g->commit_ns_max;
        pthread_mutex_unlock(&g->stats_mu);

        pthread_mutex_lock(&g->q_mu);
        q_len = g->q_len;
        q_max = g->q_len_max;
        pthread_mutex_unlock(&g->q_mu);

        uint64_t d_rx_msgs = rx_msgs - last_rx_msgs;
        uint64_t d_rx_bytes = rx_bytes - last_rx_bytes;
        uint64_t d_db_rows = db_rows - last_db_rows;
        uint64_t d_commit = commit_count - last_commit_count;
        int64_t  d_commit_ns_sum = commit_ns_sum - last_commit_ns_sum;

        double rx_rate = (double)d_rx_msgs / dt_s;
        double db_rate = (double)d_db_rows / dt_s;
        double kbps = (double)d_rx_bytes * 8.0 / 1000.0 / dt_s;

        double avg_payload = (rx_msgs > 0) ? ((double)rx_bytes / (double)rx_msgs) : 0.0;
        double avg_commit_ms = (d_commit > 0) ? ((double)d_commit_ns_sum / 1e6 / (double)d_commit) : 0.0;
        double max_commit_ms = (double)commit_ns_max / 1e6;
        double up_s = (double)(now_ns - g->start_mono_ns) / 1e9;

        fprintf(stderr,
            "GWSTAT up=%.1fs rx=%.1f msg/s db=%.1f row/s kbps=%.1f avg_payload=%.1fB avg_commit=%.3fms max_commit=%.3fms qlen=%zu qmax=%zu batch=%d\n",
            up_s, rx_rate, db_rate, kbps, avg_payload, avg_commit_ms, max_commit_ms, q_len, q_max, g->batch_size);

        last_rx_msgs = rx_msgs;
        last_rx_bytes = rx_bytes;
        last_db_rows = db_rows;
        last_commit_count = commit_count;
        last_commit_ns_sum = commit_ns_sum;
        last_mono_ns = now_ns;
    }

    return NULL;
}

static void on_connect(struct mosquitto *mosq, void *ud, int rc){
    (void)ud;
    if(rc != 0){
        fprintf(stderr, "[GW] MQTT connect rc=%d\n", rc);
        return;
    }
    int s_rc = mosquitto_subscribe(mosq, NULL, TOPIC_TELEMETRY_STATE, 0);
    if(s_rc != MOSQ_ERR_SUCCESS){
        fprintf(stderr, "[GW] subscribe failed: %s\n", mosquitto_strerror(s_rc));
    }else{
        fprintf(stderr, "[GW] subscribed %s\n", TOPIC_TELEMETRY_STATE);
    }
}

static void on_message(struct mosquitto *mosq, void *ud, const struct mosquitto_message *msg){
    (void)mosq;
    telemetry_gateway_t *g = (telemetry_gateway_t*)ud;
    if(!g || !g->running || !msg || !msg->topic) return;
    if(strcmp(msg->topic, TOPIC_TELEMETRY_STATE) != 0) return;

    pthread_mutex_lock(&g->stats_mu);
    g->rx_msgs += 1u;
    if(msg->payloadlen > 0) g->rx_bytes += (uint64_t)msg->payloadlen;
    pthread_mutex_unlock(&g->stats_mu);

    Robot__V1__TelemetryState *st = robot__v1__telemetry_state__unpack(
        NULL, (size_t)msg->payloadlen, (const uint8_t*)msg->payload);

    if(!st){
        fprintf(stderr, "[GW] protobuf unpack failed (len=%d)\n", msg->payloadlen);
        return;
    }

    telemetry_state_t s;
    memset(&s, 0, sizeof(s));
    s.received_at_ns = now_realtime_ns();
    s.topic = TOPIC_TELEMETRY_STATE;

    // defaults
    safe_copy(s.robot_id, sizeof(s.robot_id), NULL, "robot");
    safe_copy(s.imu_id, sizeof(s.imu_id), NULL, "imu0");
    safe_copy(s.tilt_id, sizeof(s.tilt_id), NULL, "tilt0");
    safe_copy(s.motor_id[0], sizeof(s.motor_id[0]), NULL, "motor1");
    safe_copy(s.motor_id[1], sizeof(s.motor_id[1]), NULL, "motor2");

    if(st->header){
        s.seq = st->header->seq;
        s.msg_timestamp_ns = st->header->timestamp_ns;
        safe_copy(s.robot_id, sizeof(s.robot_id), st->header->robot_id, "robot");
    }

    safe_copy(s.imu_id, sizeof(s.imu_id), st->imu_id, "imu0");
    safe_copy(s.tilt_id, sizeof(s.tilt_id), st->tilt_id, "tilt0");

    if(st->acc_g){
        s.acc_g[0] = st->acc_g->x;
        s.acc_g[1] = st->acc_g->y;
        s.acc_g[2] = st->acc_g->z;
    }
    if(st->gyro_dps){
        s.gyro_dps[0] = st->gyro_dps->x;
        s.gyro_dps[1] = st->gyro_dps->y;
        s.gyro_dps[2] = st->gyro_dps->z;
    }
    if(st->tilt_deg){
        s.tilt_deg[0] = st->tilt_deg->x;
        s.tilt_deg[1] = st->tilt_deg->y;
        s.tilt_deg[2] = st->tilt_deg->z;
    }
    s.tilt_status = st->tilt_status;

    // Motors
    if(st->motor1){
        safe_copy(s.motor_id[0], sizeof(s.motor_id[0]), st->motor1->motor_id, "motor1");
        s.motor_tics[0] = st->motor1->tics;
        s.motor_rpm[0] = st->motor1->rpm;
        s.motor_temperature_c[0] = st->motor1->temperature_c;
        s.motor_voltage_power_stage_mv[0] = st->motor1->voltage_power_stage_mv;
        s.motor_current_power_stage_ma[0] = st->motor1->current_power_stage_ma;
    }
    if(st->motor2){
        safe_copy(s.motor_id[1], sizeof(s.motor_id[1]), st->motor2->motor_id, "motor2");
        s.motor_tics[1] = st->motor2->tics;
        s.motor_rpm[1] = st->motor2->rpm;
        s.motor_temperature_c[1] = st->motor2->temperature_c;
        s.motor_voltage_power_stage_mv[1] = st->motor2->voltage_power_stage_mv;
        s.motor_current_power_stage_ma[1] = st->motor2->current_power_stage_ma;
    }

    if(msg->payloadlen > 0 && msg->payload){
        s.raw_len = (uint32_t)msg->payloadlen;
        s.raw = (uint8_t*)malloc((size_t)s.raw_len);
        if(s.raw) memcpy(s.raw, msg->payload, (size_t)s.raw_len);
        else s.raw_len = 0;
    }

    robot__v1__telemetry_state__free_unpacked(st, NULL);
    queue_push(g, &s);
}

int telemetry_gateway_start(telemetry_gateway_t **out,
                           const char *mqtt_host, int mqtt_port,
                           const char *db_path,
                           int ws_port){
    if(!out) return -1;

    telemetry_gateway_t *g = (telemetry_gateway_t*)calloc(1, sizeof(*g));
    if(!g) return -1;

    g->running = 1;
    pthread_mutex_init(&g->q_mu, NULL);
    pthread_cond_init(&g->q_cv, NULL);
    pthread_mutex_init(&g->stats_mu, NULL);

    g->batch_size = env_int("GW_BATCH", 50, 1, 4096);
    g->stats_enabled = env_int("GW_STATS", 0, 0, 1);
    g->stats_interval_ms = env_int("GW_STATS_MS", 1000, 50, 60000);
    g->start_mono_ns = now_monotonic_ns();

    if(db_writer_open(&g->dbw, db_path) != 0){
        fprintf(stderr, "[GW] db_writer_open failed\n");
        telemetry_gateway_stop(g);
        return -2;
    }

    if(db_reader_open(&g->dbr, db_path) != 0){
        fprintf(stderr, "[GW] db_reader_open failed\n");
        telemetry_gateway_stop(g);
        return -3;
    }

    if(ws_server_start(&g->ws, ws_port, &g->dbr) != 0){
        fprintf(stderr, "[GW] ws_server_start failed\n");
        telemetry_gateway_stop(g);
        return -4;
    }

    if(pthread_create(&g->writer_th, NULL, writer_thread, g) != 0){
        fprintf(stderr, "[GW] pthread_create(writer) failed\n");
        telemetry_gateway_stop(g);
        return -5;
    }

    if(g->stats_enabled){
        if(pthread_create(&g->stats_th, NULL, stats_thread, g) != 0){
            fprintf(stderr, "[GW] pthread_create(stats) failed; continuing without stats\n");
            g->stats_enabled = 0;
        }
    }

    char cid[64];
    snprintf(cid, sizeof(cid), "telemetry-gw-%d", getpid());

    if(mqtt_client_connect(&g->mqtt, cid, mqtt_host, mqtt_port) != 0){
        fprintf(stderr, "[GW] mqtt connect failed\n");
        telemetry_gateway_stop(g);
        return -6;
    }

    mosquitto_user_data_set(g->mqtt.mosq, g);
    mosquitto_connect_callback_set(g->mqtt.mosq, on_connect);
    mosquitto_message_callback_set(g->mqtt.mosq, on_message);

    mosquitto_subscribe(g->mqtt.mosq, NULL, TOPIC_TELEMETRY_STATE, 0);

    *out = g;
    return 0;
}

void telemetry_gateway_stop(telemetry_gateway_t *g){
    if(!g) return;

    g->running = 0;

    pthread_mutex_lock(&g->q_mu);
    pthread_cond_broadcast(&g->q_cv);
    pthread_mutex_unlock(&g->q_mu);

    if(g->writer_th) pthread_join(g->writer_th, NULL);
    if(g->stats_th) pthread_join(g->stats_th, NULL);

    mqtt_client_close(&g->mqtt);

    if(g->ws) ws_server_stop(g->ws);

    db_reader_close(&g->dbr);
    db_writer_close(&g->dbw);

    pthread_mutex_destroy(&g->q_mu);
    pthread_cond_destroy(&g->q_cv);
    pthread_mutex_destroy(&g->stats_mu);

    qnode_t *q = g->q_head;
    while(q){
        qnode_t *n = q->next;
        if(q->s.raw) free(q->s.raw);
        free(q);
        q = n;
    }

    free(g);
}