#include "host/nft_control.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TCP_SHIFT_NFT_CTX_DEFAULT 0U

struct nft_ctx;

struct tcp_shift_nft_api {
    void *library;
    struct nft_ctx *(*ctx_new)(uint32_t flags);
    void (*ctx_free)(struct nft_ctx *ctx);
    void (*ctx_set_dry_run)(struct nft_ctx *ctx, bool dry);
    int (*ctx_buffer_output)(struct nft_ctx *ctx);
    int (*ctx_buffer_error)(struct nft_ctx *ctx);
    const char *(*ctx_get_output_buffer)(struct nft_ctx *ctx);
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
    TCP_SHIFT_NFT_LOAD(ctx_get_output_buffer, "nft_ctx_get_output_buffer");
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

static int tcp_shift_nft_prepare(struct tcp_shift_nft_api *api,
                                 struct nft_ctx **ctx)
{
    int available;

    available = tcp_shift_nft_api_open(api);
    if (available <= 0) {
        errno = EOPNOTSUPP;
        return available;
    }

    *ctx = api->ctx_new(TCP_SHIFT_NFT_CTX_DEFAULT);
    if (*ctx == NULL) {
        tcp_shift_nft_api_close(api);
        errno = ENOMEM;
        return -1;
    }
    if (api->ctx_buffer_output(*ctx) != 0 ||
        api->ctx_buffer_error(*ctx) != 0) {
        api->ctx_free(*ctx);
        *ctx = NULL;
        tcp_shift_nft_api_close(api);
        errno = ENOMEM;
        return -1;
    }
    return 1;
}

int tcp_shift_nft_control_probe(void)
{
    struct tcp_shift_nft_api api;
    struct nft_ctx *ctx = NULL;
    const char *message;
    int saved_errno;
    int permission_error;
    int prepared;
    int result;

    prepared = tcp_shift_nft_prepare(&api, &ctx);
    if (prepared <= 0) {
        return prepared;
    }

    errno = 0;
    result = api.run_cmd_from_buffer(ctx, "list tables");
    saved_errno = errno;
    message = api.ctx_get_error_buffer(ctx);
    permission_error =
        tcp_shift_nft_permission_error(saved_errno, message);

    api.ctx_free(ctx);
    tcp_shift_nft_api_close(&api);

    if (result == 0) {
        return 1;
    }
    if (permission_error != 0) {
        errno = saved_errno != 0 ? saved_errno : EPERM;
        return -1;
    }

    errno = 0;
    return 0;
}

static void tcp_shift_nft_emit(FILE *stream, const char *text)
{
    if (stream == NULL || text == NULL || text[0] == '\0') {
        return;
    }
    fputs(text, stream);
    if (text[strlen(text) - 1U] != '\n') {
        fputc('\n', stream);
    }
}

int tcp_shift_nft_control_run(const char *commands,
                              int dry_run,
                              FILE *output_stream,
                              FILE *error_stream)
{
    struct tcp_shift_nft_api api;
    struct nft_ctx *ctx = NULL;
    const char *output;
    const char *message;
    int saved_errno;
    int prepared;
    int result;

    if (commands == NULL || commands[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    prepared = tcp_shift_nft_prepare(&api, &ctx);
    if (prepared <= 0) {
        if (prepared == 0) {
            errno = EOPNOTSUPP;
        }
        return -1;
    }

    api.ctx_set_dry_run(ctx, dry_run != 0);
    errno = 0;
    result = api.run_cmd_from_buffer(ctx, commands);
    saved_errno = errno;
    output = api.ctx_get_output_buffer(ctx);
    message = api.ctx_get_error_buffer(ctx);

    tcp_shift_nft_emit(output_stream, output);
    if (result != 0) {
        tcp_shift_nft_emit(error_stream, message);
    }

    api.ctx_free(ctx);
    tcp_shift_nft_api_close(&api);

    if (result != 0) {
        errno = saved_errno != 0 ? saved_errno : EPROTO;
        return -1;
    }
    return 0;
}
