#ifndef TCP_SHIFT_CC_REGISTRY_H
#define TCP_SHIFT_CC_REGISTRY_H

#include "cc/cc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Built-in controller lookup. The registry itself is freestanding and does
 * not allocate. Returning NULL means the requested controller is not built in.
 * The default remains Reno until an explicit policy change is qualified. */
const struct tcp_shift_cc_ops *tcp_shift_cc_find_ops(const char *name);
const struct tcp_shift_cc_ops *tcp_shift_cc_default_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* TCP_SHIFT_CC_REGISTRY_H */
