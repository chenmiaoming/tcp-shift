# gVisor upstream policy

`tcp-shift` treats gVisor netstack as an upstream dependency, not as vendored source.
The project has two dependency tracks with different purposes.

## 1. Verified baseline

`.gvisor-baseline` contains the exact gVisor commit used for normal development,
benchmarks, and release candidates. A baseline is reproducible and must have passed:

- tcp-shift build and CLI smoke tests;
- gVisor CUBIC and tcp-shift BBR long-fat-network tests;
- memory/CPU/throughput reporting;
- protocol correctness tests added as the project matures.

Changing the baseline is an explicit dependency update, not an incidental side effect
of building tcp-shift.

## 2. Upstream compatibility track

CI also builds with `GVISOR_REF=master`. This resolves `google/gvisor@master` to a
concrete SHA at build time, applies the tcp-shift integration patch, builds the relay,
and records the resolved SHA.

This job is intentionally strict. If gVisor changes an internal TCP interface that
our patch depends on, CI should fail immediately. The failure is a compatibility
signal; it must not silently fall back to an older gVisor revision.

Once the upstream-master job is green and the benchmark/correctness suite is green,
that SHA can be promoted into `.gvisor-baseline`.

## Why releases are not built from a floating master

A production binary must be reproducible. Building a release from a moving `master`
would make two builds with the same tcp-shift source potentially contain different
network stacks. Therefore:

- compatibility CI follows moving upstream;
- release artifacts use a verified exact SHA;
- every binary embeds both the requested gVisor ref and the resolved gVisor SHA.

This provides continuous compatibility without sacrificing reproducibility.

## Patch-surface rule

The long-term goal is to keep tcp-shift-specific changes to gVisor TCP as small and
mechanical as possible. The preferred order is:

1. use public netstack APIs directly;
2. add generic congestion-control/pacing extension points that could plausibly be
   accepted upstream;
3. keep tcp-shift BBR implementation in a separate added source file;
4. avoid replacing large upstream functions or copying unrelated gVisor code.

The current PoC still replaces a substantial portion of `sender.sendData()` to add
pacing. This is transitional technical debt. Before a production release, it should
be reduced to narrow generic hooks (for example pre-send allowance, post-send
accounting, and a sender pacing timer) so routine gVisor sender changes do not cause
large rebases.

## Updating gVisor locally

Build the verified baseline:

```bash
bash ./scripts/build.sh
./bin/tcp-shift --version
```

Build current upstream master:

```bash
GVISOR_REF=master bash ./scripts/build.sh
./bin/tcp-shift --version
```

Test a specific gVisor commit or tag:

```bash
GVISOR_REF=<commit-or-tag> bash ./scripts/build.sh
```

If compatibility and benchmark results are acceptable, update `.gvisor-baseline` in
a dedicated dependency-update commit or pull request.

## Promotion criteria

A future automated baseline-promotion workflow may propose updates, but it should not
auto-merge them. At minimum promotion should require:

- build compatibility;
- TCP relay integration tests;
- repeated long-fat-network benchmark results;
- no material RSS regression under the 128 MiB target environment;
- no material throughput/CPU regression relative to the current baseline;
- review of gVisor TCP changes between the old and new SHAs.
