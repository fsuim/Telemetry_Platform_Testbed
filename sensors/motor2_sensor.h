#ifndef MOTOR2_SENSOR_H
#define MOTOR2_SENSOR_H

#include <pthread.h>

#include "../sensor_runtime.h"
#include "../telemetry_cache.h"

// Publica tópicos RAW do motor2 e atualiza o telemetry_cache
int motor2_sensor_start(const sensor_runtime_t *rt,
                        telemetry_cache_t *cache,
                        pthread_t *out_thread);

#endif