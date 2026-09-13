# Development and agent handoff contract

`tcp-shift` is expected to move between human developers and multiple coding agents. The repository itself is the canonical memory. Chat history is useful context but is never the only place where an architectural decision may live.

## Required reading before changing behavior

Read these files in order:

1. `README.md` for the current product front door and status.
2. `ARCHITECTURE.md` for current product ownership and hard boundaries.
3. `docs/lwip-roadmap.md` for milestone order, exit criteria, and stop criteria.
4. `docs/ci.md` for qualification and evidence requirements.
5. the active milestone document under `docs/milestones/`.

Historical commits and old experiments may explain why a choice was rejected, but current architecture wins when they conflict.

## Same-change documentation rule

A code change must update documentation in the same branch/PR when it changes any of the following:

- product CLI or deployment prerequisites;
- packet path, process ownership, lifecycle, or security boundary;
- source/module ownership;
- IPv4/IPv6 behavior;
- memory/CPU model or an advertised resource claim;
- congestion-control, pacing, sampling, or recovery semantics;
- milestone exit criteria or CI gates;
- a previously documented design decision.

Do not leave an important design decision only in a PR comment or chat response.

## Milestone record

Each active milestone document should contain:

- problem and non-goals;
- intended data/control path;
- current implementation status;
- invariants and failure behavior;
- CI/evidence required to exit;
- measured results once available;
- open questions and deliberately deferred work;
- decisions that should not be silently revisited.

When a milestone completes, mark it complete but keep the document. It is design history and future porting evidence.

## Small reversible increments

Prefer changes that establish one mechanical boundary at a time. Build new modules under `-Werror` before depending on them in the product runtime. Preserve a known-good lower milestone while privileged or destructive integration paths are still under construction.

Do not weaken a gate merely to obtain green CI. If a gate is wrong, record why it was wrong and replace it with a gate that measures the intended property.

## Portability rule for congestion control

`cc/` is intended to be pure C and independently buildable. Platform adapters provide time, transport events, and pacing execution. The CC core must not gain Linux-specific TUN, epoll, timerfd, netfilter, socket-fd, or process-lifecycle dependencies.

The immediate product remains `tcp-shift`; do not broaden scope into a general embedded networking project before the API has been proven by the VPS runtime. If the CC interface stabilizes, extracting it to a separate `lwip-cc` repository is a later packaging decision rather than a redesign.

## Handoff checklist

Before handing work to another agent, the branch should make the following discoverable without private context: what works now, what does not work yet, which CI run/evidence proves the claim, what the next smallest implementation step is, and which alternatives were considered and rejected.
