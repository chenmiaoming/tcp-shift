#include <stdio.h>
#include <string.h>

#include "lwip/cc_hooks.h"

#define CHECK(expr)                                                         \
    do {                                                                    \
        if (!(expr)) {                                                      \
            fprintf(stderr, "cc-hook-recovery: check failed at %s:%d: %s\n",\
                    __FILE__, __LINE__, #expr);                             \
            return 1;                                                       \
        }                                                                   \
    } while (0)

struct fake_state {
    unsigned loss_calls;
    unsigned timeout_calls;
    unsigned handle_loss;
    unsigned handle_timeout;
};

static int fake_loss(void *arg,
                     struct tcp_pcb *pcb,
                     tcpwnd_size_t lost_bytes)
{
    struct fake_state *state = arg;

    (void)pcb;
    (void)lost_bytes;
    state->loss_calls++;
    return state->handle_loss != 0U;
}

static int fake_timeout(void *arg, struct tcp_pcb *pcb)
{
    struct fake_state *state = arg;

    (void)pcb;
    state->timeout_calls++;
    return state->handle_timeout != 0U;
}

int main(void)
{
    static const struct tcp_shift_lwip_cc_hook_ops ops = {
        .on_loss = fake_loss,
        .on_timeout = fake_timeout,
    };
    struct tcp_shift_lwip_cc_hook hook;
    struct tcp_pcb pcb;
    struct fake_state state;

    memset(&hook, 0, sizeof(hook));
    memset(&pcb, 0, sizeof(pcb));
    memset(&state, 0, sizeof(state));
    hook.ops = &ops;
    hook.arg = &state;
    pcb.ext_args[TCP_SHIFT_LWIP_CC_EXT_ARG_ID] = &hook;

    state.handle_loss = 1U;
    CHECK(tcp_shift_lwip_cc_hook_loss(&pcb, 1460U) == 1);
    CHECK(state.loss_calls == 1U);
    CHECK(tcp_shift_lwip_cc_hook_recovery_is_active(&hook) == 1U);
    CHECK(hook.recovery_enter_events == 1U);
    CHECK(hook.recovery_exit_events == 0U);
    CHECK(tcp_shift_lwip_cc_hook_take_recovery_exit(&hook) == 0U);

    /* A duplicate entry signal while the same recovery episode is active must
     * not create a second episode. Pinned lwIP normally suppresses this via
     * TF_INFR, but keep the observation layer idempotent as well. */
    tcp_shift_lwip_cc_hook_recovery_mark_enter(&hook);
    CHECK(hook.recovery_enter_events == 1U);

    tcp_shift_lwip_cc_hook_recovery_exit(&pcb);
    CHECK(tcp_shift_lwip_cc_hook_recovery_is_active(&hook) == 0U);
    CHECK(hook.recovery_exit_events == 1U);
    CHECK(tcp_shift_lwip_cc_hook_take_recovery_exit(&hook) == 1U);
    CHECK(tcp_shift_lwip_cc_hook_take_recovery_exit(&hook) == 0U);

    /* Repeated exit is also idempotent. */
    tcp_shift_lwip_cc_hook_recovery_exit(&pcb);
    CHECK(hook.recovery_exit_events == 1U);

    /* An unhandled loss falls through to native lwIP and must not be reported
     * as controller-owned recovery. */
    state.handle_loss = 0U;
    CHECK(tcp_shift_lwip_cc_hook_loss(&pcb, 1460U) == 0);
    CHECK(state.loss_calls == 2U);
    CHECK(hook.recovery_enter_events == 1U);
    CHECK(tcp_shift_lwip_cc_hook_recovery_is_active(&hook) == 0U);

    /* A handled timeout starts a distinct transport recovery episode; clear
     * any fast-recovery observation so no stale EXIT reaches a future BBR ACK. */
    state.handle_loss = 1U;
    CHECK(tcp_shift_lwip_cc_hook_loss(&pcb, 1460U) == 1);
    CHECK(hook.recovery_enter_events == 2U);
    state.handle_timeout = 1U;
    CHECK(tcp_shift_lwip_cc_hook_timeout(&pcb) == 1);
    CHECK(state.timeout_calls == 1U);
    CHECK(tcp_shift_lwip_cc_hook_recovery_is_active(&hook) == 0U);
    CHECK(tcp_shift_lwip_cc_hook_take_recovery_exit(&hook) == 0U);

    printf("lwip_recovery_observation=ok enter=handled-fast-loss "
           "exit=before-tf-infr-clear timeout=reset native_recovery=unchanged\n");
    return 0;
}
