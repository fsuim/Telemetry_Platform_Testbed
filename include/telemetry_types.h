#ifndef TELEMETRY_TYPES_H
#define TELEMETRY_TYPES_H

#include <stdint.h>

typedef struct {
    int64_t  received_at_ns;      // epoch (CLOCK_REALTIME) em ns
    uint64_t msg_timestamp_ns;    // header.timestamp_ns (monotonic ou epoch)
    uint32_t seq;

    char robot_id[64];
    char imu_id[32];
    char tilt_id[32];

    float acc_g[3];
    float gyro_dps[3];
    float tilt_deg[3];
    uint32_t tilt_status;

    // Motors (snapshot protobuf /robot/v1/telemetry/state)
    // idx 0 => motor1, idx 1 => motor2
    char motor_id[2][32];
    uint32_t motor_tics[2];
    int32_t  motor_rpm[2];
    int32_t  motor_temperature_c[2];
    int32_t  motor_voltage_power_stage_mv[2];
    int32_t  motor_current_power_stage_ma[2];

    const char *topic;            // ponteiro para string estática
    uint8_t *raw;                 // cópia do protobuf (opcional)
    uint32_t raw_len;
} telemetry_state_t;

#endif