#ifndef IMU_SENSOR_H
#define IMU_SENSOR_H

#include <pthread.h>
#include "../sensor_runtime.h"
#include "../telemetry_cache.h"

int imu_sensor_start(const sensor_runtime_t *rt, telemetry_cache_t *cache, pthread_t *out_thread);

#endif