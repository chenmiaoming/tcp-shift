# P1: dual-stack L3 TUN packet path

Status: **P1a IPv4 packet path qualified; IPv4 ingress lifecycle still in progress**.

## Goal

Create the smallest correct packet boundary between Linux TUN and lwIP. P1 does not implement the application stream bridge or congestion control. Its job is to prove that lwIP can own public IPv4 and IPv6 TCP packets with event-driven host I/O, bounded memory, explicit host-resource ownership, and deterministic cleanup.

## Why TUN

The public TCP connection must terminate inside lwIP. A normal host TCP relay such as stock rinetd would terminate TCP in the outer kernel and therefore would not solve the congestion-control ownership problem. Raw physical-interface approaches introduce L2/NDP/ARP ownership and host-kernel packet competition before the transport path is proven. L3 TUN keeps the boundary explicit and testable.

## P1a implementation state

Implemented and qualified under `-Werror`:

- `src/host/tun.*` opens/closes a nonpersistent nonblocking `IFF_TUN | IFF_NO_PI` device;
- `src/host/ifconfig.*` configures host-side IPv4 address, MTU, and link-up state from the same process that owns the TUN fd;
- `src/lwip/l3_tun.*` attaches an IPv4 lwIP netif and injects complete packets into `ip4_input`;
- TX uses `writev` over the pbuf chain and preserves whole IP packets;
- TUN `EAGAIN` retains pbuf references in a bounded FIFO capped at 64 packets / 96 KiB;
- queued packets preserve FIFO order; queue exhaustion returns `ERR_MEM` and increments an observable drop counter;
- `src/runtime/lwip_loop.*` uses epoll plus `sys_timeouts_sleeptime()` / `sys_check_timeouts()` and has no fixed polling tick;
- EPOLLOUT is armed only while the bounded TUN TX queue is non-empty;
- event-loop telemetry records waits, ready wakes, timeout wakes, EINTR, readable wakes, and writable wakes;
- oversized RX packets are counted as nonfatal drops rather than terminating the runtime;
- `src/lwip/probe_listener.*` installs a minimal raw-API IPv4 TCP listener used only for P1 handshake qualification;
- `tcp-shift-p1` is a temporary privileged bring-up executable, not the final product CLI;
- unprivileged contract binaries exercise the production TX backpressure and RX-drop paths without test-only runtime hooks.

The nonpersistent TUN fd is the rollback boundary for interface/address/MTU/connected-route state. Closing the fd removes the interface and dependent state.

Still required before **P1a exit**:

- product-owned transactional narrow nftables DNAT lifecycle instead of CI-only DNAT resources;
- startup-failure, signal, and stale-resource cleanup qualification for that owned firewall resource;
- read-only preflight for required host capabilities/prerequisites without silently changing host-wide sysctls or broad firewall policy.

The packet-path properties that previously remained open—DNAT/conntrack behavior, idle wakeups, TX queue pressure, MTU boundaries, and ICMP checksum handling—are now qualified.

## Retained IPv4 evidence

### Direct ICMP and TCP

Run `34740740077` proved direct IPv4 ICMP through a real nonpersistent TUN: 3/3 replies, 0% loss, and TUN cleanup after process exit.

Run `34740867645` added a real TCP `connect()` to lwIP port 18080 and proved the raw-API `tcp_accept` callback fires after SYN/SYN-ACK/ACK.

### DNAT/conntrack path

Run `34743049605` created an isolated client namespace and veth pair and proved:

```text
198.51.100.2 client namespace
        |
        v
198.51.100.1 host-side veth:18080
        |
        | nftables PREROUTING DNAT
        v
10.231.0.2:18080 over TUN
        |
        v
lwIP raw TCP listener
```

Both direct and DNAT connections completed. The harness required conntrack evidence for original and reply tuples and cleaned the TUN, namespace, veth pair, nftables table, and temporary forwarding exceptions.

The preceding failed run `34742949259` remains useful evidence: GitHub-hosted runners carry Docker's IPv4 `FORWARD` policy `DROP`. Routes and DNAT were correct but forwarding was blocked. The harness therefore installs two exact temporary ACCEPT rules scoped to the test interfaces/IP/port. Those rules are CI scaffolding, not product behavior.

### Idle event loop

Run `34743203062` measured a separate two-second idle runtime:

```text
loop_wait_calls=4
loop_timeout_wakeups=2
loop_tun_writable_wakeups=0
```

The remaining wakeups were one TUN-readable bring-up event and the terminating SIGTERM/EINTR. CI enforces a deliberately loose ceiling of 32 waits in two seconds to catch busy/fixed-rate polling regressions.

### Bounded TX backpressure

Run `34743392756` drove the production `netif.output -> writev -> queue -> flush` path against a nonblocking packet socket filled to `EAGAIN`:

```text
tcp-shift-p1-backpressure: packets=64 peak_bytes=96000 drops=1 tx_packets=64 tx_would_block=17 fifo_order=ok detach_cleanup=ok
```

The 65th full-MTU packet returned `ERR_MEM`; FIFO ordering survived repeated EAGAIN/flush cycles; detach released queued references and reset queue state.

### MTU, RX-drop, checksum, direct TCP, and DNAT: run 34744304038

The latest complete P1a packet-path run qualified all remaining packet semantics on commit `153ebaa348c28465dafc793d4f820fa355280252`.

The unprivileged RX contract injected a 1501-byte packet into a 1500-byte adapter and required:

```text
tcp-shift-p1-rx-contract: mtu=1500 oversize=1501 rx_drops=1 rx_errors=0 runtime_survives_drop=ok
```

The real TUN path then proved a 1500-byte IPv4 DF packet succeeds while 1501 bytes is rejected by the host MTU boundary with `message too long, mtu=1500`.

The checksum probe sent one deliberately corrupt ICMP echo request and one valid request over the same TUN path. Software verification and captured wire bytes agreed:

```text
send seq=4097 valid=0 checksum_field=0xebbc software_verify=0x2886
send seq=4098 valid=1 checksum_field=0x1442 software_verify=0x0000
icmp_checksum_bad_reply=none icmp_checksum_good_reply=received
```

`tcpdump` marked the first request with a wrong checksum and the second as valid. lwIP ignored the invalid request and replied to the valid one. The same active process then completed direct TCP and DNAT TCP:

```text
direct connected 10.231.0.2:18080
DNAT connected 198.51.100.1:18080
```

Final runtime evidence was:

```text
rx_packets=16 rx_drops=0 rx_errors=0 tx_packets=9 tx_queue_peak_bytes=0 tx_queue_drops=0 tcp_accepts=2 tcp_rx_bytes=0 tcp_errors=0 loop_wait_calls=18 loop_ready_wakeups=12 loop_timeout_wakeups=5 loop_eintr_wakeups=1 loop_tun_readable_wakeups=12 loop_tun_writable_wakeups=0
```

For that commit, `lwIP P0`, `lwIP upstream provenance`, and `lwIP P1 IPv4 TUN` all passed.

## Temporary IPv4 bring-up shape

The current bring-up executable accepts:

```text
tcp-shift-p1 <tun-name> <lwip-ipv4> <netmask> <host-ipv4> [listen-port]
```

Example:

```text
tcp-shift-p1 ts0 10.0.0.2 255.255.255.252 10.0.0.1 18080
```

It creates `ts0`, configures host-side IPv4 and MTU 1500, brings the interface up, attaches lwIP at the guest address, and starts the temporary TCP probe listener. This remains a development harness, not the final CLI.

## Host-ingress ownership boundary

P1a's remaining implementation work is not another packet-path experiment. It is ownership and cleanup.

The product should own one instance-scoped exact-match nftables NAT resource after the TUN is successfully configured. The first production backend is intentionally narrow: nftables only, exact public IPv4 address + TCP destination port, no broad masquerade, no broad FORWARD policy, and no automatic fallback to a different firewall backend.

Global `net.ipv4.ip_forward` and surrounding host forwarding policy remain operator-managed prerequisites. tcp-shift should inspect and diagnose them; it should not silently rewrite global sysctls or unrelated firewall state.

Normal cleanup and startup rollback remove owned resources in reverse acquisition order: firewall resource first, then the nonpersistent TUN by closing its fd. Existing resources must never be adopted merely because a generated/requested name collides.

## P1b: IPv6

After the IPv4 ingress lifecycle is owned and qualified, extend the same adapter rather than creating a parallel runtime. Required work includes IPv6 host/TUN addressing, `netif->output_ip6`, `ip6_input`, ICMPv6, TCP, IPv6-only operation, Packet Too Big/PMTU validation, and extension-header-safe host filtering.

IPv6-only deployment is a product requirement because it is common in the low-cost VPS/OpenVZ environments targeted by tcp-shift.

## Event-loop invariant

There is one mutable lwIP owner. TUN RX, TUN TX retry, lwIP timers, and later backend sockets all execute on that owner. No per-flow worker threads are introduced.

## Packet ownership

TUN is packet-oriented. A transmit packet is never stream-split across writes. On `EAGAIN`, tcp-shift takes a pbuf reference and retains the entire packet in the bounded FIFO. The reference is released after successful transmit or adapter teardown. Queue exhaustion is observable and bounded.

RX allocates only the pbuf required by the received packet after copying from a bounded host read buffer. Correctness and bounded residency take priority over copy reduction in P1.

## P1 exit evidence

P1 as a whole is complete only when retained evidence proves both address families:

- interface acquisition/configuration and cleanup;
- ICMP/ICMPv6 through lwIP;
- TCP SYN/SYN-ACK/accept at a minimal lwIP listener;
- exact ingress DNAT/conntrack path where NAT is part of deployment;
- checksums and MTU behavior;
- IPv6 Packet Too Big/PMTU behavior;
- no permanent writable polling and bounded idle wakeups;
- bounded TX backpressure;
- no leaked TUN, route, firewall, namespace, or test resources after success and forced failure paths.

Failure diagnostics should retain packet captures, interface/routing/firewall/conntrack state, runtime counters, and logs.

## Deferred to P2

P1 does not connect accepted TCP streams to an application backend. Production raw TCP callback ownership, host loopback sockets, partial stream I/O, half-close/reset semantics, backend failure, and connection lifecycle belong to P2.
