# P2: dual-stack TCP stream bridge

Status: **complete on GitHub-hosted runners**.

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

## Implemented bridge ownership

`src/bridge/bridge.*` owns one control object per accepted public flow. `src/runtime/lwip_loop.*` remains the sole mutable owner of lwIP and host-socket readiness. There are no per-flow forwarding threads.

Public IPv4 and IPv6 use the same bridge state machine. The only public-family-specific work is listener/TUN bring-up; every accepted flow connects to a nonblocking `AF_INET` socket at `127.0.0.1:<backend-port>`.

The runtime event loop now supports caller-owned fd watcher objects. It intentionally consumes one ready fd per `epoll_wait`, so a backend callback may unregister and free its enclosing flow without another event in the same batch retaining a stale pointer.

## Backpressure and memory residency

The bridge deliberately has no fixed application-data buffer in either direction.

Public-to-backend bytes remain in lwIP-delivered pbufs until `writev()` commits bytes to the backend socket. `tcp_recved()` is called only for committed bytes. If the backend send path returns `EAGAIN`, the pbuf remains owned by the flow and backend `EPOLLOUT` is armed. The public receive window therefore applies backpressure instead of allowing unbounded userspace accumulation.

Backend-to-public uses a 4-KiB stack scratch buffer plus `MSG_PEEK`. The host socket is consumed only after `tcp_write(..., TCP_WRITE_FLAG_COPY)` accepts the same bytes into lwIP. If lwIP send memory is exhausted, backend readable interest is suppressed and restored from `tcp_sent`/`tcp_poll`.

Backend sockets request 16-KiB `SO_SNDBUF` and `SO_RCVBUF`. Linux reports 32 KiB for both on the qualification runner because the kernel accounts/doubles the requested value. P2 records these observed values rather than treating the requested value as the actual kernel allocation.

The 1-MiB blocked-peer gate produced:

```text
bridge_peak_pending_public_bytes=32768
bridge_backend_write_blocked_events=172
bridge_backend_read_blocked_events=357
bridge_backend_socket_sndbuf_bytes=32768
bridge_backend_socket_rcvbuf_bytes=32768
```

The exact blocked-event counts are workload observations, not stable thresholds. The gates require both directions to encounter real pressure, pending public residency to remain bounded by the configured 32-KiB lwIP receive window, and the final pending count to return to zero.

P2 accounts bridge objects by `active_flows`/`peak_active_flows`, dynamic public-side residency by `pending_public_bytes`/`peak_pending_public_bytes`, and backend kernel buffers by the observed socket-buffer values. It does not claim that `sizeof(struct tcp_shift_bridge_flow)` is the per-connection memory cost. P3 measures real RSS/PSS/private-dirty increments for established flows.

## EOF, half-close, and readiness semantics

Public EOF is propagated to the backend with `shutdown(SHUT_WR)` only after all queued public bytes have been committed. Backend EOF is propagated to lwIP with `tcp_shutdown(..., shut_tx=1)` while the public receive direction remains usable.

Qualification exposed a level-triggered `EPOLLRDHUP` hazard: after backend EOF, keeping RDHUP armed could spin while the public side remained open. The bridge now removes the backend watcher when there is no useful read/write interest and re-adds only the required events when later progress requires them.

The backend-first half-close gate deliberately keeps the public write direction open for 0.5 seconds after observing backend FIN, then sends another 64 KiB. The retained run completed with only 10 `epoll_wait` calls and no readiness spin.

## Failure and teardown semantics

Backend connect refusal aborts only that public flow. A fresh flow on the same listener can connect normally afterward.

Public reset and backend reset are isolated to their respective bridge flow. The reset recovery gate performs a public RST, then a backend RST, then a healthy third flow in the same long-lived runtime. The final metrics are:

```text
bridge_accepts=3
bridge_backend_connects=3
bridge_backend_failures=1
bridge_public_errors=1
bridge_active_flows=0
```

Normal drain releases a flow before process shutdown. Process shutdown also explicitly tears down active bridge flows rather than relying on process exit. The active-shutdown gate observed:

```text
pre_stop_active_flows=1
shutdown_bridge_active_flows=0
shutdown_bridge_pending_public_bytes=0
client_close=ok
backend_close=ok
tun_cleanup=ok
```

## Concurrency and reuse

Eight simultaneous flows are established before the backend begins draining them. Concurrent payloads carry their own flow IDs so the harness does not incorrectly equate nondeterministic TCP accept order with client creation order.

The passing gate records:

```text
bridge_accepts=8
bridge_backend_connects=8
bridge_peak_active_flows=8
bridge_active_flows=0
bridge_public_to_backend_bytes=262144
bridge_backend_to_public_bytes=262144
bridge_backend_failures=0
bridge_public_errors=0
```

A 64-flow sequential reuse gate then exercises one long-lived runtime and samples `/proc/<pid>/status` after 8, 32, and 64 completed flows. On the retained runner:

```text
rss_warmup_kb=1800
rss_mid_kb=1800
rss_final_kb=1800
rss_allowance_kb=1024
bridge_reuse_no_ratcheting=ok
```

This is a P2 lifecycle/no-ratcheting gate, not a capacity claim. P3 replaces these coarse VmRSS samples with staged RSS/PSS/private-dirty and per-connection measurements.

## Dual-stack qualification

The IPv4 gate sends a deterministic 128-KiB payload through the public lwIP connection, half-closes the public write side, and requires byte-for-byte echo through the IPv4 loopback backend.

The IPv6 gate runs the same bridge implementation with an IPv6 public lwIP listener and the same IPv4 loopback backend. It therefore directly qualifies the architecture requirement that public address family and backend address family are independent.

Both gates require natural teardown with `bridge_active_flows=0` before `tcp_shift_bridge_stop()`.

## Exit evidence

Behavior head `601a49648610513d98173e3e3add722326591ffc` passed all three workflows:

- P0 run `34769960299`: success;
- full P1 dual-stack packet/lifecycle run `34769960302`: success;
- P2 bridge run `34769960275`: success.

P2 run `34769960275` qualifies:

- IPv4 and IPv6 public streams through the same bridge state machine;
- exact bidirectional payload integrity;
- real blocked-peer pressure in both directions with bounded residency;
- public-first and backend-first half-close behavior;
- backend refusal followed by listener reuse;
- public RST and backend RST followed by recovery;
- eight simultaneous flows and deterministic final object cleanup;
- SIGTERM with an active flow and explicit bridge teardown;
- 64 sequential connect/drain/reuse cycles without VmRSS ratcheting on the runner.

P2 is therefore complete on GitHub-hosted runners. Provider/OpenVZ qualification remains a deployment task rather than a bridge-state-machine milestone.

## Next milestone

P3 establishes the actual memory/capacity baseline for 32/64/128-MiB targets: idle process memory, incremental idle-established connection cost, active-flow residency at controlled inflight data, peak/post-drain floors, connection-count ceilings, and CPU behavior. Those measurements determine whether the project has enough memory headroom to proceed toward the generic CC boundary and the later BBR sampler/pacer work.
