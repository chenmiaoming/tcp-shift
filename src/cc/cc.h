#ifndef TCP_SHIFT_CC_CC_H
#define TCP_SHIFT_CC_CC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Generic congestion-control input owned by the transport adapter.
 *
 * This boundary deliberately contains transport semantics only. It must not
 * expose lwIP objects, Linux descriptors, timers, or scheduler primitives.
 */
struct tcp_shift_cc_transport {
    uint32_t mss_bytes;
    uint32_t inflight_bytes;
    uint32_t send_window_bytes;
    /* Largest cwnd/ssthresh value the transport can represent. This is a
     * transport capability, not a congestion-control target. */
    uint32_t cwnd_limit_bytes;
};

struct tcp_shift_cc_init {
    uint32_t initial_cwnd_bytes;
    uint32_t initial_ssthresh_bytes;
    uint32_t min_cwnd_bytes;
};

#define TCP_SHIFT_CC_RATE_SAMPLE_VALID UINT32_C(0x01)
#define TCP_SHIFT_CC_RATE_SAMPLE_APP_LIMITED UINT32_C(0x02)
#define TCP_SHIFT_CC_RATE_SAMPLE_RETRANSMITTED UINT32_C(0x04)
#define TCP_SHIFT_CC_RATE_SAMPLE_RTT_VALID UINT32_C(0x08)

/* Transport-neutral delivery-rate observation. The transport adapter owns the
 * timestamp/sequence mechanics; controllers consume only the resulting sample.
 * A sample without VALID set must be ignored for bandwidth estimation. */
struct tcp_shift_cc_rate_sample {
    uint64_t delivery_rate_bytes_per_sec;
    uint64_t interval_ns;
    uint64_t send_interval_ns;
    uint64_t ack_interval_ns;
    uint64_t rtt_ns;
    uint32_t delivered_bytes;
    uint32_t prior_inflight_bytes;
    uint32_t flags;
};

struct tcp_shift_cc_ack {
    /* Sequence-space bytes newly acknowledged. Conventional Reno keeps using
     * this field exactly as before P5. */
    uint32_t acked_bytes;
    struct tcp_shift_cc_rate_sample rate;
};

struct tcp_shift_cc_loss {
    uint32_t lost_bytes;
};

/* ssthresh is explicit policy because the transport's native recovery
 * machinery may need the controller's threshold while it temporarily owns
 * recovery-specific cwnd inflation. A zero pacing rate means that the
 * controller does not request pacing. The runtime owns pacing mechanics. */
struct tcp_shift_cc_policy {
    uint32_t cwnd_bytes;
    uint32_t ssthresh_bytes;
    uint64_t pacing_rate_bytes_per_sec;
};

struct tcp_shift_cc_ops {
    const char *name;
    size_t state_size;

    int (*init)(void *state,
                const struct tcp_shift_cc_transport *transport,
                const struct tcp_shift_cc_init *init,
                struct tcp_shift_cc_policy *policy);
    int (*on_ack)(void *state,
                  const struct tcp_shift_cc_transport *transport,
                  const struct tcp_shift_cc_ack *ack,
                  struct tcp_shift_cc_policy *policy);
    int (*on_loss)(void *state,
                   const struct tcp_shift_cc_transport *transport,
                   const struct tcp_shift_cc_loss *loss,
                   struct tcp_shift_cc_policy *policy);
    int (*on_timeout)(void *state,
                      const struct tcp_shift_cc_transport *transport,
                      struct tcp_shift_cc_policy *policy);
};

struct tcp_shift_cc {
    const struct tcp_shift_cc_ops *ops;
    void *state;
};

int tcp_shift_cc_init(struct tcp_shift_cc *cc,
                      const struct tcp_shift_cc_ops *ops,
                      void *state,
                      size_t state_size,
                      const struct tcp_shift_cc_transport *transport,
                      const struct tcp_shift_cc_init *init,
                      struct tcp_shift_cc_policy *policy);

int tcp_shift_cc_on_ack(struct tcp_shift_cc *cc,
                        const struct tcp_shift_cc_transport *transport,
                        const struct tcp_shift_cc_ack *ack,
                        struct tcp_shift_cc_policy *policy);

int tcp_shift_cc_on_loss(struct tcp_shift_cc *cc,
                         const struct tcp_shift_cc_transport *transport,
                         const struct tcp_shift_cc_loss *loss,
                         struct tcp_shift_cc_policy *policy);

int tcp_shift_cc_on_timeout(struct tcp_shift_cc *cc,
                            const struct tcp_shift_cc_transport *transport,
                            struct tcp_shift_cc_policy *policy);

#ifdef __cplusplus
}
#endif

#endif /* TCP_SHIFT_CC_CC_H */
