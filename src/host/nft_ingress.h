#ifndef TCP_SHIFT_HOST_NFT_INGRESS_H
#define TCP_SHIFT_HOST_NFT_INGRESS_H

#include <stdint.h>

#define TCP_SHIFT_NFT_TABLE_NAME_MAX 64U

struct tcp_shift_nft_ingress {
    char table_name[TCP_SHIFT_NFT_TABLE_NAME_MAX];
    int installed;
};

int tcp_shift_host_ipv4_forwarding_enabled(void);

int tcp_shift_nft_ingress_install_ipv4(struct tcp_shift_nft_ingress *ingress,
                                       const char *table_name,
                                       const char *public_ipv4,
                                       uint16_t public_port,
                                       const char *target_ipv4,
                                       uint16_t target_port);

int tcp_shift_nft_ingress_remove(struct tcp_shift_nft_ingress *ingress);

#endif /* TCP_SHIFT_HOST_NFT_INGRESS_H */
