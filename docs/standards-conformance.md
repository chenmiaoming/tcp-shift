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
| Proportional Rate Reduction (PRR) | RFC 9937 (obsoletes RFC 6937) | experimental/default-OFF RACK+SACK recovery integration is live-qualified for Reno and CUBIC: the existing SACK/RACK delivery ledger supplies DeliveredData/RecoverFS/inflight evidence, SafeACK selects CRB vs SSRB, and per-transmit accounting enforces SndCnt. RACK remains the sole project-owned loss oracle/selective-repair policy; PRR only controls recovery send credit. The PRR-aware effective cwnd must be transparently forwarded through production and qualification pacing wrappers so native tcp_output sees raw sequence-space outstanding plus SndCnt; CI fails closed on this wrapper boundary. Non-RACK/non-SACK recovery retains RFC 6582 NewReno fallback, and internal BBR retains controller-owned recovery with PRR disabled. Hosted-runner qualification covers exact 28-drop Reno/CUBIC recovery, a two-loss single-flight SafeACK/SSRB case, zero RTO on those cases, and BBR PRR-inactive separation; provider/OpenVZ exposure qualification remains separate |
| CC-side SRTT observation | RFC 6298-style estimator with Karn filtering | only the SRTT observation subset is project-owned; the transport RTO implementation remains lwIP-owned |
| Window scaling | RFC 7323 via upstream lwIP | enabled and live-qualified; tcp-shift currently advertises receive scale 0 while retaining 32-bit sender-side accounting |
| CUBIC | RFC 9438 | core CUBIC algorithm aligned with RFC 9438; congestion-window growth uses RFC 9438 new-ACK semantics and does not treat pure SACK delivery as segments_acked. Linux may operationally pass newly ACKed-or-SACKed delivery through tcp_newly_delivered()/tcp_cong_control(), but that remains implementation-specific differential evidence rather than the normative tcp-shift rule. The experimental ECN path has an explicit congestion-event API, beta reduction down to 1 SMSS, and the RFC 3168 retransmit-timer gate when a fresh ECE arrives at 1 SMSS. Hosted-runner qualification covers both the end-to-end single-CE wire sequence and repeated independent CE episodes through the 1-SMSS timer gate with zero synthetic loss/RTO events. Provider/OpenVZ evidence and any default/public exposure remain separate. Linux CUBIC remains a differential reference |
| HyStart++ | RFC 9406 | RFC 9406 HyStart++ with qualified recommended constants; delivery-domain round representation remains a documented abstraction |
| TCP ECN | RFC 3168; RFC 8311 only as context for later experimental relaxations | experimental/default-OFF transport substrate: ECE/CWR negotiation/state, CE echo, first-transmission data ECT(0), retransmission/control Not-ECT, CWR-on-next-new-data, and the 1-SMSS retransmit-timer send gate are implemented. Real TUN qualification covers SYN negotiation, forced CE, ECE feedback, CWR, ECE cessation, repeated independent CE episodes at 1 SMSS, and timer-gated send/release while keeping ECN distinct from loss/RTO. Provider/OpenVZ exposure evidence remains separate; RFC 8311 relaxations are intentionally not used by this profile |
| Sender SACK evidence | SACK semantics from the TCP standards/upstream lwIP | SACK negotiation/scoreboard/delivery evidence is independently buildable as transport input for RACK; the former fixed three-later-SACK compatibility selector and its old build alias are removed and fail closed |
| RACK-TLP | RFC 8985; RFC 2883 for D-SACK interpretation | experimental/default-OFF RFC-driven implementation; RFC 8985 time evidence is the sole fast-loss oracle and RACK is the sole project-owned selective-retransmission policy; core math, mixed ACK/SACK delivery ordering, reordering timer integration, timer-driven repair, TLP/PTO tail probing, lost-retransmission repair, TLP RTO re-arm semantics, reordering/D-SACK adaptation, generation-safe timer teardown, resource-cost qualification, and deterministic Reno/CUBIC/internal-BBR recovery are covered; provider/OpenVZ production qualification and explicit exposure review remain separate |
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
- RFC 9937: https://www.rfc-editor.org/rfc/rfc9937.html
- RFC 6298: https://www.rfc-editor.org/rfc/rfc6298.html
- RFC 6928: https://www.rfc-editor.org/rfc/rfc6928.html
- RFC 7323: https://www.rfc-editor.org/rfc/rfc7323.html
- RFC 3168: https://www.rfc-editor.org/rfc/rfc3168.html
- RFC 8311: https://www.rfc-editor.org/rfc/rfc8311.html
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
- "RFC 3168 ECN transport substrate experimental/default-OFF; hosted-runner single-CE and persistent-CE/1-SMSS timer-gate paths qualified; provider/OpenVZ exposure qualification pending."
- "RFC 6582-style NewReno partial-ACK recovery."
- "RFC 9937 PRR integrated for experimental RACK+SACK Reno/CUBIC recovery; live 28-drop and SafeACK/SSRB paths runner-qualified; internal BBR excluded."
- "RFC 6298-style SRTT observation; RTO remains transport-owned."
- "RFC 8985 RACK-TLP experimental/default-OFF; current qualified subset documented separately."
- "BBRv1-style compact controller qualified against Linux BBR references."

Avoid:

- "fully RFC-conformant TCP stack" unless a dedicated end-to-end conformance effort proves that claim;
- "Linux BBR implementation" for tcp-shift's compact BBR controller;
- "RFC 6298 implementation" for the CC-only SRTT estimator;
- "RFC 6675 recovery" for tcp-shift's current recovery path; RACK-TLP is the project-owned selective-recovery implementation.

## Change-review rule

Any future change that alters TCP congestion control, loss detection, retransmission selection, RTT/RTO behavior, window negotiation, or PMTU behavior should answer these questions in its PR:

1. What is the normative RFC, if one exists?
2. Which RFC section or algorithm step is being implemented or intentionally deviated from?
3. If Linux behavior is used, why is Linux the appropriate reference instead of an RFC?
4. What deterministic contract covers the semantic change?
5. What live datapath qualification covers timing/packet-order/recovery behavior?
6. Does the new evidence justify a stronger conformance claim, or should the existing narrower wording remain?
