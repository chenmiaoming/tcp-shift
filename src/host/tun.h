#ifndef TCP_SHIFT_HOST_TUN_H
#define TCP_SHIFT_HOST_TUN_H

#include <net/if.h>

struct tcp_shift_tun {
    int fd;
    char ifname[IFNAMSIZ];
};

int tcp_shift_tun_open(struct tcp_shift_tun *tun, const char *requested_name);
void tcp_shift_tun_close(struct tcp_shift_tun *tun);

#endif /* TCP_SHIFT_HOST_TUN_H */
