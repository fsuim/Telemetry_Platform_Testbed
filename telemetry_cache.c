#include "telemetry_cache.h"
#include <string.h>

void telemetry_cache_init(telemetry_cache_t *c){
    memset(c, 0, sizeof(*c));
    pthread_mutex_init(&c->mu, NULL);
}

void telemetry_cache_set_imu(telemetry_cache_t *c,
                             float ax_g, float ay_g, float az_g,
                             float gx_dps, float gy_dps, float gz_dps){
    pthread_mutex_lock(&c->mu);
    c->acc_g[0] = ax_g; c->acc_g[1] = ay_g; c->acc_g[2] = az_g;
    c->gyro_dps[0] = gx_dps; c->gyro_dps[1] = gy_dps; c->gyro_dps[2] = gz_dps;
    c->seq++;
    pthread_mutex_unlock(&c->mu);
}

void telemetry_cache_set_tilt(telemetry_cache_t *c,
                              float tx_deg, float ty_deg, float tz_deg,
                              uint32_t status){
    pthread_mutex_lock(&c->mu);
    c->tilt_deg[0] = tx_deg; c->tilt_deg[1] = ty_deg; c->tilt_deg[2] = tz_deg;
    c->tilt_status = status;
    c->seq++;
    pthread_mutex_unlock(&c->mu);
}

void telemetry_cache_set_motor(telemetry_cache_t *c,
                               int motor_index,
                               uint32_t tics,
                               int32_t rpm,
                               int32_t temperature_c,
                               int32_t voltage_power_stage_mv,
                               int32_t current_power_stage_ma){
    if(!c) return;
    if(motor_index != 1 && motor_index != 2) return;

    const int idx = motor_index - 1;
    pthread_mutex_lock(&c->mu);
    c->motor_tics[idx] = tics;
    c->motor_rpm[idx] = rpm;
    c->motor_temp_c[idx] = temperature_c;
    c->motor_voltage_mv[idx] = voltage_power_stage_mv;
    c->motor_current_ma[idx] = current_power_stage_ma;
    c->seq++;
    pthread_mutex_unlock(&c->mu);
}

void telemetry_cache_get(telemetry_cache_t *c,
                         float acc_g[3], float gyro_dps[3],
                         float tilt_deg[3], uint32_t *tilt_status,
                         uint32_t motor_tics[2],
                         int32_t motor_rpm[2],
                         int32_t motor_temp_c[2],
                         int32_t motor_voltage_mv[2],
                         int32_t motor_current_ma[2],
                         uint32_t *seq_out){
    pthread_mutex_lock(&c->mu);
    memcpy(acc_g, c->acc_g, sizeof(float)*3);
    memcpy(gyro_dps, c->gyro_dps, sizeof(float)*3);
    memcpy(tilt_deg, c->tilt_deg, sizeof(float)*3);
    if(tilt_status) *tilt_status = c->tilt_status;

    if(motor_tics) memcpy(motor_tics, c->motor_tics, sizeof(uint32_t)*2);
    if(motor_rpm) memcpy(motor_rpm, c->motor_rpm, sizeof(int32_t)*2);
    if(motor_temp_c) memcpy(motor_temp_c, c->motor_temp_c, sizeof(int32_t)*2);
    if(motor_voltage_mv) memcpy(motor_voltage_mv, c->motor_voltage_mv, sizeof(int32_t)*2);
    if(motor_current_ma) memcpy(motor_current_ma, c->motor_current_ma, sizeof(int32_t)*2);

    if(seq_out) *seq_out = c->seq;
    pthread_mutex_unlock(&c->mu);
}