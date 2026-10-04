#ifndef TCP_SHIFT_HOST_FIREWALL_INTERNAL_H
#define TCP_SHIFT_HOST_FIREWALL_INTERNAL_H

#include "host/firewall.h"

int tcp_shift_firewall_nft_probe(void);
int tcp_shift_firewall_nft_install(struct tcp_shift_firewall *firewall);
int tcp_shift_firewall_nft_remove(struct tcp_shift_firewall *firewall);

int tcp_shift_firewall_legacy_probe(unsigned ip_version);
int tcp_shift_firewall_legacy_install(struct tcp_shift_firewall *firewall);
int tcp_shift_firewall_legacy_remove(struct tcp_shift_firewall *firewall);

#endif /* TCP_SHIFT_HOST_FIREWALL_INTERNAL_H */
