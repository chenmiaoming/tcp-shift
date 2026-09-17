#include <stdint.h>
#include <stdio.h>

#include "cc/transport_pacing.h"

#define CHECK(expr)                                                           \
    do {                                                                      \
        if (!(expr)) {                                                        \
            fprintf(stderr,                                                   \
                    "transport-pacing-quantum: check failed at %s:%d: %s\n", \
                    __FILE__, __LINE__, #expr);                               \
            return 1;                                                         \
        }                                                                     \
    } while (0)

int main(void)
{
    const uint32_t mss = 1460U;

    /* Linux v6.17 tcp_tso_autosize() starts its byte target at
     * sk_pacing_rate >> sk_pacing_shift, with the default pacing shift 10.
     * The fq packet traces in this qualification branch show the corresponding
     * low/edge/high steady packet groups: 2, 2 and about 4 MSS respectively. */
    CHECK(tcp_shift_transport_pacing_quantum_bytes(
              UINT64_C(2944954), mss) == 2U * mss);
    CHECK(tcp_shift_transport_pacing_quantum_bytes(
              UINT64_C(1451000), mss) == 2U * mss);
    CHECK(tcp_shift_transport_pacing_quantum_bytes(
              UINT64_C(7281000), mss) == 4U * mss);

    /* tcp-shift's current high-BDP dynamic target is slightly higher than the
     * Linux reference, so the same rate-shaped rule yields a six-MSS batch. */
    CHECK(tcp_shift_transport_pacing_quantum_bytes(
              UINT64_C(9257067), mss) == 6U * mss);

    /* Userspace v1 deliberately caps batching because it has no GSO/TSO skb
     * to let a downstream qdisc segment a larger aggregate safely. */
    CHECK(tcp_shift_transport_pacing_quantum_bytes(
              UINT64_MAX, mss) ==
          TCP_SHIFT_TRANSPORT_PACING_MAX_QUANTUM_SEGS * mss);

    CHECK(tcp_shift_transport_pacing_quantum_bytes(0U, mss) == 0U);
    CHECK(tcp_shift_transport_pacing_quantum_bytes(UINT64_C(1000000), 0U) ==
          0U);

    printf("transport_pacing_quantum=ok shift=%u min_segs=%u max_segs=%u "
           "low_segs=2 edge_segs=2 high_linux_segs=4\n",
           TCP_SHIFT_TRANSPORT_PACING_QUANTUM_SHIFT,
           TCP_SHIFT_TRANSPORT_PACING_MIN_QUANTUM_SEGS,
           TCP_SHIFT_TRANSPORT_PACING_MAX_QUANTUM_SEGS);
    return 0;
}
