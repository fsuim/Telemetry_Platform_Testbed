#ifndef TELEMETRY_STATE_PUB_H
#define TELEMETRY_STATE_PUB_H

#include <pthread.h>
#include "sensor_runtime.h"
#include "telemetry_cache.h"

// Inicia um thread que publica /robot/v1/telemetry/state em Protobuf (TelemetryState)
int telemetry_state_pub_start(const sensor_runtime_t *rt,
                              telemetry_cache_t *cache,
                              double state_rate_hz,
                              pthread_t *out_thread);

#endif