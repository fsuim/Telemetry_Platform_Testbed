#include "imu_sensor.h"
#include "../mqtt_client.h"
#include "../pack_le.h"
#include "../time_utils.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TOPIC_IMU_ACC  "/robot/sensors/imu0/acc"
#define TOPIC_IMU_GYRO "/robot/sensors/imu0/gyro"

static const double ACC_LSB_PER_G    = 5461.0; // ±6g
static const double GYRO_LSB_PER_DPS = 262.4;  // ±125 dps

typedef struct {
    sensor_runtime_t rt;
    char client_id[64];
    telemetry_cache_t *cache;
} imu_args_t;

static double frand_unit(unsigned int *seed){
    // [0,1)
    return (double)rand_r(seed) / ((double)RAND_MAX + 1.0);
}

static double noise(unsigned int *seed, double sigma){
    // ruído ~ uniforme aproximado
    return sigma * (frand_unit(seed) - 0.5) * 2.0;
}

/*static int16_t clamp_i16(long v){
    if(v > 32767) return 32767;
    if(v < -32768) return -32768;
    return (int16_t)v;
}*/

static void *imu_thread(void *arg){
    imu_args_t *a = (imu_args_t*)arg;

    mqtt_client_t cli;
    if(mqtt_client_connect(&cli, a->client_id, a->rt.host, a->rt.port) != 0){
        fprintf(stderr, "[IMU] failed to connect MQTT\n");
        free(a);
        return NULL;
    }

    uint64_t period_ns = (uint64_t)(1e9 / a->rt.rate_hz);
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)getpid();
    double t = 0.0;

    while(*(a->rt.running)){
        // aceleração em g (fake)
        double ax_g = 0.2 * sin(2.0 * M_PI * 0.50 * t) + noise(&seed, 0.02);
        double ay_g = 0.2 * cos(2.0 * M_PI * 0.40 * t) + noise(&seed, 0.02);
        double az_g = 1.0 + 0.05 * sin(2.0 * M_PI * 0.20 * t) + noise(&seed, 0.02);

        int16_t ax = clamp_i16(lround(ax_g * ACC_LSB_PER_G));
        int16_t ay = clamp_i16(lround(ay_g * ACC_LSB_PER_G));
        int16_t az = clamp_i16(lround(az_g * ACC_LSB_PER_G));

        uint8_t payload_acc[6];
        pack_i16_le(&payload_acc[0], ax);
        pack_i16_le(&payload_acc[2], ay);
        pack_i16_le(&payload_acc[4], az);

        int rc = mosquitto_publish(cli.mosq, NULL, TOPIC_IMU_ACC, (int)sizeof(payload_acc), payload_acc, 0, false);
        if(rc != MOSQ_ERR_SUCCESS){
            fprintf(stderr, "[IMU] publish acc failed: %s\n", mosquitto_strerror(rc));
        }

        // gyro em dps (fake)
        double gx_dps = 10.0 * sin(2.0 * M_PI * 0.30 * t) + noise(&seed, 0.5);
        double gy_dps =  8.0 * cos(2.0 * M_PI * 0.22 * t) + noise(&seed, 0.5);
        double gz_dps =  5.0 * sin(2.0 * M_PI * 0.15 * t) + noise(&seed, 0.5);

        // State Protobuf item 2.4: atualiza cache em unidades reais
        if(a->cache){
            telemetry_cache_set_imu(a->cache,
                                    (float)ax_g, (float)ay_g, (float)az_g,
                                    (float)gx_dps, (float)gy_dps, (float)gz_dps);
        }

        int16_t gx = clamp_i16(lround(gx_dps * GYRO_LSB_PER_DPS));
        int16_t gy = clamp_i16(lround(gy_dps * GYRO_LSB_PER_DPS));
        int16_t gz = clamp_i16(lround(gz_dps * GYRO_LSB_PER_DPS));

        uint8_t payload_gyro[6];
        pack_i16_le(&payload_gyro[0], gx);
        pack_i16_le(&payload_gyro[2], gy);
        pack_i16_le(&payload_gyro[4], gz);

        rc = mosquitto_publish(cli.mosq, NULL, TOPIC_IMU_GYRO, (int)sizeof(payload_gyro), payload_gyro, 0, false);
        if(rc != MOSQ_ERR_SUCCESS){
            fprintf(stderr, "[IMU] publish gyro failed: %s\n", mosquitto_strerror(rc));
        }

        sleep_until_next_tick(&next, period_ns);
        t += 1.0 / a->rt.rate_hz;
    }

    mqtt_client_close(&cli);
    free(a);
    return NULL;
}

int imu_sensor_start(const sensor_runtime_t *rt, telemetry_cache_t *cache, pthread_t *out_thread){
    imu_args_t *a = (imu_args_t *)calloc(1, sizeof(*a));
    if(!a) return -1;
    a->rt = *rt;
    a->cache = cache;

    // client id único
    snprintf(a->client_id, sizeof(a->client_id), "fake-imu-%d-%ld", getpid(), (long)time(NULL));

    if(pthread_create(out_thread, NULL, imu_thread, a) != 0){
        free(a);
        return -2;
    }
    return 0;
}