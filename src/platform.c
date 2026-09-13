#include <stdint.h>
#include <time.h>

#include "lwip/sys.h"

u32_t sys_now(void)
{
    struct timespec ts;
    uint64_t millis;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    millis = (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
    return (u32_t)millis;
}
