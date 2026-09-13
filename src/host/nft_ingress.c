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

static int tcp_shift_valid_ipv4(const char *text)
{
    struct in_addr address;

    if (text == NULL || inet_pton(AF_INET, text, &address) != 1) {
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

int tcp_shift_host_ipv4_forwarding_enabled(void)
{
    FILE *file;
    int value;

    file = fopen("/proc/sys/net/ipv4/ip_forward", "r");
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

int tcp_shift_nft_ingress_install_ipv4(struct tcp_shift_nft_ingress *ingress,
                                       const char *table_name,
                                       const char *public_ipv4,
                                       uint16_t public_port,
                                       const char *target_ipv4,
                                       uint16_t target_port)
{
    char batch[TCP_SHIFT_NFT_BATCH_MAX];
    int length;

    if (ingress == NULL || public_port == 0U || target_port == 0U) {
        errno = EINVAL;
        return -1;
    }
    if (ingress->installed != 0) {
        errno = EALREADY;
        return -1;
    }
    if (tcp_shift_valid_table_name(table_name) < 0 ||
        tcp_shift_valid_ipv4(public_ipv4) < 0 ||
        tcp_shift_valid_ipv4(target_ipv4) < 0) {
        return -1;
    }

    length = snprintf(batch, sizeof(batch),
                      "create table ip %s\n"
                      "add chain ip %s prerouting { type nat hook prerouting priority -100; policy accept; }\n"
                      "add rule ip %s prerouting ip daddr %s tcp dport %u counter dnat to %s:%u\n",
                      table_name, table_name, table_name, public_ipv4,
                      (unsigned)public_port, target_ipv4,
                      (unsigned)target_port);
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
    ingress->installed = 1;
    return 0;
}

int tcp_shift_nft_ingress_remove(struct tcp_shift_nft_ingress *ingress)
{
    char batch[128];
    int length;

    if (ingress == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (ingress->installed == 0) {
        return 0;
    }

    length = snprintf(batch, sizeof(batch), "delete table ip %s\n",
                      ingress->table_name);
    if (length < 0 || (size_t)length >= sizeof(batch)) {
        errno = EOVERFLOW;
        return -1;
    }
    if (tcp_shift_run_nft_batch(batch, 0) < 0) {
        return -1;
    }

    ingress->installed = 0;
    ingress->table_name[0] = '\0';
    return 0;
}
