#ifndef TCP_SHIFT_HOST_FIREWALL_H
#define TCP_SHIFT_HOST_FIREWALL_H

#include <stdint.h>

#define TCP_SHIFT_FIREWALL_RESOURCE_MAX 64U
#define TCP_SHIFT_FIREWALL_ADDRESS_MAX 64U

enum tcp_shift_firewall_backend {
    TCP_SHIFT_FIREWALL_AUTO = 0,
    TCP_SHIFT_FIREWALL_NFTABLES,
    TCP_SHIFT_FIREWALL_IPTABLES,
    TCP_SHIFT_FIREWALL_NONE,
};

struct tcp_shift_firewall_spec {
    unsigned ip_version;
    const char *resource_name;
    const char *public_address;
    uint16_t public_port;
    const char *target_address;
    uint16_t target_port;
};

struct tcp_shift_firewall {
    enum tcp_shift_firewall_backend backend;
    unsigned ip_version;
    int installed;
    int legacy_chain_created;
    int legacy_jump_installed;
    char resource_name[TCP_SHIFT_FIREWALL_RESOURCE_MAX];
    char public_address[TCP_SHIFT_FIREWALL_ADDRESS_MAX];
    char target_address[TCP_SHIFT_FIREWALL_ADDRESS_MAX];
    uint16_t public_port;
    uint16_t target_port;
};

int tcp_shift_host_ipv4_forwarding_enabled(void);
int tcp_shift_host_ipv6_forwarding_enabled(void);

int tcp_shift_firewall_backend_parse(const char *text,
                                     enum tcp_shift_firewall_backend *backend);
const char *tcp_shift_firewall_backend_name(
    enum tcp_shift_firewall_backend backend);

int tcp_shift_firewall_install(
    struct tcp_shift_firewall *firewall,
    enum tcp_shift_firewall_backend requested_backend,
    const struct tcp_shift_firewall_spec *spec);

int tcp_shift_firewall_remove(struct tcp_shift_firewall *firewall);

#endif /* TCP_SHIFT_HOST_FIREWALL_H */
