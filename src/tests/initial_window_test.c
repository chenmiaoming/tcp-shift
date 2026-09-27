#include <stdint.h>
#include <stdio.h>

#include "lwip/initial_window.h"

#define CHECK(expr)                                                           \
    do {                                                                      \
        if (!(expr)) {                                                        \
            fprintf(stderr, "initial-window: check failed at %s:%d: %s\n", \
                    __FILE__, __LINE__, #expr);                               \
            return 1;                                                         \
        }                                                                     \
    } while (0)

int main(void)
{
    CHECK(tcp_shift_initial_cwnd_rfc6928(1460U) == 14600U);
    CHECK(tcp_shift_initial_cwnd_rfc6928(1000U) == 10000U);
    CHECK(tcp_shift_initial_cwnd_rfc6928(2000U) == 14600U);
    CHECK(tcp_shift_initial_cwnd_rfc6928(8000U) == 16000U);

    CHECK(tcp_shift_initial_cwnd_legacy(1460U) == 4380U);
    CHECK(tcp_shift_initial_cwnd_legacy(1000U) == 4000U);
    CHECK(tcp_shift_initial_cwnd_legacy(3000U) == 6000U);

#if defined(TCP_SHIFT_LEGACY_INITIAL_WINDOW) && TCP_SHIFT_LEGACY_INITIAL_WINDOW
    CHECK(tcp_shift_initial_cwnd_bytes(1460U) == 4380U);
    printf("initial_window_contract=ok default=legacy-lwip mss1460=4380 "
           "rfc6928_mss1460=14600\n");
#else
    CHECK(tcp_shift_initial_cwnd_bytes(1460U) == 14600U);
    printf("initial_window_contract=ok default=rfc6928-iw10 mss1460=14600 "
           "legacy_mss1460=4380\n");
#endif
    return 0;
}
