#ifndef TCP_SHIFT_CC_REGISTRY_H
#define TCP_SHIFT_CC_REGISTRY_H

#include "cc/cc.h"
#include "cc/reno.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Built-in controller state is caller-owned and bounded. Keep this capacity at
 * the maximum state_size of controllers actually registered in this build;
 * increase it only when a newly qualified built-in controller requires it.
 * Each registered state type is also a union member so casts from the opaque
 * state pointer have defined C object/alignment semantics. */
#define TCP_SHIFT_CC_BUILTIN_STATE_CAPACITY 16U
union tcp_shift_cc_builtin_state {
    struct tcp_shift_reno_state reno;
    uint64_t alignment;
    unsigned char bytes[TCP_SHIFT_CC_BUILTIN_STATE_CAPACITY];
};

/* Built-in controller lookup. The registry itself is freestanding and does
 * not allocate. Returning NULL means the requested controller is not built in.
 * The default remains Reno until an explicit policy change is qualified. */
const struct tcp_shift_cc_ops *tcp_shift_cc_find_ops(const char *name);
const struct tcp_shift_cc_ops *tcp_shift_cc_default_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* TCP_SHIFT_CC_REGISTRY_H */
