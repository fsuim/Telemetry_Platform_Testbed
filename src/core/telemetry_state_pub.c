#include "mqtt_client.h"
#include "sensor_runtime.h"
#include "telemetry_cache.h"

#include "robot_telemetry.pb-c.h"   // gerado pelo protoc-c

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TOPIC_TELEMETRY_STATE "/robot/v1/telemetry/state"

typedef struct {
    sensor_runtime_t rt;
    char client_id[64];

    telemetry_cache_t *cache;

    const char *robot_id;
    const char *imu_id;
    const char *tilt_id;
    const char *motor1_id;
    const char *motor2_id;

    double state_rate_hz; // ex: 20.0
} telemetry_pub_args_t;

static uint64_t now_monotonic_ns(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void *telemetry_pub_thread(void *arg){
    telemetry_pub_args_t *a = (telemetry_pub_args_t *)arg;

    mqtt_client_t cli = (mqtt_client_t){0};
    if(mqtt_client_connect(&cli, a->client_id, a->rt.host, a->rt.port) != 0){
        free(a);
        return NULL;
    }

    if(a->state_rate_hz <= 0.0) a->state_rate_hz = 20.0;

    const long period_ns = (long)llround(1e9 / a->state_rate_hz);
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    while(*(a->rt.running)){
        float acc_g[3] = {0}, gyro_dps[3] = {0}, tilt_deg[3] = {0};
        uint32_t tilt_status = 0, seq = 0;

        uint32_t motor_tics[2] = {0,0};
        int32_t motor_rpm[2] = {0,0};
        int32_t motor_temp_c[2] = {0,0};
        int32_t motor_voltage_mv[2] = {0,0};
        int32_t motor_current_ma[2] = {0,0};

        telemetry_cache_get(a->cache,
                            acc_g, gyro_dps,
                            tilt_deg, &tilt_status,
                            motor_tics,
                            motor_rpm,
                            motor_temp_c,
                            motor_voltage_mv,
                            motor_current_ma,
                            &seq);

        // --- monta protobuf ---
        Robot__V1__TelemetryState msg = ROBOT__V1__TELEMETRY_STATE__INIT;
        Robot__V1__Header hdr = ROBOT__V1__HEADER__INIT;
        Robot__V1__Vec3f acc = ROBOT__V1__VEC3F__INIT;
        Robot__V1__Vec3f gyr = ROBOT__V1__VEC3F__INIT;
        Robot__V1__Vec3f tlt = ROBOT__V1__VEC3F__INIT;
        Robot__V1__MotorState m1 = ROBOT__V1__MOTOR_STATE__INIT;
        Robot__V1__MotorState m2 = ROBOT__V1__MOTOR_STATE__INIT;

        hdr.robot_id = (char*)a->robot_id;
        hdr.timestamp_ns = now_monotonic_ns();
        hdr.seq = seq;

        acc.x = acc_g[0];  acc.y = acc_g[1];  acc.z = acc_g[2];
        gyr.x = gyro_dps[0]; gyr.y = gyro_dps[1]; gyr.z = gyro_dps[2];
        tlt.x = tilt_deg[0]; tlt.y = tilt_deg[1]; tlt.z = tilt_deg[2];

        msg.header = &hdr;

        msg.imu_id = (char*)a->imu_id;
        msg.acc_g = &acc;
        msg.gyro_dps = &gyr;

        msg.tilt_id = (char*)a->tilt_id;
        msg.tilt_deg = &tlt;
        msg.tilt_status = tilt_status;

        m1.motor_id = (char*)a->motor1_id;
        m1.tics = motor_tics[0];
        m1.rpm = motor_rpm[0];
        m1.temperature_c = motor_temp_c[0];
        m1.voltage_power_stage_mv = motor_voltage_mv[0];
        m1.current_power_stage_ma = motor_current_ma[0];

        m2.motor_id = (char*)a->motor2_id;
        m2.tics = motor_tics[1];
        m2.rpm = motor_rpm[1];
        m2.temperature_c = motor_temp_c[1];
        m2.voltage_power_stage_mv = motor_voltage_mv[1];
        m2.current_power_stage_ma = motor_current_ma[1];

        msg.motor1 = &m1;
        msg.motor2 = &m2;

        size_t n = robot__v1__telemetry_state__get_packed_size(&msg);
        uint8_t *buf = (uint8_t*)malloc(n);
        if(!buf){
            fprintf(stderr, "[STATE] malloc failed\n");
            usleep(1000);
            continue;
        }

        robot__v1__telemetry_state__pack(&msg, buf);

        int rc = mosquitto_publish(cli.mosq, NULL,
                                  TOPIC_TELEMETRY_STATE,
                                  (int)n, buf,
                                  0, false);

        free(buf);

        if(rc != MOSQ_ERR_SUCCESS){
            fprintf(stderr, "[STATE] publish failed: %s\n", mosquitto_strerror(rc));
        }

        // tick
        next.tv_nsec += period_ns;
        while(next.tv_nsec >= 1000000000L){
            next.tv_nsec -= 1000000000L;
            next.tv_sec += 1;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }

    mqtt_client_close(&cli);
    free(a);
    return NULL;
}

int telemetry_state_pub_start(const sensor_runtime_t *rt,
                             telemetry_cache_t *cache,
                             double state_rate_hz,
                             pthread_t *out_thread){
    telemetry_pub_args_t *a = (telemetry_pub_args_t*)calloc(1, sizeof(*a));
    if(!a) return -1;

    a->rt = *rt;
    a->cache = cache;
    a->robot_id = "robot_sim";
    a->imu_id = "imu0";
    a->tilt_id = "tilt0";
    a->motor1_id = "motor1";
    a->motor2_id = "motor2";
    a->state_rate_hz = state_rate_hz;

    snprintf(a->client_id, sizeof(a->client_id), "telemetry-state-%d", getpid());

    if(pthread_create(out_thread, NULL, telemetry_pub_thread, a) != 0){
        free(a);
        return -2;
    }
    return 0;
}