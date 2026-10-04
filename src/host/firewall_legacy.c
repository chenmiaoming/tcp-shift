#include "host/firewall_internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define TCP_SHIFT_LEGACY_VERSION_MAX 256U
#define TCP_SHIFT_LEGACY_DESTINATION_MAX (TCP_SHIFT_FIREWALL_ADDRESS_MAX + 16U)

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

static int tcp_shift_legacy_version_is_nf_tables(const char *path)
{
    char buffer[TCP_SHIFT_LEGACY_VERSION_MAX];
    int fds[2];
    ssize_t total = 0;
    ssize_t count;
    pid_t child;

    if (pipe(fds) < 0) {
        return -1;
    }
    child = fork();
    if (child < 0) {
        int saved_errno = errno;

        close(fds[0]);
        close(fds[1]);
        errno = saved_errno;
        return -1;
    }
    if (child == 0) {
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0 ||
            dup2(fds[1], STDERR_FILENO) < 0) {
            _exit(126);
        }
        close(fds[1]);
        execl(path, path, "--version", (char *)NULL);
        _exit(127);
    }

    close(fds[1]);
    while ((size_t)total + 1U < sizeof(buffer)) {
        count = read(fds[0], buffer + total,
                     sizeof(buffer) - (size_t)total - 1U);
        if (count > 0) {
            total += count;
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    close(fds[0]);
    if (tcp_shift_wait_child(child) < 0) {
        return -1;
    }
    buffer[total] = '\0';
    return strstr(buffer, "nf_tables") != NULL ? 1 : 0;
}

static const char *tcp_shift_legacy_find_tool(unsigned ip_version)
{
    static const char *const ipv4_legacy[] = {
        "/usr/sbin/iptables-legacy",
        "/usr/bin/iptables-legacy",
        "/sbin/iptables-legacy",
        "/bin/iptables-legacy",
    };
    static const char *const ipv6_legacy[] = {
        "/usr/sbin/ip6tables-legacy",
        "/usr/bin/ip6tables-legacy",
        "/sbin/ip6tables-legacy",
        "/bin/ip6tables-legacy",
    };
    static const char *const ipv4_plain[] = {
        "/usr/sbin/iptables",
        "/usr/bin/iptables",
        "/sbin/iptables",
        "/bin/iptables",
    };
    static const char *const ipv6_plain[] = {
        "/usr/sbin/ip6tables",
        "/usr/bin/ip6tables",
        "/sbin/ip6tables",
        "/bin/ip6tables",
    };
    const char *const *legacy;
    const char *const *plain;
    size_t legacy_count;
    size_t plain_count;
    size_t index;
    int nft_backend;

    if (ip_version == 4U) {
        legacy = ipv4_legacy;
        legacy_count = sizeof(ipv4_legacy) / sizeof(ipv4_legacy[0]);
        plain = ipv4_plain;
        plain_count = sizeof(ipv4_plain) / sizeof(ipv4_plain[0]);
    } else if (ip_version == 6U) {
        legacy = ipv6_legacy;
        legacy_count = sizeof(ipv6_legacy) / sizeof(ipv6_legacy[0]);
        plain = ipv6_plain;
        plain_count = sizeof(ipv6_plain) / sizeof(ipv6_plain[0]);
    } else {
        errno = EAFNOSUPPORT;
        return NULL;
    }

    for (index = 0U; index < legacy_count; index++) {
        if (access(legacy[index], X_OK) == 0) {
            return legacy[index];
        }
    }

    for (index = 0U; index < plain_count; index++) {
        if (access(plain[index], X_OK) != 0) {
            continue;
        }
        nft_backend = tcp_shift_legacy_version_is_nf_tables(plain[index]);
        if (nft_backend == 0) {
            return plain[index];
        }
    }

    errno = ENOENT;
    return NULL;
}

int tcp_shift_firewall_legacy_probe(unsigned ip_version)
{
    return tcp_shift_legacy_find_tool(ip_version) != NULL ? 1 : 0;
}

static int tcp_shift_legacy_run(const char *tool, char *const argv[])
{
    pid_t child;

    child = fork();
    if (child < 0) {
        return -1;
    }
    if (child == 0) {
        execv(tool, argv);
        _exit(127);
    }
    return tcp_shift_wait_child(child);
}

static int tcp_shift_legacy_delete_jump(struct tcp_shift_firewall *firewall,
                                        const char *tool)
{
    char port[6];
    char *argv[] = {
        (char *)tool, "-w", "-t", "nat", "-D", "PREROUTING",
        "-d", firewall->public_address,
        "-p", "tcp", "--dport", port,
        "-j", firewall->resource_name,
        NULL
    };

    (void)snprintf(port, sizeof(port), "%u",
                   (unsigned)firewall->public_port);
    return tcp_shift_legacy_run(tool, argv);
}

static int tcp_shift_legacy_flush_chain(struct tcp_shift_firewall *firewall,
                                        const char *tool)
{
    char *argv[] = {
        (char *)tool, "-w", "-t", "nat", "-F", firewall->resource_name, NULL
    };

    return tcp_shift_legacy_run(tool, argv);
}

static int tcp_shift_legacy_delete_chain(struct tcp_shift_firewall *firewall,
                                         const char *tool)
{
    char *argv[] = {
        (char *)tool, "-w", "-t", "nat", "-X", firewall->resource_name, NULL
    };

    return tcp_shift_legacy_run(tool, argv);
}

static void tcp_shift_legacy_rollback(struct tcp_shift_firewall *firewall,
                                      const char *tool)
{
    if (firewall->legacy_jump_installed != 0) {
        (void)tcp_shift_legacy_delete_jump(firewall, tool);
        firewall->legacy_jump_installed = 0;
    }
    if (firewall->legacy_chain_created != 0) {
        (void)tcp_shift_legacy_flush_chain(firewall, tool);
        (void)tcp_shift_legacy_delete_chain(firewall, tool);
        firewall->legacy_chain_created = 0;
    }
    firewall->installed = 0;
}

int tcp_shift_firewall_legacy_install(struct tcp_shift_firewall *firewall)
{
    const char *tool;
    char port[6];
    char destination[TCP_SHIFT_LEGACY_DESTINATION_MAX];
    int destination_length;
    int saved_errno;
    char *create_chain[] = {
        NULL, "-w", "-t", "nat", "-N", NULL, NULL
    };
    char *add_dnat[] = {
        NULL, "-w", "-t", "nat", "-A", NULL,
        "-j", "DNAT", "--to-destination", destination, NULL
    };
    char *add_jump[] = {
        NULL, "-w", "-t", "nat", "-I", "PREROUTING", "1",
        "-d", NULL, "-p", "tcp", "--dport", port,
        "-j", NULL, NULL
    };

    if (firewall == NULL ||
        firewall->backend != TCP_SHIFT_FIREWALL_IPTABLES) {
        errno = EINVAL;
        return -1;
    }
    tool = tcp_shift_legacy_find_tool(firewall->ip_version);
    if (tool == NULL) {
        return -1;
    }

    if (firewall->ip_version == 4U) {
        destination_length = snprintf(destination, sizeof(destination),
                                      "%s:%u", firewall->target_address,
                                      (unsigned)firewall->target_port);
    } else if (firewall->ip_version == 6U) {
        destination_length = snprintf(destination, sizeof(destination),
                                      "[%s]:%u", firewall->target_address,
                                      (unsigned)firewall->target_port);
    } else {
        errno = EAFNOSUPPORT;
        return -1;
    }
    if (destination_length < 0 ||
        (size_t)destination_length >= sizeof(destination)) {
        errno = EOVERFLOW;
        return -1;
    }
    (void)snprintf(port, sizeof(port), "%u",
                   (unsigned)firewall->public_port);

    create_chain[0] = (char *)tool;
    create_chain[5] = firewall->resource_name;
    add_dnat[0] = (char *)tool;
    add_dnat[5] = firewall->resource_name;
    add_jump[0] = (char *)tool;
    add_jump[8] = firewall->public_address;
    add_jump[14] = firewall->resource_name;

    if (tcp_shift_legacy_run(tool, create_chain) < 0) {
        return -1;
    }
    firewall->legacy_chain_created = 1;

    if (tcp_shift_legacy_run(tool, add_dnat) < 0) {
        saved_errno = errno;
        tcp_shift_legacy_rollback(firewall, tool);
        errno = saved_errno;
        return -1;
    }

    if (tcp_shift_legacy_run(tool, add_jump) < 0) {
        saved_errno = errno;
        tcp_shift_legacy_rollback(firewall, tool);
        errno = saved_errno;
        return -1;
    }
    firewall->legacy_jump_installed = 1;
    firewall->installed = 1;
    return 0;
}

int tcp_shift_firewall_legacy_remove(struct tcp_shift_firewall *firewall)
{
    const char *tool;
    int chain_result;

    if (firewall == NULL ||
        firewall->backend != TCP_SHIFT_FIREWALL_IPTABLES ||
        firewall->installed == 0) {
        errno = EINVAL;
        return -1;
    }
    tool = tcp_shift_legacy_find_tool(firewall->ip_version);
    if (tool == NULL) {
        return -1;
    }

    if (firewall->legacy_jump_installed != 0) {
        (void)tcp_shift_legacy_delete_jump(firewall, tool);
        firewall->legacy_jump_installed = 0;
    }
    if (firewall->legacy_chain_created == 0) {
        firewall->installed = 0;
        return 0;
    }

    (void)tcp_shift_legacy_flush_chain(firewall, tool);
    chain_result = tcp_shift_legacy_delete_chain(firewall, tool);
    if (chain_result < 0) {
        return -1;
    }

    firewall->legacy_chain_created = 0;
    firewall->installed = 0;
    return 0;
}
