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
