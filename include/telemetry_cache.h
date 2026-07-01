#ifndef TELEMETRY_CACHE_H
#define TELEMETRY_CACHE_H

#include <pthread.h>
#include <stdint.h>

typedef struct {
    pthread_mutex_t mu;

    // últimos valores (já em unidades reais)
    float acc_g[3];
    float gyro_dps[3];
    float tilt_deg[3];
    uint32_t tilt_status;

    // motores (2 motores: idx 0 => motor1, idx 1 => motor2)
    uint32_t motor_tics[2];
    int32_t  motor_rpm[2];
    int32_t  motor_temp_c[2];
    int32_t  motor_voltage_mv[2];
    int32_t  motor_current_ma[2];

    uint32_t seq;
} telemetry_cache_t;

void telemetry_cache_init(telemetry_cache_t *c);

void telemetry_cache_set_imu(telemetry_cache_t *c,
                             float ax_g, float ay_g, float az_g,
                             float gx_dps, float gy_dps, float gz_dps);

void telemetry_cache_set_tilt(telemetry_cache_t *c,
                              float tx_deg, float ty_deg, float tz_deg,
                              uint32_t status);

// motor_index: 1 para motor1, 2 para motor2
void telemetry_cache_set_motor(telemetry_cache_t *c,
                               int motor_index,
                               uint32_t tics,
                               int32_t rpm,
                               int32_t temperature_c,
                               int32_t voltage_power_stage_mv,
                               int32_t current_power_stage_ma);

void telemetry_cache_get(telemetry_cache_t *c,
                         float acc_g[3], float gyro_dps[3],
                         float tilt_deg[3], uint32_t *tilt_status,
                         uint32_t motor_tics[2],
                         int32_t motor_rpm[2],
                         int32_t motor_temp_c[2],
                         int32_t motor_voltage_mv[2],
                         int32_t motor_current_ma[2],
                         uint32_t *seq_out);

#endif