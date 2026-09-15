#ifndef TCP_SHIFT_CC_OBSERVATION_H
#define TCP_SHIFT_CC_OBSERVATION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Transport-neutral RFC 6298 SRTT estimator.
 *
 * This deliberately tracks only SRTT because congestion-control algorithms
 * such as RFC 9438 CUBIC need a smoothed RTT reference but do not own the
 * transport's retransmission timeout calculation. The transport adapter remains
 * responsible for Karn filtering and supplies only valid RTT observations.
 */
struct tcp_shift_cc_srtt {
    uint64_t smoothed_rtt_ns;
    uint64_t samples;
};

void tcp_shift_cc_srtt_init(struct tcp_shift_cc_srtt *srtt);

/* Apply RFC 6298's alpha=1/8 SRTT update. sample_rtt_ns must be nonzero.
 * Returns 0 on success and -1 for invalid input without modifying state. */
int tcp_shift_cc_srtt_update(struct tcp_shift_cc_srtt *srtt,
                             uint64_t sample_rtt_ns);

#ifdef __cplusplus
}
#endif

#endif /* TCP_SHIFT_CC_OBSERVATION_H */
