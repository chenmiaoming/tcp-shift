#include <stdint.h>
#include <stdio.h>

#include "cc/observation.h"

#define CHECK(expr)                                                           \
    do {                                                                      \
        if (!(expr)) {                                                        \
            fprintf(stderr,                                                   \
                    "cc-observation: check failed at %s:%d: %s\n",          \
                    __FILE__, __LINE__, #expr);                               \
            return 1;                                                         \
        }                                                                     \
    } while (0)

int main(void)
{
    struct tcp_shift_cc_srtt srtt;
    struct tcp_shift_cc_srtt before;

    tcp_shift_cc_srtt_init(&srtt);
    CHECK(srtt.smoothed_rtt_ns == 0U);
    CHECK(srtt.samples == 0U);

    CHECK(tcp_shift_cc_srtt_update(&srtt, UINT64_C(80000000)) == 0);
    CHECK(srtt.smoothed_rtt_ns == UINT64_C(80000000));
    CHECK(srtt.samples == 1U);

    /* RFC 6298 alpha=1/8: 80ms -> 90ms for a 160ms sample. */
    CHECK(tcp_shift_cc_srtt_update(&srtt, UINT64_C(160000000)) == 0);
    CHECK(srtt.smoothed_rtt_ns == UINT64_C(90000000));
    CHECK(srtt.samples == 2U);

    /* 90ms -> 83.75ms for a 40ms sample. */
    CHECK(tcp_shift_cc_srtt_update(&srtt, UINT64_C(40000000)) == 0);
    CHECK(srtt.smoothed_rtt_ns == UINT64_C(83750000));
    CHECK(srtt.samples == 3U);

    /* Integer implementation floors the exact weighted average without
     * overflowing the 64-bit nanosecond domain. */
    srtt.smoothed_rtt_ns = 8U;
    srtt.samples = 1U;
    CHECK(tcp_shift_cc_srtt_update(&srtt, 7U) == 0);
    CHECK(srtt.smoothed_rtt_ns == 7U);

    before = srtt;
    CHECK(tcp_shift_cc_srtt_update(&srtt, 0U) == -1);
    CHECK(srtt.smoothed_rtt_ns == before.smoothed_rtt_ns);
    CHECK(srtt.samples == before.samples);
    CHECK(tcp_shift_cc_srtt_update(NULL, 1U) == -1);

    tcp_shift_cc_srtt_init(NULL);

    printf("cc_observation=ok srtt_alpha=1/8 time_unit=ns zero_rejected=1\n");
    return 0;
}
