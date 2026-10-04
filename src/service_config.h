#ifndef TCP_SHIFT_SERVICE_CONFIG_H
#define TCP_SHIFT_SERVICE_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#define TCP_SHIFT_SERVICE_CC_MAX 16U
#define TCP_SHIFT_SERVICE_FIREWALL_BACKEND_MAX 16U
#define TCP_SHIFT_SERVICE_TUN_NAME_MAX 16U
#define TCP_SHIFT_SERVICE_ADDRESS_MAX 64U

struct tcp_shift_service_config {
    unsigned version;
    int ip_version;
    char cc[TCP_SHIFT_SERVICE_CC_MAX];
    char firewall_backend[TCP_SHIFT_SERVICE_FIREWALL_BACKEND_MAX];
    char tun_name[TCP_SHIFT_SERVICE_TUN_NAME_MAX];
    char tun_host_address[TCP_SHIFT_SERVICE_ADDRESS_MAX];
    char tun_host_cidr[TCP_SHIFT_SERVICE_ADDRESS_MAX];
    char tun_guest_address[TCP_SHIFT_SERVICE_ADDRESS_MAX];
    char tun_netmask[TCP_SHIFT_SERVICE_ADDRESS_MAX];
    char public_address[TCP_SHIFT_SERVICE_ADDRESS_MAX];
    uint16_t public_port;
    uint16_t backend_port;
};

int tcp_shift_service_config_load(const char *path,
                                  struct tcp_shift_service_config *config,
                                  char *error,
                                  size_t error_size);

#endif /* TCP_SHIFT_SERVICE_CONFIG_H */
