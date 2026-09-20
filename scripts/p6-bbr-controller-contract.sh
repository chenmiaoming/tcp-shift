#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build/p6-bbr-controller-contract"
BINARY="$BUILD/tcp-shift-p6-bbr-controller-contract"
RECOVERY_BINARY="$BUILD/tcp-shift-p6-bbr-recovery-contract"
TIMEOUT_BINARY="$BUILD/tcp-shift-p6-bbr-timeout-contract"
HOOK_BINARY="$BUILD/tcp-shift-p6-lwip-recovery-observation-contract"
STUB="$BUILD/stub"

rm -rf "$BUILD"
mkdir -p "$BUILD" "$STUB/lwip"

${CC:-cc} \
    -std=c11 \
    -Wall -Wextra -Wpedantic -Werror \
    -ffreestanding -fno-builtin \
    -I"$ROOT/src" \
    "$ROOT/src/cc/bbr.c" \
    "$ROOT/src/cc/bbr_pacing.c" \
    "$ROOT/src/cc/bbr_drain.c" \
    "$ROOT/src/cc/bbr_probe.c" \
    "$ROOT/src/cc/bbr_recovery.c" \
    "$ROOT/src/cc/bbr_controller.c" \
    "$ROOT/src/tests/bbr_controller_test.c" \
    -o "$BINARY"

${CC:-cc} \
    -std=c11 \
    -Wall -Wextra -Wpedantic -Werror \
    -ffreestanding -fno-builtin \
    -I"$ROOT/src" \
    "$ROOT/src/cc/bbr_recovery.c" \
    "$ROOT/src/tests/bbr_recovery_test.c" \
    -o "$RECOVERY_BINARY"

${CC:-cc} \
    -std=c11 \
    -Wall -Wextra -Wpedantic -Werror \
    -ffreestanding -fno-builtin \
    -I"$ROOT/src" \
    "$ROOT/src/cc/bbr.c" \
    "$ROOT/src/cc/bbr_pacing.c" \
    "$ROOT/src/cc/bbr_drain.c" \
    "$ROOT/src/cc/bbr_probe.c" \
    "$ROOT/src/cc/bbr_recovery.c" \
    "$ROOT/src/cc/bbr_controller.c" \
    "$ROOT/src/tests/bbr_timeout_test.c" \
    -o "$TIMEOUT_BINARY"

cat > "$STUB/lwip/opt.h" <<'EOF'
#ifndef LWIP_OPT_H
#define LWIP_OPT_H
#define LWIP_TCP 1
#define LWIP_TCP_PCB_NUM_EXT_ARGS 2
#endif
EOF

cat > "$STUB/lwip/tcp.h" <<'EOF'
#ifndef LWIP_TCP_H
#define LWIP_TCP_H
#include <stdint.h>
typedef uint8_t u8_t;
typedef uint16_t u16_t;
typedef uint32_t u32_t;
typedef uint32_t tcpwnd_size_t;
#define TF_INFR 0x01U
struct tcp_pcb {
    void *ext_args[2];
    u8_t flags;
};
#define tcp_clear_flags(pcb, flag_bits) \
    ((pcb)->flags = (u8_t)((pcb)->flags & (u8_t)~(flag_bits)))
static inline void *tcp_ext_arg_get(const struct tcp_pcb *pcb, u8_t id)
{
    return pcb != 0 && id < 2U ? pcb->ext_args[id] : 0;
}
#endif
EOF

${CC:-cc} \
    -std=c11 \
    -Wall -Wextra -Wpedantic -Werror \
    -ffreestanding -fno-builtin \
    -I"$STUB" \
    -I"$ROOT/src" \
    "$ROOT/src/tests/cc_hook_recovery_test.c" \
    -o "$HOOK_BINARY"

"$BINARY" | tee "$BUILD/summary.txt"
grep -F 'bbr_controller_lifecycle=ok modes=startup-drain-probebw-probertt-probebw ' \
    "$BUILD/summary.txt" >/dev/null
grep -F 'public_ops=disabled loss_timeout=pending cycle_seed=external' \
    "$BUILD/summary.txt" >/dev/null
grep -F 'bbr_controller_recovery=ok conservation=one-packet-round ' \
    "$BUILD/summary.txt" >/dev/null
grep -F 'round_marker=delivered restore=prior_cwnd public_ops=disabled' \
    "$BUILD/summary.txt" >/dev/null

"$RECOVERY_BINARY" | tee "$BUILD/recovery-summary.txt"
grep -F 'bbr_recovery=ok packet_conservation=first_round ' \
    "$BUILD/recovery-summary.txt" >/dev/null
grep -F 'restore=prior_cwnd sndbuf_expand=3x units=bytes' \
    "$BUILD/recovery-summary.txt" >/dev/null

"$TIMEOUT_BINARY" | tee "$BUILD/timeout-summary.txt"
grep -F 'bbr_controller_timeout=ok loss_state=preserve-mode ' \
    "$BUILD/timeout-summary.txt" >/dev/null
grep -F 'full_bw_baseline=reset full_bw_reached=preserved ' \
    "$BUILD/timeout-summary.txt" >/dev/null
grep -F 'cwnd=post-loss-inflight-plus-one-mss pacing=preserved' \
    "$BUILD/timeout-summary.txt" >/dev/null

"$HOOK_BINARY" | tee "$BUILD/recovery-observation-summary.txt"
grep -F 'lwip_recovery_observation=ok enter=handled-fast-loss ' \
    "$BUILD/recovery-observation-summary.txt" >/dev/null
grep -F 'exit=before-tf-infr-clear timeout=reset controller_owned=qualified ' \
    "$BUILD/recovery-observation-summary.txt" >/dev/null
grep -F 'native_recovery=unchanged' \
    "$BUILD/recovery-observation-summary.txt" >/dev/null

printf '%s\n' \
    'p6h_bbr_controller=internal-lifecycle-with-recovery-consumption' \
    'p6h_public_registry=disabled' \
    'p6i_bbr_recovery=packet-conservation-model-qualified' \
    'p6i_lwip_recovery_observation=hook-state-qualified' \
    'p6i_bbr_recovery_consumption=internal-controller-qualified' \
    'p6j_bbr_timeout=loss-state-model-qualified' \
    'p6j_timeout_post_loss_inflight=transport-observation-required' \
    'p6j_lwip_bbr_binding=recovery-timeout-qualified' \
    | tee "$BUILD/reference.txt"
