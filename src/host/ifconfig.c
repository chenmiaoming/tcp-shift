#include "host/ifconfig.h"

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stddef.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

static int tcp_shift_ifreq_name(struct ifreq *ifr, const char *ifname)
{
    size_t len;

    if (ifr == NULL || ifname == NULL) {
        errno = EINVAL;
        return -1;
    }

    len = strlen(ifname);
    if (len == 0U || len >= IFNAMSIZ) {
        errno = ENAMETOOLONG;
        return -1;
    }

    memset(ifr, 0, sizeof(*ifr));
    memcpy(ifr->ifr_name, ifname, len + 1U);
    return 0;
}

static int tcp_shift_ifreq_ipv4(struct ifreq *ifr,
                                const char *ifname,
                                const char *address)
{
    struct sockaddr_in *sin;

    if (tcp_shift_ifreq_name(ifr, ifname) < 0) {
        return -1;
    }

    sin = (struct sockaddr_in *)&ifr->ifr_addr;
    sin->sin_family = AF_INET;
    if (inet_pton(AF_INET, address, &sin->sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int tcp_shift_host_configure_ipv4_tun(const char *ifname,
                                      const char *host_ipv4,
                                      const char *netmask,
                                      unsigned int mtu)
{
    struct ifreq ifr;
    int fd;
    int saved_errno;

    if (ifname == NULL || host_ipv4 == NULL || netmask == NULL || mtu == 0U) {
        errno = EINVAL;
        return -1;
    }

    fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }

    if (tcp_shift_ifreq_name(&ifr, ifname) < 0) {
        goto fail;
    }
    ifr.ifr_mtu = (int)mtu;
    if (ioctl(fd, SIOCSIFMTU, &ifr) < 0) {
        goto fail;
    }

    if (tcp_shift_ifreq_ipv4(&ifr, ifname, host_ipv4) < 0) {
        goto fail;
    }
    if (ioctl(fd, SIOCSIFADDR, &ifr) < 0) {
        goto fail;
    }

    if (tcp_shift_ifreq_ipv4(&ifr, ifname, netmask) < 0) {
        goto fail;
    }
    if (ioctl(fd, SIOCSIFNETMASK, &ifr) < 0) {
        goto fail;
    }

    if (tcp_shift_ifreq_name(&ifr, ifname) < 0) {
        goto fail;
    }
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0) {
        goto fail;
    }
    ifr.ifr_flags = (short)(ifr.ifr_flags | IFF_UP);
    if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0) {
        goto fail;
    }

    close(fd);
    return 0;

fail:
    saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return -1;
}
