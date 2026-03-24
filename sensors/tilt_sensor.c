#include "tilt_sensor.h"
#include "../mqtt_client.h"
#include "../pack_le.h"
#include "../time_utils.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TOPIC_TILT "/robot/sensors/tilt0/tilt"

static const double TILT_LSB_PER_DEG = 182.0;

typedef struct {
    sensor_runtime_t rt;
    char client_id[64];
    telemetry_cache_t *cache;
} tilt_args_t;

static double frand_unit(unsigned int *seed){
    return (double)rand_r(seed) / ((double)RAND_MAX + 1.0);
}

static double noise(unsigned int *seed, double sigma){
    return sigma * (frand_unit(seed) - 0.5) * 2.0;
}

/*static int16_t clamp_i16(long v){
    if(v > 32767) return 32767;
    if(v < -32768) return -32768;
    return (int16_t)v;
}*/

static void *tilt_thread(void *arg){
    tilt_args_t *a = (tilt_args_t*)arg;

    mqtt_client_t cli;
    if(mqtt_client_connect(&cli, a->client_id, a->rt.host, a->rt.port) != 0){
        fprintf(stderr, "[TILT] failed to connect MQTT\n");
        free(a);
        return NULL;
    }

    uint64_t period_ns = (uint64_t)(1e9 / a->rt.rate_hz);
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)getpid();
    double t = 0.0;

    while(*(a->rt.running)){
        double tx_deg = 20.0 * sin(2.0 * M_PI * 0.10 * t) + noise(&seed, 0.2);
        double ty_deg = 15.0 * cos(2.0 * M_PI * 0.08 * t) + noise(&seed, 0.2);
        double tz_deg =  5.0 * sin(2.0 * M_PI * 0.05 * t) + noise(&seed, 0.2);

        int16_t tx = clamp_i16(lround(tx_deg * TILT_LSB_PER_DEG));
        int16_t ty = clamp_i16(lround(ty_deg * TILT_LSB_PER_DEG));
        int16_t tz = clamp_i16(lround(tz_deg * TILT_LSB_PER_DEG));

        uint16_t status = 0;

        // State Protobuf item 2.4: atualiza cache em unidades reais
        if(a->cache){
            telemetry_cache_set_tilt(a->cache,
                                     (float)tx_deg, (float)ty_deg, (float)tz_deg,
                                     (uint32_t)status);
        }

        uint8_t payload[8];
        pack_i16_le(&payload[0], tx);
        pack_i16_le(&payload[2], ty);
        pack_i16_le(&payload[4], tz);
        pack_u16_le(&payload[6], status);

        int rc = mosquitto_publish(cli.mosq, NULL, TOPIC_TILT, (int)sizeof(payload), payload, 0, false);
        if(rc != MOSQ_ERR_SUCCESS){
            fprintf(stderr, "[TILT] publish failed: %s\n", mosquitto_strerror(rc));
        }

        sleep_until_next_tick(&next, period_ns);
        t += 1.0 / a->rt.rate_hz;
    }

    mqtt_client_close(&cli);
    free(a);
    return NULL;
}

int tilt_sensor_start(const sensor_runtime_t *rt, telemetry_cache_t *cache, pthread_t *out_thread){
    tilt_args_t *a = (tilt_args_t *)calloc(1, sizeof(*a));
    if(!a) return -1;
    a->rt = *rt;
    a->cache = cache;

    snprintf(a->client_id, sizeof(a->client_id), "fake-tilt-%d-%ld", getpid(), (long)time(NULL));

    if(pthread_create(out_thread, NULL, tilt_thread, a) != 0){
        free(a);
        return -2;
    }
    return 0;
}