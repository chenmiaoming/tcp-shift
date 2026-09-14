#ifndef TCP_SHIFT_CC_RENO_H
#define TCP_SHIFT_CC_RENO_H

#include <stdint.h>

#include "cc/cc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Conventional byte-counting Reno state used to qualify the generic boundary.
 * It is intentionally small and caller-owned. It is not a BBR precursor state
 * object and does not contain delivery-rate or pacing-scheduler metadata. */
struct tcp_shift_reno_state {
    uint32_t cwnd_bytes;
    uint32_t ssthresh_bytes;
    uint32_t ca_acked_bytes;
    uint32_t min_cwnd_bytes;
};

/* P5c qualification policy: identical Reno cwnd/ssthresh semantics while
 * publishing a caller-selected fixed nonzero pacing rate. Keeping it in the
 * pure-C policy boundary proves that pacing is driven through the generic
 * policy output rather than an lwIP/runtime test hook. */
struct tcp_shift_fixed_pacing_reno_state {
    struct tcp_shift_reno_state reno;
    uint64_t pacing_rate_bytes_per_sec;
};

extern const struct tcp_shift_cc_ops tcp_shift_reno_ops;
extern const struct tcp_shift_cc_ops tcp_shift_fixed_pacing_reno_ops;

#ifdef __cplusplus
}
#endif

#endif /* TCP_SHIFT_CC_RENO_H */
