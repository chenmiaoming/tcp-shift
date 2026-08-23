# Linux TCP runtime exploration

## Goal

Explore a second tcp-shift userspace TCP backend that reuses the Linux TCP/IP implementation directly instead of re-implementing Linux BBR semantics on top of gVisor Netstack.

The target is deliberately narrower than a general-purpose Linux Kernel Library:

- POSIX/Linux host only initially;
- IPv4 TCP only for the first prototype;
- native Linux delivery-rate sampling, recovery, congestion-control and pacing semantics;
- native `tcp_bbr.c` and `sch_fq` built in, unchanged;
- one virtual network device connected to tcp-shift packet I/O;
- a minimal socket API sufficient for `socket/bind/listen/accept/connect/read/write/close/setsockopt`;
- no userspace process model, filesystems, block devices, modules, netfilter, containers or general device support unless a build dependency proves unavoidable.

This branch starts from the current gVisor experiment so the existing long-fat-network benchmark remains available as a control. No further BBR behaviour patches should be added to the gVisor implementation on this branch.

## Important design decision: prune Linux, do not copy TCP files

A literal extraction of `net/ipv4/tcp*.c` is not maintainable. Linux TCP relies on kernel infrastructure including `struct sock`, `sk_buff`, netdevice/qdisc, softirq, timers/hrtimers, RCU, workqueues, memory allocators, wait queues, locking and scheduler/thread primitives.

Instead, treat the runtime as a tiny Linux architecture/host port:

```text
                    tcp-shift
                        |
                 runtime control
                        |
             +----------v-----------+
             | Linux TCP runtime    |
             |                      |
             | net/core             |
             | net/ipv4 + TCP       |
             | tcp_rate.c           |
             | tcp_recovery.c       |
             | tcp_bbr.c            |
             | sch_fq               |
             | socket/syscall glue  |
             +----------+-----------+
                        |
                virtual netdevice
                        |
                host packet backend
                        |
                     TAP/TUN
```

The Linux source tree remains upstream-owned. tcp-shift should carry only:

1. a small architecture/host adaptation layer;
2. a networking-only Kconfig fragment;
3. a virtual netdevice backend;
4. a thin exported userspace API;
5. build scripts and narrowly-scoped compatibility patches when unavoidable.

The intended maintenance model should resemble the current gVisor source staging: fetch a pinned upstream source, apply a small port layer, build an artifact, and continuously test a moving upstream revision separately.

## Why this is materially different from the gVisor BBR port

The gVisor experiment has had to recreate Linux-specific contracts around:

- per-segment delivery-rate sampling;
- app-limited state;
- packet-timed BBR rounds;
- Linux-like packets-in-flight accounting;
- recovery-entry and recovery-exit handling;
- RTO-specific BBR state;
- packet conservation;
- persistent STARTUP pacing;
- userspace pacing timer behaviour.

A Linux runtime retains these semantics at their owner. BBR should see the exact `struct tcp_sock`, `rate_sample`, CA state, packet scheduler and timer behaviour it was written against.

## Runtime boundary

### Linux code that is expected to remain

At minimum:

- kernel scheduler/thread primitives required by the architecture port;
- timer/hrtimer/timekeeping infrastructure;
- softirq/tasklet/workqueue pieces required by networking;
- RCU and locking primitives;
- slab/page allocation needed by socket and skb allocation;
- socket core and networking core;
- netdevice and qdisc core;
- IPv4 and TCP;
- SACK/recovery/RACK code selected by the target Linux version;
- delivery-rate estimator;
- `TCP_CONG_BBR`;
- `NET_SCH_FQ`;
- one virtual netdevice implementation.

### Features to exclude from v0

- IPv6;
- UDP as a public API (kernel-internal dependencies may remain);
- netfilter/nftables;
- namespaces/containers;
- eBPF;
- loadable modules;
- block devices;
- real hardware drivers;
- filesystems except pseudo-filesystem/sysctl infrastructure that the networking stack proves to require;
- SMP initially;
- checkpoint/restore.

The correct criterion is not source-directory purity. If TCP needs a generic kernel primitive, keep that primitive. The optimization target is a small stable host boundary, not the minimum possible object size.

## Host ABI

The first implementation should be POSIX-only and intentionally smaller than a cross-platform LKL host interface. Expected operations are roughly:

```c
struct tshift_host_ops {
    void (*print)(const char *buf, size_t len);
    void (*panic)(void);

    void *(*alloc)(size_t size);
    void (*free)(void *ptr);

    uint64_t (*clock_monotonic_ns)(void);
    void *(*timer_alloc)(void (*fn)(void *), void *arg);
    int (*timer_arm_oneshot)(void *timer, uint64_t delta_ns);
    void (*timer_free)(void *timer);

    void *(*mutex_alloc)(void);
    void (*mutex_lock)(void *);
    void (*mutex_unlock)(void *);

    void *(*sem_alloc)(unsigned initial);
    void (*sem_up)(void *);
    void (*sem_down)(void *);

    uintptr_t (*thread_create)(void (*fn)(void *), void *arg);
    void (*thread_join)(uintptr_t thread);

    int (*net_tx)(const void *frame, size_t len);
};
```

This is a design sketch, not yet an ABI commitment. The architecture port should determine the exact primitive set.

## Packet I/O

Start with an Ethernet-like netdevice/TAP path rather than forcing an L3 TUN interface into the kernel netdevice model. This preserves normal Linux qdisc and packet scheduling semantics:

```text
Linux TCP
   |
 sch_fq
   |
 virtual Ethernet netdev
   |
 host TAP / packet queue
```

After BBR parity is established, a host-side L3 adapter may strip/add Ethernet headers for compatibility with tcp-shift's existing TUN topology.

Do not bypass `sch_fq` in the first performance prototype. Current Linux `TCP_CONG_BBR` explicitly expects the fq pacing packet scheduler, with internal per-socket hrtimer pacing as fallback.

## API integration

Two integration forms are possible.

### v0: separate C runtime process

Prefer this for the proof of concept:

```text
Go tcp-shift controller
        |
        | exec/config/control
        v
linux-tcp-runtime (C)
        |
        +-- embedded Linux networking runtime
        +-- userspace accept/read/write relay
        +-- TAP packet backend
```

Advantages:

- isolates crashes/panics during bring-up;
- avoids Go scheduler/cgo interactions in timer debugging;
- makes performance profiling simpler;
- keeps the Linux-derived runtime's licensing boundary explicit;
- allows the current Go/gVisor implementation to remain a direct control.

### later: in-process library

Only after the runtime is stable should we consider linking the kernel object into tcp-shift through cgo or a small C shim.

## Initial Kconfig target

The first networking configuration should include at least:

```text
CONFIG_EMBEDDED=y
CONFIG_NET=y
CONFIG_INET=y
CONFIG_NETDEVICES=y
CONFIG_NET_SCHED=y
CONFIG_NET_SCH_FQ=y
CONFIG_TCP_CONG_ADVANCED=y
CONFIG_TCP_CONG_BBR=y
CONFIG_DEFAULT_BBR=y
```

Everything else should be disabled with `allnoconfig`/`tinyconfig` style pruning and then added only when Kconfig or link dependencies require it.

Do not manually delete Linux source files. Let Kconfig and the linker describe the actual dependency closure.

## Milestones and stop conditions

### M0 - reference control

Build an existing LKL implementation with BBR/fq enabled and run the same long-fat benchmark. This is not the final backend; it answers whether Linux TCP in a userspace kernel runtime can reproduce the native BBR behaviour on our CI topology.

Stop and reassess the entire userspace-kernel approach if LKL itself cannot get reasonably close to the native Linux BBR baseline after timer/netdev scheduling is verified.

### M1 - minimal kernel runtime boots

A Linux source tree with the tcp-shift architecture/host port initializes and shuts down in a normal POSIX process.

Success criterion: deterministic boot/shutdown and no filesystem/network requirement yet.

### M2 - socket core and loopback TCP

Enable networking and prove an in-runtime loopback TCP connection using the exported syscall/socket API.

Success criterion: TCP transfer passes without a physical/virtual netdevice.

### M3 - virtual netdevice

Connect one Linux netdevice to a TAP/host packet queue and pass IPv4 TCP traffic through it.

Success criterion: native CUBIC transfers correctly under the existing network-namespace test topology.

### M4 - untouched Linux BBR

Enable BBR and fq without modifying Linux TCP/BBR source.

Success criterion on the existing 100 Mbit/s, ~200 ms RTT long-fat path:

- lossless model/wire throughput should be close to native Linux BBR;
- deterministic single-loss behaviour should preserve Linux recovery semantics;
- 0.1% data-loss performance should be materially closer to native BBR than the gVisor-derived implementation.

If M4 requires edits to `tcp_bbr.c`, `tcp_rate.c`, generic TCP recovery or BBR constants, stop: the architecture boundary is wrong.

### M5 - tcp-shift backend

Expose the runtime as a new engine while keeping `native` and `netstack` as controls.

## Upstream strategy

Do not fork Linux TCP files into this repository. Pin a Linux baseline and keep the port layer separately reviewable. The long-term goal is that updating Linux consists primarily of rebasing the small architecture/host/netdev layer and rerunning compatibility tests.

For early development, an existing LKL tree is a useful executable specification for architecture-port mechanics. We should reuse ideas and, where licensing permits and attribution is preserved, code from LKL's architecture/host glue rather than independently rediscovering Linux's low-level port requirements. The final tcp-shift runtime can still be much narrower than general LKL because it needs only a Linux/POSIX host and networking.

## Licensing note

Linux kernel code is GPL-2.0-family licensed, and some BBR source carries dual SPDX terms. Linking Linux kernel objects into a distributed userspace program has licensing consequences that differ from merely talking to the host kernel through syscalls. For the prototype, keep the Linux-derived runtime as a separate build artifact/process and preserve all upstream notices. Distribution/licensing policy should be reviewed before treating an in-process library configuration as a product design.

This is an engineering boundary decision, not legal advice.
