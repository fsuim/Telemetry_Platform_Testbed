// state_dump.c
#include <mosquitto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "robot_telemetry.pb-c.h"

static void on_msg(struct mosquitto *m, void *ud, const struct mosquitto_message *msg){
    (void)m; (void)ud;

    if(!msg || !msg->payload || msg->payloadlen <= 0) return;

    Robot__V1__TelemetryState *st =
        robot__v1__telemetry_state__unpack(NULL, (size_t)msg->payloadlen, (const uint8_t*)msg->payload);

    if(!st){
        fprintf(stderr, "Failed to unpack TelemetryState (len=%d)\n", msg->payloadlen);
        return;
    }

    const char *robot_id = "(null)";
    unsigned seq = 0;
    unsigned long long ts = 0;

    if(st->header){
        seq = (unsigned)st->header->seq;
        ts = (unsigned long long)st->header->timestamp_ns;
        if(st->header->robot_id) robot_id = st->header->robot_id;
    }

    printf("seq=%u  t=%llu  robot_id=%s\n", seq, ts, robot_id);

    if(st->acc_g){
        printf(" acc_g:  %.3f  %.3f  %.3f\n", st->acc_g->x, st->acc_g->y, st->acc_g->z);
    }
    if(st->gyro_dps){
        printf(" gyro:   %.3f  %.3f  %.3f dps\n", st->gyro_dps->x, st->gyro_dps->y, st->gyro_dps->z);
    }
    printf(" imu_id=%s\n", st->imu_id ? st->imu_id : "(null)");

    if(st->tilt_deg){
        printf(" tilt:   %.3f  %.3f  %.3f deg\n", st->tilt_deg->x, st->tilt_deg->y, st->tilt_deg->z);
    }
    printf(" tilt_id=%s  status=%u\n",
           st->tilt_id ? st->tilt_id : "(null)",
           (unsigned)st->tilt_status);

    if(st->motor1){
        printf(" motor1 id=%s  tics=%u  rpm=%d  temp=%dC  vps=%dmV  cps=%dmA\n",
               st->motor1->motor_id ? st->motor1->motor_id : "(null)",
               (unsigned)st->motor1->tics,
               (int)st->motor1->rpm,
               (int)st->motor1->temperature_c,
               (int)st->motor1->voltage_power_stage_mv,
               (int)st->motor1->current_power_stage_ma);
    }
    if(st->motor2){
        printf(" motor2 id=%s  tics=%u  rpm=%d  temp=%dC  vps=%dmV  cps=%dmA\n",
               st->motor2->motor_id ? st->motor2->motor_id : "(null)",
               (unsigned)st->motor2->tics,
               (int)st->motor2->rpm,
               (int)st->motor2->temperature_c,
               (int)st->motor2->voltage_power_stage_mv,
               (int)st->motor2->current_power_stage_ma);
    }

    printf("\n");

    robot__v1__telemetry_state__free_unpacked(st, NULL);
}

int main(int argc, char **argv){
    const char *host = "127.0.0.1";
    int port = 1883;
    const char *topic = "/robot/v1/telemetry/state";

    if(argc >= 2) host = argv[1];
    if(argc >= 3) port = atoi(argv[2]);

    mosquitto_lib_init();
    struct mosquitto *mosq = mosquitto_new("state-dump", true, NULL);
    if(!mosq){
        fprintf(stderr, "mosquitto_new failed\n");
        return 1;
    }

    mosquitto_message_callback_set(mosq, on_msg);

    if(mosquitto_connect(mosq, host, port, 60) != MOSQ_ERR_SUCCESS){
        fprintf(stderr, "mosquitto_connect failed\n");
        return 2;
    }

    mosquitto_subscribe(mosq, NULL, topic, 0);

    printf("Listening %s:%d %s ...\n", host, port, topic);
    mosquitto_loop_forever(mosq, -1, 1);

    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    return 0;
}