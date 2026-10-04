#include "host/firewall_internal.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TCP_SHIFT_NFT_BATCH_MAX 1024U
#define TCP_SHIFT_NFT_DESTINATION_MAX (TCP_SHIFT_FIREWALL_ADDRESS_MAX + 16U)
#define TCP_SHIFT_NFT_CTX_DEFAULT 0U

struct nft_ctx;

struct tcp_shift_nft_api {
    void *library;
    struct nft_ctx *(*ctx_new)(uint32_t flags);
    void (*ctx_free)(struct nft_ctx *ctx);
    void (*ctx_set_dry_run)(struct nft_ctx *ctx, bool dry);
    int (*ctx_buffer_output)(struct nft_ctx *ctx);
    int (*ctx_buffer_error)(struct nft_ctx *ctx);
    const char *(*ctx_get_error_buffer)(struct nft_ctx *ctx);
    int (*run_cmd_from_buffer)(struct nft_ctx *ctx, const char *buffer);
};

static int tcp_shift_nft_load_symbol(void *library,
                                     const char *name,
                                     void *target,
                                     size_t target_size)
{
    void *symbol;

    dlerror();
    symbol = dlsym(library, name);
    if (symbol == NULL || dlerror() != NULL || target_size != sizeof(symbol)) {
        return -1;
    }
    memcpy(target, &symbol, sizeof(symbol));
    return 0;
}

static int tcp_shift_nft_api_open(struct tcp_shift_nft_api *api)
{
    static const char *const libraries[] = {
        "libnftables.so.1",
        "libnftables.so",
    };
    size_t index;

    memset(api, 0, sizeof(*api));
    for (index = 0U; index < sizeof(libraries) / sizeof(libraries[0]); index++) {
        api->library = dlopen(libraries[index], RTLD_NOW | RTLD_LOCAL);
        if (api->library != NULL) {
            break;
        }
    }
    if (api->library == NULL) {
        return 0;
    }

#define TCP_SHIFT_NFT_LOAD(field, symbol_name)                              \
    do {                                                                     \
        if (tcp_shift_nft_load_symbol(api->library, symbol_name,             \
                                      &api->field, sizeof(api->field)) < 0) { \
            dlclose(api->library);                                           \
            memset(api, 0, sizeof(*api));                                    \
            return 0;                                                        \
        }                                                                    \
    } while (0)

    TCP_SHIFT_NFT_LOAD(ctx_new, "nft_ctx_new");
    TCP_SHIFT_NFT_LOAD(ctx_free, "nft_ctx_free");
    TCP_SHIFT_NFT_LOAD(ctx_set_dry_run, "nft_ctx_set_dry_run");
    TCP_SHIFT_NFT_LOAD(ctx_buffer_output, "nft_ctx_buffer_output");
    TCP_SHIFT_NFT_LOAD(ctx_buffer_error, "nft_ctx_buffer_error");
    TCP_SHIFT_NFT_LOAD(ctx_get_error_buffer, "nft_ctx_get_error_buffer");
    TCP_SHIFT_NFT_LOAD(run_cmd_from_buffer, "nft_run_cmd_from_buffer");

#undef TCP_SHIFT_NFT_LOAD
    return 1;
}

static void tcp_shift_nft_api_close(struct tcp_shift_nft_api *api)
{
    if (api->library != NULL) {
        dlclose(api->library);
    }
    memset(api, 0, sizeof(*api));
}

static int tcp_shift_nft_permission_error(int saved_errno,
                                          const char *message)
{
    if (saved_errno == EPERM || saved_errno == EACCES) {
        return 1;
    }
    if (message != NULL &&
        (strstr(message, "Operation not permitted") != NULL ||
         strstr(message, "Permission denied") != NULL)) {
        return 1;
    }
    return 0;
}

int tcp_shift_firewall_nft_probe(void)
{
    struct tcp_shift_nft_api api;
    struct nft_ctx *ctx;
    const char *message;
    int saved_errno;
    int result;

    result = tcp_shift_nft_api_open(&api);
    if (result <= 0) {
        return result;
    }

    ctx = api.ctx_new(TCP_SHIFT_NFT_CTX_DEFAULT);
    if (ctx == NULL) {
        tcp_shift_nft_api_close(&api);
        errno = ENOMEM;
        return -1;
    }
    if (api.ctx_buffer_output(ctx) != 0 ||
        api.ctx_buffer_error(ctx) != 0) {
        api.ctx_free(ctx);
        tcp_shift_nft_api_close(&api);
        errno = ENOMEM;
        return -1;
    }

    errno = 0;
    result = api.run_cmd_from_buffer(ctx, "list tables");
    saved_errno = errno;
    message = api.ctx_get_error_buffer(ctx);

    if (result == 0) {
        api.ctx_free(ctx);
        tcp_shift_nft_api_close(&api);
        return 1;
    }
    if (tcp_shift_nft_permission_error(saved_errno, message) != 0) {
        api.ctx_free(ctx);
        tcp_shift_nft_api_close(&api);
        errno = saved_errno != 0 ? saved_errno : EPERM;
        return -1;
    }

    api.ctx_free(ctx);
    tcp_shift_nft_api_close(&api);
    errno = 0;
    return 0;
}

static int tcp_shift_nft_run(const char *commands, int dry_run)
{
    struct tcp_shift_nft_api api;
    struct nft_ctx *ctx;
    const char *message;
    int saved_errno;
    int available;
    int result;

    available = tcp_shift_nft_api_open(&api);
    if (available <= 0) {
        errno = EOPNOTSUPP;
        return -1;
    }

    ctx = api.ctx_new(TCP_SHIFT_NFT_CTX_DEFAULT);
    if (ctx == NULL) {
        tcp_shift_nft_api_close(&api);
        errno = ENOMEM;
        return -1;
    }
    if (api.ctx_buffer_output(ctx) != 0 ||
        api.ctx_buffer_error(ctx) != 0) {
        api.ctx_free(ctx);
        tcp_shift_nft_api_close(&api);
        errno = ENOMEM;
        return -1;
    }

    api.ctx_set_dry_run(ctx, dry_run != 0);
    errno = 0;
    result = api.run_cmd_from_buffer(ctx, commands);
    saved_errno = errno;
    if (result != 0) {
        message = api.ctx_get_error_buffer(ctx);
        if (message != NULL && message[0] != '\0') {
            fprintf(stderr, "tcp-shift-firewall-nft: %s", message);
            if (message[strlen(message) - 1U] != '\n') {
                fputc('\n', stderr);
            }
        }
        if (saved_errno == 0) {
            saved_errno = EPROTO;
        }
    }

    api.ctx_free(ctx);
    tcp_shift_nft_api_close(&api);
    if (result != 0) {
        errno = saved_errno;
        return -1;
    }
    return 0;
}

int tcp_shift_firewall_nft_install(struct tcp_shift_firewall *firewall)
{
    char batch[TCP_SHIFT_NFT_BATCH_MAX];
    char destination[TCP_SHIFT_NFT_DESTINATION_MAX];
    const char *family;
    const char *selector;
    const char *l4_guard;
    int length;
    int destination_length;

    if (firewall == NULL ||
        firewall->backend != TCP_SHIFT_FIREWALL_NFTABLES) {
        errno = EINVAL;
        return -1;
    }

    if (firewall->ip_version == 4U) {
        family = "ip";
        selector = "ip";
        l4_guard = "";
        destination_length = snprintf(destination, sizeof(destination),
                                      "%s:%u", firewall->target_address,
                                      (unsigned)firewall->target_port);
    } else if (firewall->ip_version == 6U) {
        family = "ip6";
        selector = "ip6";
        l4_guard = "meta l4proto tcp ";
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

    length = snprintf(
        batch, sizeof(batch),
        "create table %s %s\n"
        "add chain %s %s prerouting { type nat hook prerouting priority -100; policy accept; }\n"
        "add rule %s %s prerouting %s daddr %s %stcp dport %u counter dnat to %s\n",
        family, firewall->resource_name,
        family, firewall->resource_name,
        family, firewall->resource_name, selector, firewall->public_address,
        l4_guard, (unsigned)firewall->public_port, destination);
    if (length < 0 || (size_t)length >= sizeof(batch)) {
        errno = EOVERFLOW;
        return -1;
    }

    if (tcp_shift_nft_run(batch, 1) < 0 ||
        tcp_shift_nft_run(batch, 0) < 0) {
        return -1;
    }
    firewall->installed = 1;
    return 0;
}

int tcp_shift_firewall_nft_remove(struct tcp_shift_firewall *firewall)
{
    char batch[128];
    const char *family;
    int length;

    if (firewall == NULL ||
        firewall->backend != TCP_SHIFT_FIREWALL_NFTABLES ||
        firewall->installed == 0) {
        errno = EINVAL;
        return -1;
    }
    if (firewall->ip_version == 4U) {
        family = "ip";
    } else if (firewall->ip_version == 6U) {
        family = "ip6";
    } else {
        errno = EAFNOSUPPORT;
        return -1;
    }

    length = snprintf(batch, sizeof(batch), "delete table %s %s\n",
                      family, firewall->resource_name);
    if (length < 0 || (size_t)length >= sizeof(batch)) {
        errno = EOVERFLOW;
        return -1;
    }
    if (tcp_shift_nft_run(batch, 0) < 0) {
        return -1;
    }

    firewall->installed = 0;
    return 0;
}
