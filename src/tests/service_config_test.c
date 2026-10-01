#define _GNU_SOURCE
#include "service_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(expression)                                                    \
    do {                                                                     \
        if (!(expression)) {                                                 \
            fprintf(stderr, "CHECK failed: %s:%d: %s\n",                   \
                    __FILE__, __LINE__, #expression);                        \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

static int load_text(const char *text,
                     struct tcp_shift_service_config *config,
                     char *error,
                     size_t error_size)
{
    char path[] = "/tmp/tcp-shift-config-XXXXXX";
    int fd = mkstemp(path);
    FILE *file;
    int result;

    CHECK(fd >= 0);
    file = fdopen(fd, "w");
    CHECK(file != NULL);
    CHECK(fputs(text, file) >= 0);
    CHECK(fclose(file) == 0);

    result = tcp_shift_service_config_load(path, config, error, error_size);
    unlink(path);
    return result;
}

int main(void)
{
    struct tcp_shift_service_config config;
    char error[256];

    CHECK(load_text(
              "version = 1\n"
              "cc = \"bbr\"\n"
              "[[forward]]\n"
              "listen = \"203.0.113.10:443\"\n"
              "backend = \"127.0.0.1:8443\"\n",
              &config, error, sizeof(error)) == 0);
    CHECK(config.ip_version == 4);
    CHECK(strcmp(config.cc, "bbr") == 0);
    CHECK(strcmp(config.public_address, "203.0.113.10") == 0);
    CHECK(config.public_port == 443U);
    CHECK(config.backend_port == 8443U);
    CHECK(strcmp(config.tun_host_cidr, "198.18.0.1/30") == 0);
    CHECK(strcmp(config.tun_netmask, "255.255.255.252") == 0);
    CHECK(strcmp(config.tun_guest_address, "198.18.0.2") == 0);

    CHECK(load_text(
              "version=1\n"
              "cc=\"cubic\"\n"
              "tun_name=\"ts6\"\n"
              "tun_host_address=\"fd00:1::1/126\"\n"
              "tun_guest_address=\"fd00:1::2\"\n"
              "[[forward]]\n"
              "listen=\"[2001:db8::10]:443\"\n"
              "backend=\"127.0.0.1:443\"\n",
              &config, error, sizeof(error)) == 0);
    CHECK(config.ip_version == 6);
    CHECK(strcmp(config.tun_host_cidr, "fd00:1::1/126") == 0);
    CHECK(strcmp(config.tun_guest_address, "fd00:1::2") == 0);

    CHECK(load_text(
              "version=1\n"
              "cc=\"reno\"\n"
              "unknown=1\n"
              "[[forward]]\n"
              "listen=\"203.0.113.1:80\"\n"
              "backend=\"127.0.0.1:80\"\n",
              &config, error, sizeof(error)) != 0);
    CHECK(strstr(error, "unknown configuration key") != NULL);

    CHECK(load_text(
              "version=1\n"
              "cc=\"reno\"\n"
              "[[forward]]\n"
              "listen=\"203.0.113.1:80\"\n"
              "backend=\"127.0.0.1:80\"\n"
              "[[forward]]\n"
              "listen=\"203.0.113.2:81\"\n"
              "backend=\"127.0.0.1:81\"\n",
              &config, error, sizeof(error)) != 0);
    CHECK(strstr(error, "exactly one") != NULL);

    CHECK(load_text(
              "version=1\n"
              "cc=\"reno\"\n"
              "[[forward]]\n"
              "listen=\"203.0.113.1:80\"\n"
              "backend=\"192.0.2.1:80\"\n",
              &config, error, sizeof(error)) != 0);
    CHECK(strstr(error, "127.0.0.1") != NULL);

    puts("service_config=ok ipv4=ok ipv6=ok strict=ok");
    return 0;
}
