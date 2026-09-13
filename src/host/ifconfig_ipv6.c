#include "host/ifconfig.h"

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *tcp_shift_find_ip_tool(void)
{
    static const char *const candidates[] = {
        "/usr/sbin/ip",
        "/usr/bin/ip",
        "/sbin/ip",
        "/bin/ip",
    };
    size_t index;

    for (index = 0U; index < sizeof(candidates) / sizeof(candidates[0]); index++) {
        if (access(candidates[index], X_OK) == 0) {
            return candidates[index];
        }
    }
    errno = ENOENT;
    return NULL;
}

static int tcp_shift_wait_child(pid_t child)
{
    int status;
    pid_t result;

    do {
        result = waitpid(child, &status, 0);
    } while (result < 0 && errno == EINTR);

    if (result < 0) {
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        errno = EPROTO;
        return -1;
    }
    return 0;
}

static int tcp_shift_run_ip(const char *const argv[])
{
    const char *ip;
    pid_t child;

    ip = tcp_shift_find_ip_tool();
    if (ip == NULL) {
        return -1;
    }

    child = fork();
    if (child < 0) {
        return -1;
    }
    if (child == 0) {
        execv(ip, (char *const *)argv);
        _exit(127);
    }
    return tcp_shift_wait_child(child);
}

static int tcp_shift_validate_ipv6_cidr(const char *cidr)
{
    char address[INET6_ADDRSTRLEN];
    const char *slash;
    char *end = NULL;
    unsigned long prefix;
    struct in6_addr parsed;
    size_t address_len;

    if (cidr == NULL) {
        errno = EINVAL;
        return -1;
    }
    slash = strrchr(cidr, '/');
    if (slash == NULL || slash == cidr || slash[1] == '\0') {
        errno = EINVAL;
        return -1;
    }
    address_len = (size_t)(slash - cidr);
    if (address_len >= sizeof(address)) {
        errno = EINVAL;
        return -1;
    }
    memcpy(address, cidr, address_len);
    address[address_len] = '\0';
    if (inet_pton(AF_INET6, address, &parsed) != 1) {
        errno = EINVAL;
        return -1;
    }

    errno = 0;
    prefix = strtoul(slash + 1, &end, 10);
    if (errno != 0 || end == slash + 1 || *end != '\0' || prefix > 128UL) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int tcp_shift_host_configure_ipv6_tun(const char *ifname,
                                      const char *host_ipv6_cidr,
                                      unsigned int mtu)
{
    const char *link_argv[10];
    const char *addr_argv[10];
    char mtu_text[16];
    int length;

    if (ifname == NULL || ifname[0] == '\0' || strlen(ifname) >= IFNAMSIZ ||
        mtu == 0U || tcp_shift_validate_ipv6_cidr(host_ipv6_cidr) < 0) {
        errno = EINVAL;
        return -1;
    }

    length = snprintf(mtu_text, sizeof(mtu_text), "%u", mtu);
    if (length < 0 || (size_t)length >= sizeof(mtu_text)) {
        errno = EOVERFLOW;
        return -1;
    }

    link_argv[0] = "ip";
    link_argv[1] = "link";
    link_argv[2] = "set";
    link_argv[3] = "dev";
    link_argv[4] = ifname;
    link_argv[5] = "mtu";
    link_argv[6] = mtu_text;
    link_argv[7] = "up";
    link_argv[8] = NULL;
    link_argv[9] = NULL;
    if (tcp_shift_run_ip(link_argv) < 0) {
        return -1;
    }

    addr_argv[0] = "ip";
    addr_argv[1] = "-6";
    addr_argv[2] = "addr";
    addr_argv[3] = "add";
    addr_argv[4] = host_ipv6_cidr;
    addr_argv[5] = "dev";
    addr_argv[6] = ifname;
    addr_argv[7] = "nodad";
    addr_argv[8] = NULL;
    addr_argv[9] = NULL;
    return tcp_shift_run_ip(addr_argv);
}
