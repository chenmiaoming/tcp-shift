# P2: dual-stack TCP stream bridge

Status: **active**.

## Goal

Replace the P1 probe listener with the real single-owner stream bridge between a public lwIP TCP PCB and an ordinary nonblocking Linux TCP socket connected to an IPv4 loopback backend.

```text
public IPv4/IPv6 TCP
        |
        v
     lwIP PCB
        |
        v
  bridge flow state
        |
        v
nonblocking AF_INET socket
        |
        v
  127.0.0.1 backend
```

The public and backend TCP connections remain independent transports. The bridge carries application bytes only; public congestion control remains owned by lwIP/tcp-shift and backend congestion control remains owned by the host kernel.

## Memory and backpressure direction

The first implementation deliberately avoids fixed per-flow direction buffers.

Public-to-backend bytes remain in the pbufs already delivered by lwIP until the nonblocking backend socket accepts them. The bridge writes pbuf chains directly with `writev()` and calls `tcp_recved()` only for bytes actually accepted by the backend. A blocked backend therefore closes the lwIP receive window rather than causing unbounded userspace buffering.

Backend-to-public reads use a bounded stack scratch buffer and `MSG_PEEK`. The bridge consumes bytes from the backend socket only after `tcp_write(..., TCP_WRITE_FLAG_COPY)` accepts the same bytes into lwIP. If lwIP send memory is temporarily exhausted, backend `EPOLLIN` is suppressed and restored by `tcp_sent`/`tcp_poll`, avoiding a readiness busy loop.

This still means queued application data can reside in lwIP's configured receive/send windows. P3 will measure that residency explicitly. P2 does not yet claim a final per-flow memory target.

## Runtime ownership

`runtime/lwip_loop.*` remains the sole mutable owner. P2 generalizes its epoll registration from one hard-coded TUN fd to caller-owned watcher objects while preserving the P1 TUN callback and one-ready-fd-per-wait behavior. A flow may unregister and free its backend watcher from its callback without leaving another event in the current epoll batch that points at freed memory.

No bridge worker threads are introduced.

## First qualification increment

The first gate is intentionally IPv4-only at ingress while exercising the address-family-independent bridge state machine. `tcp-shift-p2` attaches the already-qualified IPv4 L3 TUN, starts the bridge listener, and connects each accepted public flow to `127.0.0.1:<backend-port>`.

`scripts/p2-bridge-smoke.sh` sends a deterministic 128-KiB payload, half-closes the client write side, lets a loopback backend read through EOF, echoes the exact payload, and closes. The client requires byte-for-byte equality in the reverse direction.

The gate requires:

```text
bridge_accepts=1
bridge_backend_connects=1
bridge_public_to_backend_bytes=131072
bridge_backend_to_public_bytes=131072
bridge_active_flows=0
bridge_peak_active_flows=1
bridge_backend_failures=0
bridge_public_errors=0
```

`bridge_active_flows=0` is sampled before `tcp_shift_bridge_stop()`. It therefore proves normal flow teardown rather than cleanup during process shutdown.

## Remaining P2 work

After the first bidirectional gate is green, extend qualification in small increments:

- run the same bridge state machine from the IPv6 public listener;
- deterministic partial-write and blocked-peer backpressure in both directions;
- public-first and backend-first half-close ordering;
- public reset, backend reset/refusal, and backend disappearance;
- multiple simultaneous flows and fresh-flow reuse after drain;
- shutdown with active flows and zero remaining bridge objects;
- explicit flow/object memory accounting before P3 capacity tests.

The final P2 exit gate requires both public address families and all close/error paths without regressing the complete P1 packet/lifecycle suite.
