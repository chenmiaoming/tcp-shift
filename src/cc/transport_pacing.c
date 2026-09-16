#include "cc/transport_pacing.h"

#include <stddef.h>

#define TCP_SHIFT_TRANSPORT_PACING_NSEC_PER_SEC UINT64_C(1000000000)
#define TCP_SHIFT_TRANSPORT_PACING_LINUX_PRE_SRTT_BASE UINT64_C(8000000)

static uint64_t tcp_shift_transport_pacing_scale_percent(uint64_t value,
                                                          uint32_t percent)
{
    uint64_t quotient;
    uint64_t remainder;
    uint64_t scaled;

    if (value == 0U || percent == 0U) {
        return 0U;
    }

    /* window_bytes is u32. Divide by 100 before applying the small Linux gains
     * so the intermediate remains within u64 for both RTT-derived and
     * pre-SRTT startup rates. */
    quotient = value / 100U;
    remainder = value % 100U;
    scaled = quotient * percent + (remainder * percent) / 100U;
    return scaled == 0U ? 1U : scaled;
}

uint64_t tcp_shift_transport_pacing_window_rate(
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_policy *policy,
    uint64_t smoothed_rtt_ns)
{
    uint64_t base_rate;
    uint32_t window_bytes;
    uint32_t percent;

    if (transport == NULL || policy == NULL) {
        return 0U;
    }

    /* Linux ordinary TCP pacing is shaped roughly as
     * max(cwnd, packets_out) * MSS / SRTT. tcp-shift already expresses cwnd
     * and in-flight in bytes, so the packet-to-byte multiplication is not
     * needed here. */
    window_bytes = policy->cwnd_bytes;
    if (transport->inflight_bytes > window_bytes) {
        window_bytes = transport->inflight_bytes;
    }
    if (window_bytes == 0U) {
        return 0U;
    }

    if (smoothed_rtt_ns != 0U) {
        base_rate = ((uint64_t)window_bytes *
                     TCP_SHIFT_TRANSPORT_PACING_NSEC_PER_SEC) /
                    smoothed_rtt_ns;
        if (base_rate == 0U) {
            base_rate = 1U;
        }
    } else {
        /* Linux tcp_update_pacing_rate() starts from
         * MSS * ((USEC_PER_SEC / 100) << 3), multiplies by the cwnd packet
         * count and the 200/120 percentage, and divides by scaled srtt_us only
         * when an RTT sample exists. In our byte-domain representation the
         * same pre-SRTT expression is window_bytes * 8,000,000 before the
         * percentage helper below. This is intentionally a very high finite
         * startup rate: it preserves Linux semantics without inventing path
         * RTT knowledge or using zero to mean "pacer disabled". */
        base_rate = (uint64_t)window_bytes *
                    TCP_SHIFT_TRANSPORT_PACING_LINUX_PRE_SRTT_BASE;
    }

    percent = policy->cwnd_bytes < policy->ssthresh_bytes / 2U
                  ? TCP_SHIFT_TRANSPORT_PACING_SS_PERCENT
                  : TCP_SHIFT_TRANSPORT_PACING_CA_PERCENT;
    return tcp_shift_transport_pacing_scale_percent(base_rate, percent);
}

int tcp_shift_transport_pacing_apply_window_fallback(
    const struct tcp_shift_cc_transport *transport,
    uint64_t smoothed_rtt_ns,
    uint64_t rate_cap_bytes_per_sec,
    struct tcp_shift_cc_policy *policy)
{
    uint64_t rate;

    if (transport == NULL || policy == NULL) {
        return -1;
    }

    /* BBR and any future rate-owning controller always win. The cap belongs
     * only to the fallback qualification path and must never constrain a
     * controller-owned pacing decision. */
    if (policy->pacing_rate_bytes_per_sec != 0U) {
        return 0;
    }

    rate = tcp_shift_transport_pacing_window_rate(
        transport, policy, smoothed_rtt_ns);
    if (rate_cap_bytes_per_sec != 0U && rate > rate_cap_bytes_per_sec) {
        rate = rate_cap_bytes_per_sec;
    }
    policy->pacing_rate_bytes_per_sec = rate;
    return 0;
}
