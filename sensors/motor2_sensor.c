#include "motor2_sensor.h"

#include "../mqtt_client.h"
#include "../pack_le.h"
#include "../time_utils.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TOPIC_TICS   "/robot/sensors/motor2/tics"
#define TOPIC_RPM    "/robot/sensors/motor2/rpm"
#define TOPIC_TEMP   "/robot/sensors/motor2/temperature"
#define TOPIC_VPS    "/robot/sensors/motor2/voltage_power_stage"
#define TOPIC_CPS    "/robot/sensors/motor2/current_power_stage"

#define ENC_TICKS_PER_REV_MOTOR 16384.0

typedef struct {
    sensor_runtime_t rt;
    char client_id[64];
    telemetry_cache_t *cache;
} motor2_args_t;

static double frand_unit(unsigned int *seed){
    return (double)rand_r(seed) / ((double)RAND_MAX + 1.0);
}

static double noise(unsigned int *seed, double amp){
    return (frand_unit(seed) - 0.5) * 2.0 * amp;
}

static int32_t clamp_i32_ll(long long v){
    if(v > 2147483647LL) return 2147483647;
    if(v < -2147483648LL) return (int32_t)-2147483647 - 1;
    return (int32_t)v;
}

static void publish_or_log(struct mosquitto *mosq,
                           const char *topic,
                           const void *payload,
                           int payload_len,
                           const char *tag){
    int rc = mosquitto_publish(mosq, NULL, topic, payload_len, payload, 0, false);
    if(rc != MOSQ_ERR_SUCCESS){
        fprintf(stderr, "[%s] publish %s failed: %s\n", tag, topic, mosquitto_strerror(rc));
    }
}

static void *motor2_thread(void *arg){
    motor2_args_t *a = (motor2_args_t*)arg;

    mqtt_client_t cli;
    if(mqtt_client_connect(&cli, a->client_id, a->rt.host, a->rt.port) != 0){
        fprintf(stderr, "[MOTOR2] failed to connect MQTT\n");
        free(a);
        return NULL;
    }

    const double rate_hz = (a->rt.rate_hz > 0.0) ? a->rt.rate_hz : 50.0;
    const long period_ns = (long)llround(1e9 / rate_hz);

    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)getpid() ^ 0xB4C5D602u;
    double t = 0.0;
    double tick_frac = 0.0;
    uint32_t tick_count = 0u;
    double temp_c_state = 31.0;

    while(*(a->rt.running)){
        double drive_cmd = 0.66
                        + 0.22 * sin(2.0 * M_PI * 0.10 * t + 1.0)
                        + 0.08 * sin(2.0 * M_PI * 0.29 * t + 2.1)
                        + noise(&seed, 0.02);
        if(drive_cmd < 0.03) drive_cmd = 0.03;
        if(drive_cmd > 1.00) drive_cmd = 1.00;

        const double steering_bias = 1.0 + 0.07 * sin(2.0 * M_PI * 0.05 * t + 0.4);

        double rpm_motor = (650.0 + 1750.0 * drive_cmd) * steering_bias;
        rpm_motor += 45.0 * sin(2.0 * M_PI * 0.6 * t + 1.3);
        rpm_motor += noise(&seed, 9.0);
        if(rpm_motor < 0.0) rpm_motor = 0.0;

        const double load_window = fmod(t + 4.0, 19.0);
        if(load_window > 9.2 && load_window < 10.1){
            rpm_motor *= 0.70;
        }

        const double dt = 1.0 / rate_hz;
        const double ticks_inc_real = (rpm_motor / 60.0) * ENC_TICKS_PER_REV_MOTOR * dt;
        tick_frac += ticks_inc_real;
        const uint32_t ticks_inc = (uint32_t)floor(tick_frac);
        tick_frac -= (double)ticks_inc;
        tick_count += ticks_inc;

        double v_ps_mV = 23900.0
                       + 300.0 * sin(2.0 * M_PI * 0.16 * t + 0.9)
                       - 220.0 * drive_cmd
                       + noise(&seed, 30.0);
        if(v_ps_mV < 20800.0) v_ps_mV = 20800.0;
        if(v_ps_mV > 26000.0) v_ps_mV = 26000.0;

        double i_ps_mA = 1300.0
                       + 3900.0 * drive_cmd
                       + 450.0 * fabs(sin(2.0 * M_PI * 0.21 * t + 0.1))
                       + noise(&seed, 75.0);
        if(load_window > 9.2 && load_window < 10.1) i_ps_mA += 1400.0;
        if(i_ps_mA < 0.0) i_ps_mA = 0.0;

        const double temp_target = 27.0 + 0.0070 * i_ps_mA + 0.0025 * rpm_motor;
        temp_c_state += (temp_target - temp_c_state) * 0.028 + noise(&seed, 0.06);
        if(temp_c_state < 20.0) temp_c_state = 20.0;
        if(temp_c_state > 98.0) temp_c_state = 98.0;

        const int16_t rpm_i16  = clamp_i16(lround(rpm_motor));
        const int16_t temp_i16 = clamp_i16(lround(temp_c_state));
        const int32_t v_i32 = clamp_i32_ll(llround(v_ps_mV));
        const int32_t i_i32 = clamp_i32_ll(llround(i_ps_mA));

        if(a->cache){
            telemetry_cache_set_motor(a->cache, 2, tick_count,
                                     (int32_t)rpm_i16, (int32_t)temp_i16,
                                     v_i32, i_i32);
        }

        uint8_t payload_tics[4];
        uint8_t payload_rpm[2];
        uint8_t payload_temp[2];
        uint8_t payload_vps[4];
        uint8_t payload_cps[4];

        pack_u32_le(payload_tics, tick_count);
        pack_i16_le(payload_rpm, rpm_i16);
        pack_i16_le(payload_temp, temp_i16);
        pack_i32_le(payload_vps, v_i32);
        pack_i32_le(payload_cps, i_i32);

        publish_or_log(cli.mosq, TOPIC_TICS, payload_tics, (int)sizeof(payload_tics), "MOTOR2");
        publish_or_log(cli.mosq, TOPIC_RPM, payload_rpm, (int)sizeof(payload_rpm), "MOTOR2");
        publish_or_log(cli.mosq, TOPIC_TEMP, payload_temp, (int)sizeof(payload_temp), "MOTOR2");
        publish_or_log(cli.mosq, TOPIC_VPS, payload_vps, (int)sizeof(payload_vps), "MOTOR2");
        publish_or_log(cli.mosq, TOPIC_CPS, payload_cps, (int)sizeof(payload_cps), "MOTOR2");

        sleep_until_next_tick(&next, period_ns);
        t += dt;
    }

    mqtt_client_close(&cli);
    free(a);
    return NULL;
}

int motor2_sensor_start(const sensor_runtime_t *rt,
                        telemetry_cache_t *cache,
                        pthread_t *out_thread){
    if(!rt || !out_thread || !rt->running) return -1;

    motor2_args_t *a = (motor2_args_t*)calloc(1, sizeof(*a));
    if(!a) return -2;

    a->rt = *rt;
    a->cache = cache;
    snprintf(a->client_id, sizeof(a->client_id), "fake-motor2-%d-%ld", getpid(), (long)time(NULL));

    if(pthread_create(out_thread, NULL, motor2_thread, a) != 0){
        free(a);
        return -3;
    }
    return 0;
}