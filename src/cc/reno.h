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

/* Deterministic P5c qualification policy. It uses exactly the same Reno state
 * and transitions but publishes this nonzero pacing rate through the generic
 * policy output. Production Reno remains unpaced. */
#define TCP_SHIFT_FIXED_PACING_RENO_RATE_BYTES_PER_SEC UINT64_C(65536)

extern const struct tcp_shift_cc_ops tcp_shift_reno_ops;
extern const struct tcp_shift_cc_ops tcp_shift_fixed_pacing_reno_ops;

#ifdef __cplusplus
}
#endif

#endif /* TCP_SHIFT_CC_RENO_H */
