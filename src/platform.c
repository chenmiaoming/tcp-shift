#include <errno.h>
#include <stdint.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

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

unsigned int lwip_port_rand(void)
{
    unsigned int value;
    ssize_t ret;

    do {
        ret = getrandom(&value, sizeof(value), 0);
    } while (ret < 0 && errno == EINTR);

    if (ret == (ssize_t)sizeof(value)) {
        return value;
    }

    /* getrandom is expected on supported Linux hosts. Keep P0 usable on a
     * degraded host while avoiding a dependency on libc rand() global state. */
    return (unsigned int)sys_now() ^ (unsigned int)getpid();
}
