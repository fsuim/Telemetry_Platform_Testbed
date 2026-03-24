#ifndef MOTOR1_SENSOR_H
#define MOTOR1_SENSOR_H

#include <pthread.h>

#include "../sensor_runtime.h"
#include "../telemetry_cache.h"

// Publica tópicos RAW do motor1 e atualiza o telemetry_cache
int motor1_sensor_start(const sensor_runtime_t *rt,
                        telemetry_cache_t *cache,
                        pthread_t *out_thread);

#endif