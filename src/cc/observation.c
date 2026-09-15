#include "cc/observation.h"

#include <limits.h>
#include <stddef.h>

void tcp_shift_cc_srtt_init(struct tcp_shift_cc_srtt *srtt)
{
    if (srtt == NULL) {
        return;
    }
    srtt->smoothed_rtt_ns = 0U;
    srtt->samples = 0U;
}

int tcp_shift_cc_srtt_update(struct tcp_shift_cc_srtt *srtt,
                             uint64_t sample_rtt_ns)
{
    uint64_t delta;
    uint64_t adjustment;

    if (srtt == NULL || sample_rtt_ns == 0U) {
        return -1;
    }

    if (srtt->samples == 0U) {
        srtt->smoothed_rtt_ns = sample_rtt_ns;
    } else if (sample_rtt_ns >= srtt->smoothed_rtt_ns) {
        delta = sample_rtt_ns - srtt->smoothed_rtt_ns;
        srtt->smoothed_rtt_ns += delta / 8U;
    } else {
        delta = srtt->smoothed_rtt_ns - sample_rtt_ns;
        adjustment = delta / 8U;
        if ((delta % 8U) != 0U) {
            adjustment++;
        }
        srtt->smoothed_rtt_ns -= adjustment;
    }

    if (srtt->samples != UINT64_MAX) {
        srtt->samples++;
    }
    return 0;
}
