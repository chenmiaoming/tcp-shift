#include "host/tun.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/if_tun.h>
#include <stddef.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int tcp_shift_tun_open(struct tcp_shift_tun *tun, const char *requested_name)
{
    struct ifreq ifr;
    int fd;

    if (tun == NULL) {
        errno = EINVAL;
        return -1;
    }

    tun->fd = -1;
    tun->ifname[0] = '\0';

    fd = open("/dev/net/tun", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;

    if (requested_name != NULL && requested_name[0] != '\0') {
        size_t len = strlen(requested_name);

        if (len >= sizeof(ifr.ifr_name)) {
            int saved_errno = ENAMETOOLONG;
            close(fd);
            errno = saved_errno;
            return -1;
        }
        memcpy(ifr.ifr_name, requested_name, len + 1U);
    }

    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }

    tun->fd = fd;
    memcpy(tun->ifname, ifr.ifr_name, sizeof(tun->ifname));
    tun->ifname[sizeof(tun->ifname) - 1U] = '\0';
    return 0;
}

void tcp_shift_tun_close(struct tcp_shift_tun *tun)
{
    if (tun == NULL) {
        return;
    }

    if (tun->fd >= 0) {
        close(tun->fd);
    }
    tun->fd = -1;
    tun->ifname[0] = '\0';
}
