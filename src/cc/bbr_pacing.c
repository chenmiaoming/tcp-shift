#include "cc/bbr.h"

#define TCP_SHIFT_BBR_NSEC_PER_SEC UINT64_C(1000000000)

uint64_t tcp_shift_bbr_initial_pacing_rate_bytes_per_sec(
    uint32_t initial_cwnd_bytes,
    uint64_t smoothed_rtt_ns)
{
    uint64_t base_rate;
    uint64_t rtt_ns;

    if (initial_cwnd_bytes == 0U) {
        return 0U;
    }

    rtt_ns = smoothed_rtt_ns != 0U ? smoothed_rtt_ns
                                   : TCP_SHIFT_BBR_INITIAL_RTT_NS;
    base_rate = ((uint64_t)initial_cwnd_bytes *
                 TCP_SHIFT_BBR_NSEC_PER_SEC) /
                rtt_ns;
    if (base_rate == 0U) {
        base_rate = 1U;
    }

    return tcp_shift_bbr_startup_pacing_rate_bytes_per_sec(base_rate);
}
