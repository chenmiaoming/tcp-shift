# Service configuration

The operator-facing service entrypoint is configuration-driven:

```bash
sudo tcp-shift --config /etc/tcp-shift/tcp-shift.toml
sudo tcp-shift --config /etc/tcp-shift/tcp-shift.toml --check
```

Configuration is loaded once at startup. There is no implicit file lookup and no
hot reload. `--check` validates the versioned service configuration without
creating a TUN interface, changing nftables state, or starting the transport
runtime.

The parser deliberately accepts a small, strict TOML-shaped version-1 schema
rather than attempting to implement the complete TOML language. Unknown keys,
duplicate keys, unsupported tables, malformed types/endpoints, additional
`[[forward]]` tables, non-loopback backends, and unsupported schema versions
fail closed. This keeps the parser dependency-free and bounded for the current
single-listener runtime. If a later service version needs general TOML features,
the intended parser dependency is the same pinned `chenmiaoming/toml-c`
implementation used by `linux-tcp-cc`.

## Version 1

```toml
version = 1
cc = "bbr"

# Optional. Empty/omitted lets the kernel allocate a TUN name.
# tun_name = "ts0"

# Optional; defaults are selected from the public listener family.
# IPv4 defaults:
# tun_host_address = "198.18.0.1/30"
# tun_guest_address = "198.18.0.2"
#
# IPv6 defaults:
# tun_host_address = "fd00:198:18::1/126"
# tun_guest_address = "fd00:198:18::2"

[[forward]]
listen = "203.0.113.10:443"
backend = "127.0.0.1:443"
```

For IPv6, use bracketed listener syntax:

```toml
version = 1
cc = "bbr"

[[forward]]
listen = "[2001:db8::10]:443"
backend = "127.0.0.1:443"
```

Version 1 keys are:

| Key | Type | Required | Meaning |
| --- | --- | --- | --- |
| `version` | integer | yes | Service schema version; must be `1`. |
| `cc` | string | yes | Public-side congestion controller: current default builds support `reno`, `cubic`, and `bbr` subject to build capability. |
| `tun_name` | string | no | Requested nonpersistent TUN name; omitted means no explicit name. |
| `tun_host_address` | string | no | Host-side TUN address in CIDR form. Must match the listener family. |
| `tun_guest_address` | string | no | lwIP-side TUN address. Must match the listener family and differ from the host address. |
| `[[forward]]` | table | yes | Exactly one public listener to loopback-backend mapping in version 1. |

The `[[forward]]` table contains exactly:

| Key | Type | Required | Meaning |
| --- | --- | --- | --- |
| `listen` | string | yes | Literal `IPv4:port` or `[IPv6]:port`. The address is the exact public DNAT destination. |
| `backend` | string | yes | Must be `127.0.0.1:port`. |

The listener address family selects the runtime automatically. Operators do not
choose separate IPv4/IPv6 binaries. The `tcp-shift` entrypoint dispatches to
the already-qualified sibling `tcp-shift-p2` or `tcp-shift-p2-ipv6`
runtime after validation.

The selected p2 runtime performs forwarding preflight before mutation, opens the
TUN path, starts the lwIP listener/bridge, installs an exclusive product-owned
nftables DNAT rule for the configured public address/port, and removes that rule
during shutdown. The public and backend TCP legs remain distinct.

Version 1 intentionally supports one `[[forward]]` table because the current
bridge owns one listener. The array-of-tables spelling is retained so a future
multi-listener increment can extend the schema without inventing a different
mapping model. A second `[[forward]]` currently fails closed rather than being
ignored.


## Firewall backend

Public ingress is configured through a backend-neutral firewall layer. The
service configuration accepts:

```toml
firewall_backend = "auto"
```

Valid values are `auto`, `nftables`, `iptables`, and `none`.

`auto` prefers the native nf_tables control path. tcp-shift loads
`libnftables` directly and submits the product-owned table/chain/rule
transaction without executing the `nft` command. If the nf_tables userspace
control library or kernel facility is unavailable, `auto` falls back to the
true legacy xtables backend. A permission error, resource collision, or rule
installation error on an available nftables backend fails closed instead of
silently installing a second legacy ruleset.

The legacy backend deliberately rejects `iptables-nft` as a fallback. It uses
`iptables-legacy` / `ip6tables-legacy` (or a plain iptables binary only when
its version reports a legacy backend) and owns a dedicated chain plus one exact
PREROUTING jump. `none` leaves DNAT entirely to an external firewall manager.

tcp-shift records only firewall objects it successfully created. Partial legacy
installation is rolled back immediately. On normal shutdown and on graceful
`SIGINT`, `SIGTERM`, `SIGHUP`, or `SIGQUIT`, the runtime removes the
owned nftables table or the exact legacy jump/chain before tearing down the TUN.
Foreign firewall state is never adopted or flushed. As with any userspace
cleanup, `SIGKILL`, kernel panic, or power loss cannot run the teardown path;
a stale owned-name collision therefore remains fail-closed on the next start.

## Transport memory overrides

Transport sender memory is runtime policy and is not capped by the lwIP
compile-time `TCP_SND_BUF` reference value.

Without an override, `tcp_wmem` starts at `4K,32K`. Its automatic
maximum is the representable `tcp_mem.high` value rather than a fixed byte
ceiling. Actual per-flow growth is demand-driven from the congestion-control
window: the generic transport policy targets `2*cwnd`, while compact BBR
publishes its existing `3*cwnd` sender-buffer expansion hint. Aggregate queued
payload remains bounded by the global `tcp_mem` pressure/high-water policy.

This matters on high-BDP paths. A 1 Gbit/s path at 200 ms RTT has a 25 MB BDP;
a 2x-BDP sender allowance is about 50 MB (47.7 MiB). The automatic 512 MiB-host
profile has `tcp_mem.high = tcp_wmem.max = 48 MiB`, so it no longer fails
simply because of an unrelated 4 MiB sender cap. Smaller memory-constrained
hosts still remain subject to their global `tcp_mem` budget.

Operators can override the runtime triplets with environment variables:

```bash
sudo env \
  TCP_SHIFT_TCP_WMEM=4K,32K,64M \
  TCP_SHIFT_TCP_MEM=24M,32M,48M \
  tcp-shift --config /etc/tcp-shift/tcp-shift.toml
```

`TCP_SHIFT_TCP_WMEM` is `min,initial,max` and requires
`0 < min <= initial <= max`. Explicit values may exceed 4 MiB; the byte fields
are represented by the 32-bit lwIP sender-window type. `TCP_SHIFT_TCP_MEM` is
`low,pressure,high` and requires `low <= pressure <= high`.
`TCP_SHIFT_TCP_MEM=auto` retains the effective-memory-derived global policy.

Pinned lwIP still has a 16-bit `snd_queuelen` pbuf counter. tcp-shift configures
that counter to its largest structurally safe value; it is a pbuf-count safety
bound rather than a compile-time byte ceiling on `tcp_wmem.max`.
