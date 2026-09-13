#include "host/nft_ingress.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define TCP_SHIFT_NFT_BATCH_MAX 1024U
#define TCP_SHIFT_NFT_DESTINATION_MAX (INET6_ADDRSTRLEN + 16U)

static int tcp_shift_valid_table_name(const char *name)
{
    size_t index;
    size_t length;

    if (name == NULL) {
        errno = EINVAL;
        return -1;
    }

    length = strlen(name);
    if (length == 0U || length >= TCP_SHIFT_NFT_TABLE_NAME_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (!(isalpha((unsigned char)name[0]) || name[0] == '_')) {
        errno = EINVAL;
        return -1;
    }
    for (index = 1U; index < length; index++) {
        unsigned char byte = (unsigned char)name[index];

        if (!(isalnum(byte) || byte == '_')) {
            errno = EINVAL;
            return -1;
        }
    }
    return 0;
}

static int tcp_shift_valid_ip(const char *text, int family)
{
    unsigned char address[sizeof(struct in6_addr)];

    if (text == NULL || inet_pton(family, text, address) != 1) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static const char *tcp_shift_find_nft(void)
{
    static const char *const candidates[] = {
        "/usr/sbin/nft",
        "/usr/bin/nft",
        "/sbin/nft",
        "/bin/nft",
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

static int tcp_shift_run_nft_batch(const char *batch, int check_only)
{
    const char *nft;
    FILE *input;
    pid_t child;
    int saved_errno;

    if (batch == NULL) {
        errno = EINVAL;
        return -1;
    }

    nft = tcp_shift_find_nft();
    if (nft == NULL) {
        return -1;
    }

    input = tmpfile();
    if (input == NULL) {
        return -1;
    }
    if (fputs(batch, input) == EOF || fflush(input) != 0 ||
        fseek(input, 0L, SEEK_SET) != 0) {
        saved_errno = errno;
        fclose(input);
        errno = saved_errno;
        return -1;
    }

    child = fork();
    if (child < 0) {
        saved_errno = errno;
        fclose(input);
        errno = saved_errno;
        return -1;
    }
    if (child == 0) {
        if (dup2(fileno(input), STDIN_FILENO) < 0) {
            _exit(126);
        }
        fclose(input);
        if (check_only != 0) {
            execl(nft, "nft", "-c", "-f", "-", (char *)NULL);
        } else {
            execl(nft, "nft", "-f", "-", (char *)NULL);
        }
        _exit(127);
    }

    fclose(input);
    return tcp_shift_wait_child(child);
}

static int tcp_shift_forwarding_enabled(const char *path)
{
    FILE *file;
    int value;

    file = fopen(path, "r");
    if (file == NULL) {
        return -1;
    }
    value = fgetc(file);
    if (value == EOF) {
        fclose(file);
        errno = EIO;
        return -1;
    }
    if (fclose(file) != 0) {
        return -1;
    }
    return value == '1' ? 1 : 0;
}

int tcp_shift_host_ipv4_forwarding_enabled(void)
{
    return tcp_shift_forwarding_enabled("/proc/sys/net/ipv4/ip_forward");
}

int tcp_shift_host_ipv6_forwarding_enabled(void)
{
    return tcp_shift_forwarding_enabled(
        "/proc/sys/net/ipv6/conf/all/forwarding");
}

static int tcp_shift_nft_ingress_install(struct tcp_shift_nft_ingress *ingress,
                                         const char *table_name,
                                         unsigned ip_version,
                                         const char *public_address,
                                         uint16_t public_port,
                                         const char *target_address,
                                         uint16_t target_port)
{
    char batch[TCP_SHIFT_NFT_BATCH_MAX];
    char destination[TCP_SHIFT_NFT_DESTINATION_MAX];
    const char *family;
    const char *selector;
    const char *l4_guard;
    int address_family;
    int destination_length;
    int length;

    if (ingress == NULL || public_port == 0U || target_port == 0U) {
        errno = EINVAL;
        return -1;
    }
    if (ingress->installed != 0) {
        errno = EALREADY;
        return -1;
    }

    if (ip_version == 4U) {
        family = "ip";
        selector = "ip";
        l4_guard = "";
        address_family = AF_INET;
        destination_length = snprintf(destination, sizeof(destination),
                                      "%s:%u", target_address,
                                      (unsigned)target_port);
    } else if (ip_version == 6U) {
        family = "ip6";
        selector = "ip6";
        /* Explicit l4proto walks IPv6 extension headers before tcp dport. */
        l4_guard = "meta l4proto tcp ";
        address_family = AF_INET6;
        destination_length = snprintf(destination, sizeof(destination),
                                      "[%s]:%u", target_address,
                                      (unsigned)target_port);
    } else {
        errno = EAFNOSUPPORT;
        return -1;
    }

    if (tcp_shift_valid_table_name(table_name) < 0 ||
        tcp_shift_valid_ip(public_address, address_family) < 0 ||
        tcp_shift_valid_ip(target_address, address_family) < 0) {
        return -1;
    }
    if (destination_length < 0 ||
        (size_t)destination_length >= sizeof(destination)) {
        errno = EOVERFLOW;
        return -1;
    }

    length = snprintf(batch, sizeof(batch),
                      "create table %s %s\n"
                      "add chain %s %s prerouting { type nat hook prerouting priority -100; policy accept; }\n"
                      "add rule %s %s prerouting %s daddr %s %stcp dport %u counter dnat to %s\n",
                      family, table_name,
                      family, table_name,
                      family, table_name, selector, public_address, l4_guard,
                      (unsigned)public_port, destination);
    if (length < 0 || (size_t)length >= sizeof(batch)) {
        errno = EOVERFLOW;
        return -1;
    }

    /* Read-only validation happens before mutation. The actual batch still uses
     * exclusive `create table`, so a race or stale owner is rejected atomically. */
    if (tcp_shift_run_nft_batch(batch, 1) < 0) {
        return -1;
    }
    if (tcp_shift_run_nft_batch(batch, 0) < 0) {
        return -1;
    }

    memcpy(ingress->table_name, table_name, strlen(table_name) + 1U);
    ingress->ip_version = ip_version;
    ingress->installed = 1;
    return 0;
}

int tcp_shift_nft_ingress_install_ipv4(struct tcp_shift_nft_ingress *ingress,
                                       const char *table_name,
                                       const char *public_ipv4,
                                       uint16_t public_port,
                                       const char *target_ipv4,
                                       uint16_t target_port)
{
    return tcp_shift_nft_ingress_install(ingress, table_name, 4U, public_ipv4,
                                         public_port, target_ipv4, target_port);
}

int tcp_shift_nft_ingress_install_ipv6(struct tcp_shift_nft_ingress *ingress,
                                       const char *table_name,
                                       const char *public_ipv6,
                                       uint16_t public_port,
                                       const char *target_ipv6,
                                       uint16_t target_port)
{
    return tcp_shift_nft_ingress_install(ingress, table_name, 6U, public_ipv6,
                                         public_port, target_ipv6, target_port);
}

int tcp_shift_nft_ingress_remove(struct tcp_shift_nft_ingress *ingress)
{
    char batch[128];
    const char *family;
    int length;

    if (ingress == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (ingress->installed == 0) {
        return 0;
    }

    if (ingress->ip_version == 4U) {
        family = "ip";
    } else if (ingress->ip_version == 6U) {
        family = "ip6";
    } else {
        errno = EINVAL;
        return -1;
    }

    length = snprintf(batch, sizeof(batch), "delete table %s %s\n",
                      family, ingress->table_name);
    if (length < 0 || (size_t)length >= sizeof(batch)) {
        errno = EOVERFLOW;
        return -1;
    }
    if (tcp_shift_run_nft_batch(batch, 0) < 0) {
        return -1;
    }

    ingress->installed = 0;
    ingress->ip_version = 0U;
    ingress->table_name[0] = '\0';
    return 0;
}
