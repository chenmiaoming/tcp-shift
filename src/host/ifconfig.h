#ifndef TCP_SHIFT_HOST_IFCONFIG_H
#define TCP_SHIFT_HOST_IFCONFIG_H

int tcp_shift_host_configure_ipv4_tun(const char *ifname,
                                      const char *host_ipv4,
                                      const char *netmask,
                                      unsigned int mtu);

#endif /* TCP_SHIFT_HOST_IFCONFIG_H */
