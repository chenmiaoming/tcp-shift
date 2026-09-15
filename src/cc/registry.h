#ifndef TCP_SHIFT_CC_REGISTRY_H
#define TCP_SHIFT_CC_REGISTRY_H

#include "cc/cc.h"
#include "cc/cubic.h"
#include "cc/reno.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Built-in controller state is caller-owned and bounded. Keep this capacity at
 * the maximum state_size of controllers actually registered in this build.
 * Each registered state type is also a union member so casts from the opaque
 * state pointer have defined C object/alignment semantics. */
#define TCP_SHIFT_CC_BUILTIN_STATE_CAPACITY sizeof(struct tcp_shift_cubic_model)
union tcp_shift_cc_builtin_state {
    struct tcp_shift_reno_state reno;
    struct tcp_shift_cubic_model cubic;
    uint64_t alignment;
    unsigned char bytes[TCP_SHIFT_CC_BUILTIN_STATE_CAPACITY];
};

/* Built-in controller lookup. The registry itself is freestanding and does
 * not allocate. Returning NULL means the requested controller is not built in.
 * Reno remains the default even when additional qualified controllers are
 * registered. */
const struct tcp_shift_cc_ops *tcp_shift_cc_find_ops(const char *name);
const struct tcp_shift_cc_ops *tcp_shift_cc_default_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* TCP_SHIFT_CC_REGISTRY_H */
