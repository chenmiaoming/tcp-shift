#include "host/firewall.h"
#include "host/firewall_internal.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int tcp_shift_forwarding_enabled(const char *path)
{
    FILE *file;
    int value;

    file = fopen(path, "r");
    if (file == NULL) {
        return -1;
    }
    value = fgetc(file);
    if (value == EOF) {
        fclose(file);
        errno = EIO;
        return -1;
    }
    if (fclose(file) != 0) {
        return -1;
    }
    return value == '1' ? 1 : 0;
}

int tcp_shift_host_ipv4_forwarding_enabled(void)
{
    return tcp_shift_forwarding_enabled("/proc/sys/net/ipv4/ip_forward");
}

int tcp_shift_host_ipv6_forwarding_enabled(void)
{
    return tcp_shift_forwarding_enabled(
        "/proc/sys/net/ipv6/conf/all/forwarding");
}

int tcp_shift_firewall_backend_parse(const char *text,
                                     enum tcp_shift_firewall_backend *backend)
{
    if (backend == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (text == NULL || text[0] == '\0' || strcmp(text, "auto") == 0) {
        *backend = TCP_SHIFT_FIREWALL_AUTO;
        return 0;
    }
    if (strcmp(text, "nftables") == 0) {
        *backend = TCP_SHIFT_FIREWALL_NFTABLES;
        return 0;
    }
    if (strcmp(text, "iptables") == 0) {
        *backend = TCP_SHIFT_FIREWALL_IPTABLES;
        return 0;
    }
    if (strcmp(text, "none") == 0) {
        *backend = TCP_SHIFT_FIREWALL_NONE;
        return 0;
    }
    errno = EINVAL;
    return -1;
}

const char *tcp_shift_firewall_backend_name(
    enum tcp_shift_firewall_backend backend)
{
    switch (backend) {
    case TCP_SHIFT_FIREWALL_AUTO:
        return "auto";
    case TCP_SHIFT_FIREWALL_NFTABLES:
        return "nftables";
    case TCP_SHIFT_FIREWALL_IPTABLES:
        return "iptables";
    case TCP_SHIFT_FIREWALL_NONE:
        return "none";
    default:
        return "invalid";
    }
}

static int tcp_shift_firewall_valid_resource(const char *name)
{
    size_t index;
    size_t length;

    if (name == NULL) {
        errno = EINVAL;
        return -1;
    }
    length = strlen(name);
    if (length == 0U || length >= TCP_SHIFT_FIREWALL_RESOURCE_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (!(isalpha((unsigned char)name[0]) || name[0] == '_')) {
        errno = EINVAL;
        return -1;
    }
    for (index = 1U; index < length; index++) {
        unsigned char byte = (unsigned char)name[index];

        if (!(isalnum(byte) || byte == '_')) {
            errno = EINVAL;
            return -1;
        }
    }
    return 0;
}

static int tcp_shift_firewall_valid_ip(const char *text, unsigned ip_version)
{
    unsigned char packed[sizeof(struct in6_addr)];
    int family;

    if (ip_version == 4U) {
        family = AF_INET;
    } else if (ip_version == 6U) {
        family = AF_INET6;
    } else {
        errno = EAFNOSUPPORT;
        return -1;
    }
    if (text == NULL || inet_pton(family, text, packed) != 1) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int tcp_shift_firewall_prepare(
    struct tcp_shift_firewall *firewall,
    const struct tcp_shift_firewall_spec *spec)
{
    if (firewall == NULL || spec == NULL ||
        spec->public_port == 0U || spec->target_port == 0U) {
        errno = EINVAL;
        return -1;
    }
    if (firewall->installed != 0) {
        errno = EALREADY;
        return -1;
    }
    if (tcp_shift_firewall_valid_resource(spec->resource_name) < 0 ||
        tcp_shift_firewall_valid_ip(spec->public_address, spec->ip_version) < 0 ||
        tcp_shift_firewall_valid_ip(spec->target_address, spec->ip_version) < 0) {
        return -1;
    }
    if (strlen(spec->public_address) >= sizeof(firewall->public_address) ||
        strlen(spec->target_address) >= sizeof(firewall->target_address)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    memset(firewall, 0, sizeof(*firewall));
    firewall->ip_version = spec->ip_version;
    firewall->public_port = spec->public_port;
    firewall->target_port = spec->target_port;
    strcpy(firewall->resource_name, spec->resource_name);
    strcpy(firewall->public_address, spec->public_address);
    strcpy(firewall->target_address, spec->target_address);
    return 0;
}

int tcp_shift_firewall_install(
    struct tcp_shift_firewall *firewall,
    enum tcp_shift_firewall_backend requested_backend,
    const struct tcp_shift_firewall_spec *spec)
{
    enum tcp_shift_firewall_backend selected = requested_backend;
    int probe;
    int saved_errno;

    if (tcp_shift_firewall_prepare(firewall, spec) < 0) {
        return -1;
    }

    if (selected == TCP_SHIFT_FIREWALL_NONE) {
        firewall->backend = TCP_SHIFT_FIREWALL_NONE;
        return 0;
    }

    if (selected == TCP_SHIFT_FIREWALL_AUTO ||
        selected == TCP_SHIFT_FIREWALL_NFTABLES) {
        probe = tcp_shift_firewall_nft_probe();
        if (probe < 0) {
            saved_errno = errno;
            memset(firewall, 0, sizeof(*firewall));
            errno = saved_errno;
            return -1;
        }
        if (probe > 0) {
            selected = TCP_SHIFT_FIREWALL_NFTABLES;
        } else if (requested_backend == TCP_SHIFT_FIREWALL_NFTABLES) {
            memset(firewall, 0, sizeof(*firewall));
            errno = EOPNOTSUPP;
            return -1;
        } else {
            selected = TCP_SHIFT_FIREWALL_IPTABLES;
        }
    }

    if (selected == TCP_SHIFT_FIREWALL_IPTABLES) {
        probe = tcp_shift_firewall_legacy_probe(firewall->ip_version);
        if (probe <= 0) {
            saved_errno = probe < 0 ? errno : ENOENT;
            memset(firewall, 0, sizeof(*firewall));
            errno = saved_errno;
            return -1;
        }
    }

    firewall->backend = selected;
    if (selected == TCP_SHIFT_FIREWALL_NFTABLES) {
        if (tcp_shift_firewall_nft_install(firewall) == 0) {
            return 0;
        }
    } else if (selected == TCP_SHIFT_FIREWALL_IPTABLES) {
        if (tcp_shift_firewall_legacy_install(firewall) == 0) {
            return 0;
        }
    } else {
        errno = EINVAL;
    }

    saved_errno = errno;
    memset(firewall, 0, sizeof(*firewall));
    errno = saved_errno;
    return -1;
}

int tcp_shift_firewall_remove(struct tcp_shift_firewall *firewall)
{
    if (firewall == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (firewall->installed == 0) {
        return 0;
    }
    if (firewall->backend == TCP_SHIFT_FIREWALL_NFTABLES) {
        return tcp_shift_firewall_nft_remove(firewall);
    }
    if (firewall->backend == TCP_SHIFT_FIREWALL_IPTABLES) {
        return tcp_shift_firewall_legacy_remove(firewall);
    }
    errno = EINVAL;
    return -1;
}
