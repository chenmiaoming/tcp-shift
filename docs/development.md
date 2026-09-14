# Development and agent handoff contract

This repository is the durable project memory. A human or coding agent should be able to continue the work from repository state without relying on a private chat transcript.

## Read order for a new contributor

Start with:

1. `README.md` for product intent and current milestone;
2. `ARCHITECTURE.md` for current architectural truth and ownership boundaries;
3. `docs/lwip-roadmap.md` for implementation order, exit criteria, and stop conditions;
4. `docs/ci.md` for what has actually been proven and how failures are diagnosed;
5. the active file under `docs/milestones/` for detailed current work.

Historical PRs, failed CI runs, and milestone notes are evidence about design evolution, but they do not override `ARCHITECTURE.md`.

## Same-change documentation rule

Behavior-changing implementation work must update the relevant repository memory in the same branch/PR. At minimum, ask whether the change affects:

- product scope or user-visible deployment shape (`README.md`);
- ownership, process/module boundaries, packet/stream paths, privilege, or invariants (`ARCHITECTURE.md`);
- milestone order, exit criteria, or stop criteria (`docs/lwip-roadmap.md`);
- validation, measurements, diagnostic artifacts, or retained evidence (`docs/ci.md`);
- active implementation state, unresolved risks, and handoff detail (`docs/milestones/...`).

Do not treat a green workflow as a substitute for documentation, and do not mark a behavior as qualified until a retained CI run or an explicitly recorded external test proves it.

## Milestone completion rule

A milestone is complete only when implementation, failure-path behavior, CI evidence, and repository documentation agree. Before squash-merging a milestone PR:

- verify the latest behavior-changing commit has the relevant workflows green;
- verify later commits, if any, are documentation-only or rerun the affected workflows;
- update the active milestone and architecture documents from planned/in-progress language to the exact proven state;
- record the run IDs and important measured outputs needed by a future agent;
- leave the next milestone and any intentionally deferred work explicit.

The goal is that `main` never contains a feature whose real qualification state can only be reconstructed from the chat that produced it.

## Validation discipline

Use CI as the primary Linux integration and debugging environment when it can faithfully represent the behavior. Add diagnostics before guessing: runtime counters, packet captures, address/route state, nftables/iptables rules, conntrack entries, resource limits, memory samples, CPU work counts, and cleanup state should survive failures as artifacts.

If the target environment has behavior GitHub runners cannot represent—especially OpenVZ/container capability restrictions, provider IPv6 routing, or production kernel/network policy—document an exact manual validation command sequence and the outputs that must be returned. Do not silently generalize a runner result to an untested provider environment.

## Current handoff: P3 complete, P4 next

P3 behavior head `775ea5832f7e308e2c908c2f5abedfa4175c69be` passed run `34805306193` and established the pre-CC runner baseline. Important retained planning numbers are:

```text
warm fixed process PSS: 315 KiB
conservative idle PSS: ~0.398 KiB/flow
controlled fully-window-resident process slope: ~37.52 KiB/flow
3 x 128-flow first-to-last drained PSS growth: 5 KiB
32-MiB/25% process-budget headroom at 128 active flows: ~3074 KiB
headroom at 128 active flows: ~24.0 KiB/flow
idle CPU observation: 0 ms over 1 second
small-op runtime CPU: ~34.18 us/op for 2048 x 64-byte request/echo operations
```

These numbers describe tcp-shift process PSS on a GitHub runner. Backend Linux TCP kernel memory, backend application memory, public-client kernel memory, and provider-specific overhead are excluded. Never convert the P3 projection into a full-host connection limit without separate host/provider measurement.

P4 is allowed to start because the conservative userspace planning case has positive headroom. P4/P5 must measure their incremental fixed/per-flow/per-segment memory against P3 instead of treating CC metadata as free.

## Scope discipline

Prefer narrow milestone changes over speculative abstractions. Implement the smallest boundary needed by the current exit criteria, but keep source ownership clean enough that later extraction is possible.

In particular:

- process separation and source/library separation are independent decisions;
- the congestion-control core must be an independently buildable pure-C boundary and must remain Linux-host independent even while linked into the tcp-shift process;
- host privilege mechanisms must not leak into lwIP transport or CC APIs;
- P4 should establish generic transport events/policy outputs with a conventional controller before BBR-specific state exists;
- high-resolution delivery sampling and the process-wide pacing scheduler belong to P5, not the generic P4 policy boundary;
- no BBR implementation should begin until delivery-rate, app-limited, pacing, loss/inflight, and timestamp prerequisites are mechanically qualified;
- IPv6-only operation remains a product requirement and every later public-side transport change must preserve it.

## Evidence language

Use precise claims. Distinguish `implemented`, `compiles`, `unit/contract tested`, `runner-qualified`, and `production/provider-qualified`. Include the workload and boundary when quoting memory, CPU, throughput, or packet-path results.

A failed run can be useful retained evidence when it isolates an environmental prerequisite, a harness assumption, or a real defect. Preserve the diagnosis instead of erasing failure history after the fix.
