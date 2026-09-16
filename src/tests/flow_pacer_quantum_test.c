#include "runtime/pacer.h"

#include <stdint.h>
#include <stdio.h>

#define CHECK(expr)                                                          \
    do {                                                                     \
        if (!(expr)) {                                                       \
            fprintf(stderr,                                                  \
                    "flow-pacer-quantum: check failed at %s:%d: %s\n",     \
                    __FILE__, __LINE__, #expr);                              \
            return 1;                                                        \
        }                                                                    \
    } while (0)

int main(void)
{
    struct tcp_shift_flow_pacer flow;
    struct tcp_shift_flow_pacer strict;
    uint64_t deadline = 0U;
    const uint64_t base = UINT64_C(1000000000);
    const uint64_t one_ms = UINT64_C(1000000);

    /* 1 MB/s and 1000-byte segments give exactly 1 ms of virtual-clock debt
     * per segment. A 2000-byte quantum must therefore release two segments at
     * one eligible instant and then defer the third until 2 ms has elapsed. */
    tcp_shift_flow_pacer_init(&flow, 0U);
    tcp_shift_flow_pacer_set_rate(&flow, UINT64_C(1000000));
    tcp_shift_flow_pacer_set_quantum(&flow, 2000U);

    CHECK(tcp_shift_flow_pacer_segment_eligible(
              &flow, base, 1000U, &deadline) == 1);
    CHECK(deadline == 0U);
    CHECK(flow.quantum_remaining_bytes == 1000U);
    CHECK(flow.quantum_grants == 1U);
    CHECK(tcp_shift_flow_pacer_note_tx(&flow, base, 1000U) == 0);
    CHECK(flow.next_send_ns == base + one_ms);

    deadline = 0U;
    CHECK(tcp_shift_flow_pacer_segment_eligible(
              &flow, base, 1000U, &deadline) == 1);
    CHECK(deadline == 0U);
    CHECK(flow.quantum_remaining_bytes == 0U);
    CHECK(flow.quantum_grants == 1U);
    CHECK(tcp_shift_flow_pacer_note_tx(&flow, base, 1000U) == 0);
    CHECK(flow.next_send_ns == base + 2U * one_ms);

    deadline = 0U;
    CHECK(tcp_shift_flow_pacer_segment_eligible(
              &flow, base, 1000U, &deadline) == 0);
    CHECK(deadline == base + 2U * one_ms);
    CHECK(flow.quantum_grants == 1U);

    deadline = 0U;
    CHECK(tcp_shift_flow_pacer_segment_eligible(
              &flow, base + 2U * one_ms, 1000U, &deadline) == 1);
    CHECK(deadline == 0U);
    CHECK(flow.quantum_remaining_bytes == 1000U);
    CHECK(flow.quantum_grants == 2U);

    /* Replacing the quantum size invalidates unused allowance from the old
     * bucket. This prevents a rate/quantum update from inheriting stale burst
     * credit. Re-publishing the same size remains idempotent. */
    tcp_shift_flow_pacer_set_quantum(&flow, 3000U);
    CHECK(flow.quantum_bytes == 3000U);
    CHECK(flow.quantum_remaining_bytes == 0U);
    tcp_shift_flow_pacer_set_quantum(&flow, 3000U);
    CHECK(flow.quantum_remaining_bytes == 0U);

    /* A zero quantum preserves the previous strict one-segment pacer: every
     * segment opens only enough allowance for itself, so the next one must
     * observe the virtual deadline. */
    tcp_shift_flow_pacer_init(&strict, 0U);
    tcp_shift_flow_pacer_set_rate(&strict, UINT64_C(1000000));
    CHECK(tcp_shift_flow_pacer_segment_eligible(
              &strict, base, 1000U, &deadline) == 1);
    CHECK(strict.quantum_remaining_bytes == 0U);
    CHECK(tcp_shift_flow_pacer_note_tx(&strict, base, 1000U) == 0);
    deadline = 0U;
    CHECK(tcp_shift_flow_pacer_segment_eligible(
              &strict, base, 1000U, &deadline) == 0);
    CHECK(deadline == base + one_ms);

    /* A short final segment can consume the remainder of a quantum, but a
     * remainder smaller than the next segment is deliberately discarded and
     * cannot bypass a deadline. */
    tcp_shift_flow_pacer_init(&flow, 0U);
    tcp_shift_flow_pacer_set_rate(&flow, UINT64_C(1000000));
    tcp_shift_flow_pacer_set_quantum(&flow, 2500U);
    CHECK(tcp_shift_flow_pacer_segment_eligible(
              &flow, base, 1000U, &deadline) == 1);
    CHECK(tcp_shift_flow_pacer_note_tx(&flow, base, 1000U) == 0);
    CHECK(flow.quantum_remaining_bytes == 1500U);
    CHECK(tcp_shift_flow_pacer_segment_eligible(
              &flow, base, 1000U, &deadline) == 1);
    CHECK(tcp_shift_flow_pacer_note_tx(&flow, base, 1000U) == 0);
    CHECK(flow.quantum_remaining_bytes == 500U);
    deadline = 0U;
    CHECK(tcp_shift_flow_pacer_segment_eligible(
              &flow, base, 1000U, &deadline) == 0);
    CHECK(flow.quantum_remaining_bytes == 0U);
    CHECK(deadline == base + 2U * one_ms);

    CHECK(tcp_shift_flow_pacer_segment_eligible(
              NULL, base, 1000U, &deadline) < 0);
    CHECK(tcp_shift_flow_pacer_segment_eligible(
              &flow, 0U, 1000U, &deadline) < 0);
    CHECK(tcp_shift_flow_pacer_segment_eligible(
              &flow, base, 0U, &deadline) < 0);
    CHECK(tcp_shift_flow_pacer_segment_eligible(
              &flow, base, 1000U, NULL) < 0);

    printf("flow_pacer_quantum=ok quantum_bytes=2000 batch_segments=2 "
           "long_term_spacing_ms=2 stale_credit_cleared=1 strict_mode=1\n");
    return 0;
}
