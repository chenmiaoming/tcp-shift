#ifndef TCP_SHIFT_HOST_IFCONFIG_H
#define TCP_SHIFT_HOST_IFCONFIG_H

int tcp_shift_host_configure_ipv4_tun(const char *ifname,
                                      const char *host_ipv4,
                                      const char *netmask,
                                      unsigned int mtu);

/* Configure one static IPv6 CIDR on a nonpersistent TUN. The implementation
 * uses the system ip tool only during setup; closing the TUN remains cleanup. */
int tcp_shift_host_configure_ipv6_tun(const char *ifname,
                                      const char *host_ipv6_cidr,
                                      unsigned int mtu);

#endif /* TCP_SHIFT_HOST_IFCONFIG_H */
