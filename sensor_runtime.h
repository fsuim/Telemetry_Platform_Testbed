#ifndef SENSOR_RUNTIME_H
#define SENSOR_RUNTIME_H

#include <signal.h>

typedef struct {
    char host[256];
    int  port;
    double rate_hz;                     // ex: 50.0
    volatile sig_atomic_t *running;     // flag global (Ctrl+C)
} sensor_runtime_t;

#endif