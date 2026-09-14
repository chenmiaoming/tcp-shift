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
 * P5 may extend ACK observations with delivery-rate sampling, but controller
 * code must remain independent from the runtime that supplies those samples.
 */
struct tcp_shift_cc_transport {
    uint32_t mss_bytes;
    uint32_t inflight_bytes;
    uint32_t send_window_bytes;
};

struct tcp_shift_cc_init {
    uint32_t initial_cwnd_bytes;
    uint32_t initial_ssthresh_bytes;
    uint32_t min_cwnd_bytes;
};

struct tcp_shift_cc_ack {
    uint32_t acked_bytes;
};

struct tcp_shift_cc_loss {
    uint32_t lost_bytes;
};

/* A zero pacing rate means that the controller does not request pacing. The
 * runtime owns pacing mechanics; a controller only publishes policy. */
struct tcp_shift_cc_policy {
    uint32_t cwnd_bytes;
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
