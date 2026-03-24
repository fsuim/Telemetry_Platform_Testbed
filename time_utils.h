#ifndef TIME_UTILS_H
#define TIME_UTILS_H

#include <time.h>

static inline void sleep_until_next_tick(struct timespec *next, long period_ns){
    next->tv_nsec += period_ns;
    while(next->tv_nsec >= 1000000000L){
        next->tv_nsec -= 1000000000L;
        next->tv_sec += 1;
    }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, next, NULL);
}

#endif