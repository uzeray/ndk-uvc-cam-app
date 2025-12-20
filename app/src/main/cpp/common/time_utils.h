#pragma once

#include <time.h>

static inline long long nowBoottimeNs() {
    timespec ts{};
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (long long) ts.tv_sec * 1000000000LL + (long long) ts.tv_nsec;
}
