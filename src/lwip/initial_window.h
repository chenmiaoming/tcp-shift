#ifndef TCP_SHIFT_LWIP_INITIAL_WINDOW_H
#define TCP_SHIFT_LWIP_INITIAL_WINDOW_H

#include <stdint.h>

/* RFC 6928 section 2:
 *   IW = min(10 * SMSS, max(2 * SMSS, 14600))
 *
 * Keep the legacy pinned-lwIP formula available as an explicit rollback
 * profile, but make RFC 6928 the default transport policy.
 */
static inline uint32_t tcp_shift_initial_cwnd_rfc6928(uint32_t mss_bytes)
{
    uint32_t twice_mss = 2U * mss_bytes;
    uint32_t ten_mss = 10U * mss_bytes;
    uint32_t ceiling = twice_mss > 14600U ? twice_mss : 14600U;

    return ten_mss < ceiling ? ten_mss : ceiling;
}

static inline uint32_t tcp_shift_initial_cwnd_legacy(uint32_t mss_bytes)
{
    uint32_t twice_mss = 2U * mss_bytes;
    uint32_t four_mss = 4U * mss_bytes;
    uint32_t floor = twice_mss > 4380U ? twice_mss : 4380U;

    return four_mss < floor ? four_mss : floor;
}

static inline uint32_t tcp_shift_initial_cwnd_bytes(uint32_t mss_bytes)
{
#if defined(TCP_SHIFT_LEGACY_INITIAL_WINDOW) && TCP_SHIFT_LEGACY_INITIAL_WINDOW
    return tcp_shift_initial_cwnd_legacy(mss_bytes);
#else
    return tcp_shift_initial_cwnd_rfc6928(mss_bytes);
#endif
}

#endif /* TCP_SHIFT_LWIP_INITIAL_WINDOW_H */
