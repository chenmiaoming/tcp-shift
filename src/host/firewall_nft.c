#include "host/firewall_internal.h"
#include "host/nft_control.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define TCP_SHIFT_NFT_BATCH_MAX 1024U
#define TCP_SHIFT_NFT_DESTINATION_MAX (TCP_SHIFT_FIREWALL_ADDRESS_MAX + 16U)

int tcp_shift_firewall_nft_probe(void)
{
    return tcp_shift_nft_control_probe();
}

static int tcp_shift_firewall_nft_run(const char *commands, int dry_run)
{
    return tcp_shift_nft_control_run(commands, dry_run, NULL, stderr);
}

int tcp_shift_firewall_nft_install(struct tcp_shift_firewall *firewall)
{
    char batch[TCP_SHIFT_NFT_BATCH_MAX];
    char destination[TCP_SHIFT_NFT_DESTINATION_MAX];
    const char *family;
    const char *selector;
    const char *l4_guard;
    int length;
    int destination_length;

    if (firewall == NULL ||
        firewall->backend != TCP_SHIFT_FIREWALL_NFTABLES) {
        errno = EINVAL;
        return -1;
    }

    if (firewall->ip_version == 4U) {
        family = "ip";
        selector = "ip";
        l4_guard = "";
        destination_length = snprintf(destination, sizeof(destination),
                                      "%s:%u", firewall->target_address,
                                      (unsigned)firewall->target_port);
    } else if (firewall->ip_version == 6U) {
        family = "ip6";
        selector = "ip6";
        l4_guard = "meta l4proto tcp ";
        destination_length = snprintf(destination, sizeof(destination),
                                      "[%s]:%u", firewall->target_address,
                                      (unsigned)firewall->target_port);
    } else {
        errno = EAFNOSUPPORT;
        return -1;
    }
    if (destination_length < 0 ||
        (size_t)destination_length >= sizeof(destination)) {
        errno = EOVERFLOW;
        return -1;
    }

    length = snprintf(
        batch, sizeof(batch),
        "create table %s %s\n"
        "add chain %s %s prerouting { type nat hook prerouting priority -100; policy accept; }\n"
        "add rule %s %s prerouting %s daddr %s %stcp dport %u counter dnat to %s\n",
        family, firewall->resource_name,
        family, firewall->resource_name,
        family, firewall->resource_name, selector, firewall->public_address,
        l4_guard, (unsigned)firewall->public_port, destination);
    if (length < 0 || (size_t)length >= sizeof(batch)) {
        errno = EOVERFLOW;
        return -1;
    }

    if (tcp_shift_firewall_nft_run(batch, 1) < 0 ||
        tcp_shift_firewall_nft_run(batch, 0) < 0) {
        return -1;
    }
    firewall->installed = 1;
    return 0;
}

int tcp_shift_firewall_nft_remove(struct tcp_shift_firewall *firewall)
{
    char batch[128];
    const char *family;
    int length;

    if (firewall == NULL ||
        firewall->backend != TCP_SHIFT_FIREWALL_NFTABLES ||
        firewall->installed == 0) {
        errno = EINVAL;
        return -1;
    }
    if (firewall->ip_version == 4U) {
        family = "ip";
    } else if (firewall->ip_version == 6U) {
        family = "ip6";
    } else {
        errno = EAFNOSUPPORT;
        return -1;
    }

    length = snprintf(batch, sizeof(batch), "delete table %s %s\n",
                      family, firewall->resource_name);
    if (length < 0 || (size_t)length >= sizeof(batch)) {
        errno = EOVERFLOW;
        return -1;
    }
    if (tcp_shift_firewall_nft_run(batch, 0) < 0) {
        return -1;
    }

    firewall->installed = 0;
    return 0;
}
