#define _GNU_SOURCE
#include "service_config.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TCP_SHIFT_SERVICE_CONFIG_VERSION 1U
#define TCP_SHIFT_SERVICE_CONFIG_MAX_BYTES (1024U * 1024U)

struct tcp_shift_service_parse_state {
    unsigned have_version : 1;
    unsigned have_cc : 1;
    unsigned have_firewall_backend : 1;
    unsigned have_tun_name : 1;
    unsigned have_tun_host : 1;
    unsigned have_tun_guest : 1;
    unsigned have_forward : 1;
    unsigned have_listen : 1;
    unsigned have_backend : 1;
    unsigned in_forward : 1;
    char listen[128];
    char backend[128];
};

static int config_error(char *error, size_t error_size, const char *format, ...)
{
    va_list args;

    if (error != NULL && error_size != 0U) {
        va_start(args, format);
        (void)vsnprintf(error, error_size, format, args);
        va_end(args);
    }
    return -1;
}

static char *trim(char *text)
{
    char *end;

    while (*text != '\0' && isspace((unsigned char)*text)) {
        text++;
    }
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';
    return text;
}

static void strip_comment(char *text)
{
    int quoted = 0;
    int escaped = 0;
    char *cursor;

    for (cursor = text; *cursor != '\0'; cursor++) {
        if (quoted != 0) {
            if (escaped != 0) {
                escaped = 0;
            } else if (*cursor == '\\') {
                escaped = 1;
            } else if (*cursor == '"') {
                quoted = 0;
            }
        } else if (*cursor == '"') {
            quoted = 1;
        } else if (*cursor == '#') {
            *cursor = '\0';
            return;
        }
    }
}

static int parse_string(const char *value, char *output, size_t output_size)
{
    size_t out = 0U;
    size_t i;
    size_t length = strlen(value);

    if (length < 2U || value[0] != '"' || value[length - 1U] != '"') {
        return -1;
    }
    for (i = 1U; i + 1U < length; i++) {
        unsigned char byte = (unsigned char)value[i];

        if (byte == '\\') {
            if (++i + 1U >= length) {
                return -1;
            }
            switch (value[i]) {
            case '\\':
                byte = '\\';
                break;
            case '"':
                byte = '"';
                break;
            case 'n':
                byte = '\n';
                break;
            case 'r':
                byte = '\r';
                break;
            case 't':
                byte = '\t';
                break;
            default:
                return -1;
            }
        } else if (byte == '"' || byte < 0x20U) {
            return -1;
        }
        if (out + 1U >= output_size) {
            return -1;
        }
        output[out++] = (char)byte;
    }
    output[out] = '\0';
    return 0;
}

static int parse_unsigned(const char *value,
                          unsigned long maximum,
                          unsigned long *result)
{
    char *end = NULL;
    unsigned long parsed;

    if (value[0] == '\0' || value[0] < '0' || value[0] > '9') {
        return -1;
    }
    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == NULL || *end != '\0' || parsed > maximum) {
        return -1;
    }
    *result = parsed;
    return 0;
}

static int parse_endpoint(const char *text,
                          int *version,
                          char *address,
                          size_t address_size,
                          uint16_t *port)
{
    char raw[TCP_SHIFT_SERVICE_ADDRESS_MAX];
    const char *port_text;
    size_t address_length;
    unsigned long parsed_port;
    unsigned char packed[sizeof(struct in6_addr)];
    int family;

    if (text[0] == '[') {
        const char *closing = strchr(text, ']');

        if (closing == NULL || closing[1] != ':' || closing[2] == '\0') {
            return -1;
        }
        address_length = (size_t)(closing - text - 1);
        if (address_length == 0U || address_length >= sizeof(raw)) {
            return -1;
        }
        memcpy(raw, text + 1, address_length);
        raw[address_length] = '\0';
        port_text = closing + 2;
        *version = 6;
        family = AF_INET6;
    } else {
        const char *colon = strrchr(text, ':');

        if (colon == NULL || strchr(text, ':') != colon) {
            return -1;
        }
        address_length = (size_t)(colon - text);
        if (address_length == 0U || address_length >= sizeof(raw)) {
            return -1;
        }
        memcpy(raw, text, address_length);
        raw[address_length] = '\0';
        port_text = colon + 1;
        *version = 4;
        family = AF_INET;
    }
    if (parse_unsigned(port_text, 65535UL, &parsed_port) != 0 ||
        parsed_port == 0UL || inet_pton(family, raw, packed) != 1) {
        return -1;
    }
    if ((*version == 4 &&
         memcmp(packed, "\0\0\0\0", 4U) == 0) ||
        (*version == 6 &&
         memcmp(packed,
                "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0",
                16U) == 0)) {
        return -1;
    }
    if (inet_ntop(family, packed, address, address_size) == NULL) {
        return -1;
    }
    *port = (uint16_t)parsed_port;
    return 0;
}

static int valid_cc(const char *name)
{
    size_t i;
    size_t length = strlen(name);

    if (length == 0U || length >= TCP_SHIFT_SERVICE_CC_MAX) {
        return 0;
    }
    for (i = 0U; i < length; i++) {
        unsigned char byte = (unsigned char)name[i];

        if ((byte >= 'a' && byte <= 'z') ||
            (byte >= '0' && byte <= '9') ||
            byte == '_' || byte == '-') {
            continue;
        }
        return 0;
    }
    return 1;
}

static int valid_firewall_backend(const char *name)
{
    return strcmp(name, "auto") == 0 ||
           strcmp(name, "nftables") == 0 ||
           strcmp(name, "iptables") == 0 ||
           strcmp(name, "none") == 0;
}

static int valid_tun_name(const char *name)
{
    size_t i;
    size_t length = strlen(name);

    if (length >= TCP_SHIFT_SERVICE_TUN_NAME_MAX) {
        return 0;
    }
    for (i = 0U; i < length; i++) {
        unsigned char byte = (unsigned char)name[i];

        if (isalnum(byte) || byte == '_' || byte == '-' || byte == '.') {
            continue;
        }
        return 0;
    }
    return 1;
}

static int parse_cidr(const char *text,
                      int version,
                      char *address,
                      size_t address_size,
                      char *cidr,
                      size_t cidr_size,
                      char *netmask,
                      size_t netmask_size)
{
    char raw[TCP_SHIFT_SERVICE_ADDRESS_MAX];
    char *slash;
    unsigned long prefix;
    unsigned char packed[sizeof(struct in6_addr)];
    int family = version == 4 ? AF_INET : AF_INET6;
    unsigned long maximum = version == 4 ? 32UL : 128UL;

    if (strlen(text) >= sizeof(raw)) {
        return -1;
    }
    strcpy(raw, text);
    slash = strrchr(raw, '/');
    if (slash == NULL || slash == raw || slash[1] == '\0') {
        return -1;
    }
    *slash = '\0';
    if (parse_unsigned(slash + 1, maximum, &prefix) != 0 ||
        inet_pton(family, raw, packed) != 1 ||
        inet_ntop(family, packed, address, address_size) == NULL) {
        return -1;
    }
    if (snprintf(cidr, cidr_size, "%s/%lu", address, prefix) >=
        (int)cidr_size) {
        return -1;
    }
    if (version == 4) {
        struct in_addr mask_addr;
        uint32_t mask =
            prefix == 0UL ? 0U :
            (0xffffffffU << (32U - (unsigned)prefix));

        mask_addr.s_addr = htonl(mask);
        if (inet_ntop(AF_INET, &mask_addr, netmask, netmask_size) == NULL) {
            return -1;
        }
    } else if (netmask_size != 0U) {
        netmask[0] = '\0';
    }
    return 0;
}

static int canonical_ip(const char *text,
                        int version,
                        char *output,
                        size_t output_size)
{
    unsigned char packed[sizeof(struct in6_addr)];
    int family = version == 4 ? AF_INET : AF_INET6;

    if (inet_pton(family, text, packed) != 1 ||
        inet_ntop(family, packed, output, output_size) == NULL) {
        return -1;
    }
    return 0;
}

static int same_ip(const char *left, const char *right, int version)
{
    unsigned char a[sizeof(struct in6_addr)];
    unsigned char b[sizeof(struct in6_addr)];
    int family = version == 4 ? AF_INET : AF_INET6;
    size_t length = version == 4 ? 4U : 16U;

    if (inet_pton(family, left, a) != 1 ||
        inet_pton(family, right, b) != 1) {
        return 0;
    }
    return memcmp(a, b, length) == 0;
}

static int finalize_config(struct tcp_shift_service_config *config,
                           struct tcp_shift_service_parse_state *state,
                           char *error,
                           size_t error_size)
{
    int listen_version;
    int backend_version;
    char backend_address[TCP_SHIFT_SERVICE_ADDRESS_MAX];
    uint16_t backend_port;
    const char *default_host;
    const char *default_guest;
    char guest[TCP_SHIFT_SERVICE_ADDRESS_MAX];

    if (state->have_version == 0U) {
        return config_error(error, error_size,
                            "missing required configuration key 'version'");
    }
    if (config->version != TCP_SHIFT_SERVICE_CONFIG_VERSION) {
        return config_error(error, error_size,
                            "unsupported configuration version %u "
                            "(expected %u)",
                            config->version,
                            TCP_SHIFT_SERVICE_CONFIG_VERSION);
    }
    if (state->have_cc == 0U) {
        return config_error(error, error_size,
                            "missing required configuration key 'cc'");
    }
    if (state->have_firewall_backend == 0U) {
        strcpy(config->firewall_backend, "auto");
    }
    if (!valid_firewall_backend(config->firewall_backend)) {
        return config_error(error, error_size,
                            "firewall_backend must be one of "
                            "auto|nftables|iptables|none");
    }
    if (state->have_forward == 0U ||
        state->have_listen == 0U ||
        state->have_backend == 0U) {
        return config_error(error, error_size,
                            "configuration requires exactly one complete "
                            "[[forward]] table");
    }
    if (!valid_cc(config->cc)) {
        return config_error(error, error_size,
                            "invalid congestion-control name '%s'",
                            config->cc);
    }
    if (!valid_tun_name(config->tun_name)) {
        return config_error(error, error_size, "invalid TUN name");
    }
    if (parse_endpoint(state->listen, &listen_version,
                       config->public_address,
                       sizeof(config->public_address),
                       &config->public_port) != 0) {
        return config_error(error, error_size,
                            "forward listen must use literal IPv4:port "
                            "or [IPv6]:port syntax");
    }
    if (parse_endpoint(state->backend, &backend_version,
                       backend_address, sizeof(backend_address),
                       &backend_port) != 0 ||
        backend_version != 4 ||
        strcmp(backend_address, "127.0.0.1") != 0) {
        return config_error(error, error_size,
                            "forward backend must use 127.0.0.1:port");
    }
    config->backend_port = backend_port;
    config->ip_version = listen_version;

    if (listen_version == 4) {
        default_host = "198.18.0.1/30";
        default_guest = "198.18.0.2";
    } else {
        default_host = "fd00:198:18::1/126";
        default_guest = "fd00:198:18::2";
    }
    if (state->have_tun_host == 0U) {
        strncpy(config->tun_host_cidr, default_host,
                sizeof(config->tun_host_cidr) - 1U);
        config->tun_host_cidr[sizeof(config->tun_host_cidr) - 1U] = '\0';
    }
    if (parse_cidr(config->tun_host_cidr, listen_version,
                   config->tun_host_address,
                   sizeof(config->tun_host_address),
                   config->tun_host_cidr,
                   sizeof(config->tun_host_cidr),
                   config->tun_netmask,
                   sizeof(config->tun_netmask)) != 0) {
        return config_error(error, error_size,
                            "tun_host_address must be an IPv%d CIDR "
                            "matching the listener family",
                            listen_version);
    }
    if (state->have_tun_guest == 0U) {
        strncpy(config->tun_guest_address, default_guest,
                sizeof(config->tun_guest_address) - 1U);
        config->tun_guest_address[
            sizeof(config->tun_guest_address) - 1U] = '\0';
    }
    if (canonical_ip(config->tun_guest_address, listen_version,
                     guest, sizeof(guest)) != 0) {
        return config_error(error, error_size,
                            "tun_guest_address must be an IPv%d address "
                            "matching the listener family",
                            listen_version);
    }
    strcpy(config->tun_guest_address, guest);
    if (same_ip(config->tun_host_address,
                config->tun_guest_address,
                listen_version)) {
        return config_error(error, error_size,
                            "TUN host and guest addresses must be distinct");
    }
    return 0;
}

int tcp_shift_service_config_load(const char *path,
                                  struct tcp_shift_service_config *config,
                                  char *error,
                                  size_t error_size)
{
    struct tcp_shift_service_parse_state state;
    FILE *stream;
    char *line = NULL;
    size_t capacity = 0U;
    size_t total = 0U;
    ssize_t length;
    unsigned line_number = 0U;
    int result = -1;

    if (path == NULL || path[0] == '\0' || config == NULL) {
        return config_error(error, error_size,
                            "configuration path is invalid");
    }
    memset(config, 0, sizeof(*config));
    memset(&state, 0, sizeof(state));
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }

    stream = fopen(path, "r");
    if (stream == NULL) {
        return config_error(error, error_size,
                            "opening configuration '%s' failed: %s",
                            path, strerror(errno));
    }

    while ((length = getline(&line, &capacity, stream)) >= 0) {
        char *text;
        char *separator;
        char *key;
        char *value;
        char parsed[128];
        unsigned long number;

        line_number++;
        total += (size_t)length;
        if (total > TCP_SHIFT_SERVICE_CONFIG_MAX_BYTES) {
            config_error(error, error_size,
                         "configuration exceeds the 1 MiB limit");
            goto out;
        }
        if (memchr(line, '\0', (size_t)length) != NULL) {
            config_error(error, error_size,
                         "configuration contains a NUL byte at line %u",
                         line_number);
            goto out;
        }

        strip_comment(line);
        text = trim(line);
        if (text[0] == '\0') {
            continue;
        }
        if (strcmp(text, "[[forward]]") == 0) {
            if (state.have_forward != 0U) {
                config_error(error, error_size,
                             "configuration version 1 supports exactly "
                             "one [[forward]] table");
                goto out;
            }
            state.have_forward = 1U;
            state.in_forward = 1U;
            continue;
        }
        if (text[0] == '[') {
            config_error(error, error_size,
                         "unsupported table syntax at line %u",
                         line_number);
            goto out;
        }

        separator = strchr(text, '=');
        if (separator == NULL) {
            config_error(error, error_size,
                         "expected key = value at line %u",
                         line_number);
            goto out;
        }
        *separator = '\0';
        key = trim(text);
        value = trim(separator + 1);
        if (key[0] == '\0' || value[0] == '\0') {
            config_error(error, error_size,
                         "invalid assignment at line %u",
                         line_number);
            goto out;
        }

        if (state.in_forward != 0U) {
            if (strcmp(key, "listen") == 0) {
                if (state.have_listen != 0U ||
                    parse_string(value, state.listen,
                                 sizeof(state.listen)) != 0) {
                    config_error(error, error_size,
                                 "invalid or duplicate forward listen "
                                 "at line %u",
                                 line_number);
                    goto out;
                }
                state.have_listen = 1U;
            } else if (strcmp(key, "backend") == 0) {
                if (state.have_backend != 0U ||
                    parse_string(value, state.backend,
                                 sizeof(state.backend)) != 0) {
                    config_error(error, error_size,
                                 "invalid or duplicate forward backend "
                                 "at line %u",
                                 line_number);
                    goto out;
                }
                state.have_backend = 1U;
            } else {
                config_error(error, error_size,
                             "unknown forward key '%s' at line %u",
                             key, line_number);
                goto out;
            }
            continue;
        }

        if (strcmp(key, "version") == 0) {
            if (state.have_version != 0U ||
                parse_unsigned(value, 0xffffffffUL, &number) != 0) {
                config_error(error, error_size,
                             "invalid or duplicate version at line %u",
                             line_number);
                goto out;
            }
            config->version = (unsigned)number;
            state.have_version = 1U;
        } else if (strcmp(key, "cc") == 0) {
            if (state.have_cc != 0U ||
                parse_string(value, parsed, sizeof(parsed)) != 0 ||
                strlen(parsed) >= sizeof(config->cc)) {
                config_error(error, error_size,
                             "invalid or duplicate cc at line %u",
                             line_number);
                goto out;
            }
            strcpy(config->cc, parsed);
            state.have_cc = 1U;
        } else if (strcmp(key, "firewall_backend") == 0) {
            if (state.have_firewall_backend != 0U ||
                parse_string(value, parsed, sizeof(parsed)) != 0 ||
                strlen(parsed) >= sizeof(config->firewall_backend) ||
                !valid_firewall_backend(parsed)) {
                config_error(error, error_size,
                             "invalid or duplicate firewall_backend "
                             "at line %u",
                             line_number);
                goto out;
            }
            strcpy(config->firewall_backend, parsed);
            state.have_firewall_backend = 1U;
        } else if (strcmp(key, "tun_name") == 0) {
            if (state.have_tun_name != 0U ||
                parse_string(value, parsed, sizeof(parsed)) != 0 ||
                strlen(parsed) >= sizeof(config->tun_name)) {
                config_error(error, error_size,
                             "invalid or duplicate tun_name at line %u",
                             line_number);
                goto out;
            }
            strcpy(config->tun_name, parsed);
            state.have_tun_name = 1U;
        } else if (strcmp(key, "tun_host_address") == 0) {
            if (state.have_tun_host != 0U ||
                parse_string(value, parsed, sizeof(parsed)) != 0 ||
                strlen(parsed) >= sizeof(config->tun_host_cidr)) {
                config_error(error, error_size,
                             "invalid or duplicate tun_host_address "
                             "at line %u",
                             line_number);
                goto out;
            }
            strcpy(config->tun_host_cidr, parsed);
            state.have_tun_host = 1U;
        } else if (strcmp(key, "tun_guest_address") == 0) {
            if (state.have_tun_guest != 0U ||
                parse_string(value, parsed, sizeof(parsed)) != 0 ||
                strlen(parsed) >= sizeof(config->tun_guest_address)) {
                config_error(error, error_size,
                             "invalid or duplicate tun_guest_address "
                             "at line %u",
                             line_number);
                goto out;
            }
            strcpy(config->tun_guest_address, parsed);
            state.have_tun_guest = 1U;
        } else {
            config_error(error, error_size,
                         "unknown configuration key '%s' at line %u",
                         key, line_number);
            goto out;
        }
    }

    if (ferror(stream) != 0) {
        config_error(error, error_size,
                     "reading configuration '%s' failed: %s",
                     path, strerror(errno));
        goto out;
    }

    result = finalize_config(config, &state, error, error_size);

out:
    free(line);
    fclose(stream);
    if (result != 0) {
        memset(config, 0, sizeof(*config));
    }
    return result;
}
