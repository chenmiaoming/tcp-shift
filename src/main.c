#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lwip/init.h"
#include "service_config.h"

static void usage(FILE *stream)
{
    fprintf(stream,
            "usage: tcp-shift --config FILE [--check]\n"
            "       tcp-shift --help\n"
            "\n"
            "Run the configured TCP termination/bridge service. The service\n"
            "configuration selects IPv4 or IPv6 from [[forward]].listen.\n"
            "\n"
            "  --config FILE   load versioned service configuration\n"
            "  --check         validate configuration without host mutation\n"
            "  --help          show this help\n");
}

static int resolve_sibling(const char *name, char *path, size_t path_size)
{
    char executable[PATH_MAX];
    char *slash;
    ssize_t length;

    length = readlink("/proc/self/exe", executable, sizeof(executable) - 1U);
    if (length < 0 || (size_t)length >= sizeof(executable)) {
        return -1;
    }
    executable[length] = '\0';
    slash = strrchr(executable, '/');
    if (slash == NULL) {
        return -1;
    }
    *slash = '\0';
    if (snprintf(path, path_size, "%s/%s", executable, name) >=
        (int)path_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int exec_service(const struct tcp_shift_service_config *config)
{
    const char *binary = config->ip_version == 4 ?
        "tcp-shift-p2" : "tcp-shift-p2-ipv6";
    const char *host = config->ip_version == 4 ?
        config->tun_host_address : config->tun_host_cidr;
    char child_path[PATH_MAX];
    char public_port[6];
    char backend_port[6];
    char *args[10];
    int saved_errno;

    (void)snprintf(public_port, sizeof(public_port), "%u",
                   (unsigned)config->public_port);
    (void)snprintf(backend_port, sizeof(backend_port), "%u",
                   (unsigned)config->backend_port);

    args[0] = (char *)binary;
    args[1] = (char *)config->tun_name;
    args[2] = (char *)config->tun_guest_address;
    if (config->ip_version == 4) {
        args[3] = (char *)config->tun_netmask;
        args[4] = (char *)host;
        args[5] = public_port;
        args[6] = backend_port;
        args[7] = (char *)config->cc;
        args[8] = (char *)config->public_address;
        args[9] = NULL;
    } else {
        args[3] = (char *)host;
        args[4] = public_port;
        args[5] = backend_port;
        args[6] = (char *)config->cc;
        args[7] = (char *)config->public_address;
        args[8] = NULL;
        args[9] = NULL;
    }

    if (resolve_sibling(binary, child_path, sizeof(child_path)) == 0) {
        execv(child_path, args);
        saved_errno = errno;
        if (saved_errno != ENOENT) {
            errno = saved_errno;
            return -1;
        }
    }

    execvp(binary, args);
    return -1;
}

int main(int argc, char **argv)
{
    static const struct option options[] = {
        { "config", required_argument, NULL, 'c' },
        { "check", no_argument, NULL, 1000 },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    struct tcp_shift_service_config config;
    const char *config_path = NULL;
    char error[256];
    int check_only = 0;
    int option;

    /* Preserve the original P0 smoke contract for the low-level CI target. */
    if (argc == 1) {
        lwip_init();
        printf("tcp-shift: lwIP %s initialized (P0)\n", LWIP_VERSION_STRING);
        return 0;
    }

    opterr = 0;
    while ((option = getopt_long(argc, argv, "c:h", options, NULL)) != -1) {
        switch (option) {
        case 'c':
            if (config_path != NULL) {
                fprintf(stderr,
                        "tcp-shift: error: --config may be specified only once\n");
                return 2;
            }
            config_path = optarg;
            break;
        case 1000:
            check_only = 1;
            break;
        case 'h':
            usage(stdout);
            return 0;
        default:
            usage(stderr);
            return 2;
        }
    }

    if (optind != argc || config_path == NULL || config_path[0] == '\0') {
        usage(stderr);
        return 2;
    }
    if (tcp_shift_service_config_load(config_path, &config,
                                      error, sizeof(error)) != 0) {
        fprintf(stderr, "tcp-shift: error: %s\n", error);
        return 1;
    }

    if (check_only != 0) {
        printf("tcp-shift: configuration ok version=%u family=ipv%d cc=%s "
               "listen=%s:%u backend=127.0.0.1:%u\n",
               config.version, config.ip_version, config.cc,
               config.public_address, (unsigned)config.public_port,
               (unsigned)config.backend_port);
        return 0;
    }

    if (exec_service(&config) < 0) {
        fprintf(stderr,
                "tcp-shift: error: starting IPv%d service failed: %s\n",
                config.ip_version, strerror(errno));
        return 1;
    }
    return 0;
}
