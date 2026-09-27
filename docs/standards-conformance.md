# Standards and implementation references

This document defines the reference hierarchy for protocol and congestion-control behavior in tcp-shift. It is a development rule, not a blanket claim that the complete stack is conformant to every RFC listed here.

## Reference hierarchy

1. When a published RFC normatively specifies the behavior we are implementing, the RFC is the primary source. Code and tests should cite the relevant RFC and section where practical.
2. A standards claim must be scoped to the behavior actually implemented and qualified. Do not turn an aligned subset into a whole-stack conformance claim.
3. Linux is a differential/reference implementation when an RFC exists. Linux implementation behavior becomes a primary behavioral reference only when the behavior is not normatively specified by an applicable RFC, or when tcp-shift deliberately implements an OS-level mechanism such as pacing/rate sampling rather than a wire-protocol requirement.
4. Linux-derived behavior should identify the relevant kernel subsystem/function and, for qualification evidence, the kernel version used by the test environment when that distinction matters.
5. Base TCP/IP behavior inherited unchanged from the pinned lwIP revision is upstream behavior. Project patches that alter transport semantics must state whether they are RFC-driven, Linux-derived, or project policy.
6. Project resource policy, event-loop design, memory autotuning, bounded queues, and deployment behavior are engineering policy unless a protocol standard directly governs the behavior.
7. Strong protocol claims require both deterministic contracts and live datapath qualification where the behavior depends on ACK/loss/timer/packet ordering. Differential benchmarks are evidence, not a substitute for normative contracts.

## Current standards/reference matrix

| Area | Primary reference | Current claim |
| --- | --- | --- |
| Base TCP transport | pinned upstream lwIP; applicable TCP standards including RFC 9293 | inherited upstream behavior plus a bounded project patch surface; no blanket whole-stack RFC 9293 conformance claim |
| Initial window | RFC 6928 section 2 | RFC 6928 IW formula is the default transport policy; legacy pinned-lwIP formula remains an explicit rollback profile |
| Reno congestion control | RFC 5681 plus RFC 3465 behavior where byte-counting rules apply | conventional Reno-compatible loss-based controller; provenance should remain explicit as code evolves |
| NewReno partial-ACK recovery | RFC 6582 | transport-owned RFC 6582-style continuation across partial ACKs; do not claim broader SACK/RACK recovery conformance from this alone |
| CC-side SRTT observation | RFC 6298-style estimator with Karn filtering | only the SRTT observation subset is project-owned; the transport RTO implementation remains lwIP-owned |
| Window scaling | RFC 7323 via upstream lwIP | enabled and live-qualified; tcp-shift currently advertises receive scale 0 while retaining 32-bit sender-side accounting |
| CUBIC | RFC 9438 | core CUBIC algorithm aligned with RFC 9438; Linux CUBIC is a differential reference |
| HyStart++ | RFC 9406 | RFC 9406 HyStart++ with qualified recommended constants; delivery-domain round representation remains a documented abstraction |
| Sender SACK experiment | SACK semantics from the TCP standards/upstream lwIP; project bounded recovery experiment | experimental/default-OFF; not an RFC 6675 conformance claim |
| RACK-TLP | RFC 8985; RFC 2883 for D-SACK interpretation | experimental/default-OFF RFC-driven implementation; core math, delivery ordering, reordering timer integration, timer-driven repair, live tail-loss TLP, deterministic lost-retransmission repair, below-window reordering tolerance, and D-SACK-driven reordering-window adaptation are present, but production qualification is not complete |
| BBR | Linux mainline BBRv1 behavior; current IETF BBR draft as secondary semantic reference | compact BBRv1-style controller with selected independently justified newer semantics; never describe it as Linux-BBR-equivalent or RFC-conformant |
| Generic TCP pacing | Linux TCP pacing behavior | Linux-derived implementation mechanism, not an RFC protocol requirement |
| Delivery-rate sampling / app-limited accounting | Linux TCP/BBR rate-sampling semantics | Linux-derived transport observation mechanism used by internal BBR qualification |
| Pacing quantum/batching | Linux TCP TSO/autosize behavior as a reference | Linux-shaped userspace batching with project bounds; not a wire-standard behavior |
| IPv6 PTB / PMTU | RFC 8201 behavior through upstream lwIP, with tcp-shift L3 cache integration | live-qualified PTB learning and MSS adaptation; the project adapter enables upstream PMTU state on a pure L3 TUN path |
| Runtime/memory/tcp_wmem policy | project engineering policy, with selected Linux-inspired heuristics | resource-control policy; do not present these values as RFC requirements |

Normative/reference links:

- RFC 9293: https://www.rfc-editor.org/rfc/rfc9293.html
- RFC 5681: https://www.rfc-editor.org/rfc/rfc5681.html
- RFC 3465: https://www.rfc-editor.org/rfc/rfc3465.html
- RFC 6582: https://www.rfc-editor.org/rfc/rfc6582.html
- RFC 6298: https://www.rfc-editor.org/rfc/rfc6298.html
- RFC 6928: https://www.rfc-editor.org/rfc/rfc6928.html
- RFC 7323: https://www.rfc-editor.org/rfc/rfc7323.html
- RFC 8201: https://www.rfc-editor.org/rfc/rfc8201.html
- RFC 2883: https://www.rfc-editor.org/rfc/rfc2883.html
- RFC 8985: https://www.rfc-editor.org/rfc/rfc8985.html
- RFC 9406: https://www.rfc-editor.org/rfc/rfc9406.html
- RFC 9438: https://www.rfc-editor.org/rfc/rfc9438.html
- IETF BBR work: https://datatracker.ietf.org/doc/draft-ietf-ccwg-bbr/

## Claim language

Use the narrowest accurate wording.

Good examples:

- "RFC 6928 IW10 formula implemented as the default initial-window policy."
- "RFC 9438 core-algorithm aligned with RFC 9406 HyStart++."
- "RFC 6582-style NewReno partial-ACK recovery."
- "RFC 6298-style SRTT observation; RTO remains transport-owned."
- "RFC 8985 RACK-TLP experimental/default-OFF; current qualified subset documented separately."
- "BBRv1-style compact controller qualified against Linux BBR references."

Avoid:

- "fully RFC-conformant TCP stack" unless a dedicated end-to-end conformance effort proves that claim;
- "Linux BBR implementation" for tcp-shift's compact BBR controller;
- "RFC 6298 implementation" for the CC-only SRTT estimator;
- "RFC 6675 recovery" for the current bounded sender-SACK experiment.

## Change-review rule

Any future change that alters TCP congestion control, loss detection, retransmission selection, RTT/RTO behavior, window negotiation, or PMTU behavior should answer these questions in its PR:

1. What is the normative RFC, if one exists?
2. Which RFC section or algorithm step is being implemented or intentionally deviated from?
3. If Linux behavior is used, why is Linux the appropriate reference instead of an RFC?
4. What deterministic contract covers the semantic change?
5. What live datapath qualification covers timing/packet-order/recovery behavior?
6. Does the new evidence justify a stronger conformance claim, or should the existing narrower wording remain?
