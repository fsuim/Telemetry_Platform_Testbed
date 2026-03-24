#include <mosquitto.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sensor_runtime.h"
#include "sensors/imu_sensor.h"
#include "sensors/tilt_sensor.h"
#include "sensors/motor1_sensor.h"
#include "sensors/motor2_sensor.h"

#include "telemetry_cache.h"
#include "telemetry_state_pub.h"

static volatile sig_atomic_t g_running = 1;

static void on_sigint(int sig){
    (void)sig;
    g_running = 0;
}

static void usage(const char *prog){
    fprintf(stderr,
        "Usage: %s [--host HOST] [--port PORT] [--rate HZ] [--state-rate HZ]\n"
        "Defaults: host=127.0.0.1 port=1883 rate=50 state-rate=20\n", prog);
}

int main(int argc, char **argv){
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    char host[256] = "127.0.0.1";
    int port = 1883;
    double rate = 50.0;
    double state_rate = 20.0;

    for(int i=1; i<argc; i++){
        if(strcmp(argv[i], "--host")==0 && i+1<argc){
            snprintf(host, sizeof(host), "%s", argv[++i]);
        }else if(strcmp(argv[i], "--port")==0 && i+1<argc){
            port = atoi(argv[++i]);
        }else if(strcmp(argv[i], "--rate")==0 && i+1<argc){
            rate = atof(argv[++i]);
        }else if(strcmp(argv[i], "--state-rate")==0 && i+1<argc){
            state_rate = atof(argv[++i]);
        }else if(strcmp(argv[i], "--help")==0){
            usage(argv[0]);
            return 0;
        }else{
            fprintf(stderr, "Unknown arg: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    mosquitto_lib_init();

    sensor_runtime_t rt = {0};
    snprintf(rt.host, sizeof(rt.host), "%s", host);
    rt.port = port;
    rt.rate_hz = (rate > 0.0) ? rate : 50.0;
    rt.running = &g_running;

    if(state_rate <= 0.0) state_rate = 20.0;

    telemetry_cache_t cache;
    telemetry_cache_init(&cache);

    pthread_t th_imu, th_tilt, th_motor1, th_motor2, th_state;

    if(imu_sensor_start(&rt, &cache, &th_imu) != 0){
        fprintf(stderr, "Failed to start IMU sensor\n");
        mosquitto_lib_cleanup();
        return 1;
    }

    if(tilt_sensor_start(&rt, &cache, &th_tilt) != 0){
        fprintf(stderr, "Failed to start TILT sensor\n");
        g_running = 0;
        pthread_join(th_imu, NULL);
        mosquitto_lib_cleanup();
        return 1;
    }

    if(motor1_sensor_start(&rt, &cache, &th_motor1) != 0){
        fprintf(stderr, "Failed to start MOTOR1 sensor\n");
        g_running = 0;
        pthread_join(th_imu, NULL);
        pthread_join(th_tilt, NULL);
        mosquitto_lib_cleanup();
        return 1;
    }

    if(motor2_sensor_start(&rt, &cache, &th_motor2) != 0){
        fprintf(stderr, "Failed to start MOTOR2 sensor\n");
        g_running = 0;
        pthread_join(th_imu, NULL);
        pthread_join(th_tilt, NULL);
        pthread_join(th_motor1, NULL);
        mosquitto_lib_cleanup();
        return 1;
    }

    if(telemetry_state_pub_start(&rt, &cache, state_rate, &th_state) != 0){
        fprintf(stderr, "Failed to start telemetry state publisher\n");
        g_running = 0;
        pthread_join(th_imu, NULL);
        pthread_join(th_tilt, NULL);
        pthread_join(th_motor1, NULL);
        pthread_join(th_motor2, NULL);
        mosquitto_lib_cleanup();
        return 1;
    }

    printf("Robot simulator running. host=%s port=%d rate=%.2fHz state-rate=%.2fHz\n", host, port, rate, state_rate);
    printf("Press Ctrl+C to stop.\n");

    pthread_join(th_imu, NULL);
    pthread_join(th_tilt, NULL);
    pthread_join(th_motor1, NULL);
    pthread_join(th_motor2, NULL);
    pthread_join(th_state, NULL);

    mosquitto_lib_cleanup();
    printf("Stopped.\n");
    return 0;
}