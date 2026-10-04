#include "host/nft_control.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TCP_SHIFT_NFT_TEST_MAX_BYTES (1024U * 1024U)

static char *read_commands(void)
{
    char *buffer;
    size_t capacity = 4096U;
    size_t length = 0U;

    buffer = malloc(capacity);
    if (buffer == NULL) {
        return NULL;
    }

    for (;;) {
        size_t available = capacity - length - 1U;
        size_t count;

        if (available == 0U) {
            char *grown;
            size_t next_capacity;

            if (capacity >= TCP_SHIFT_NFT_TEST_MAX_BYTES) {
                free(buffer);
                errno = EOVERFLOW;
                return NULL;
            }
            next_capacity = capacity * 2U;
            if (next_capacity > TCP_SHIFT_NFT_TEST_MAX_BYTES) {
                next_capacity = TCP_SHIFT_NFT_TEST_MAX_BYTES;
            }
            grown = realloc(buffer, next_capacity);
            if (grown == NULL) {
                free(buffer);
                return NULL;
            }
            buffer = grown;
            capacity = next_capacity;
            continue;
        }

        count = fread(buffer + length, 1U, available, stdin);
        length += count;
        if (count < available) {
            if (ferror(stdin) != 0) {
                int saved_errno = errno;

                free(buffer);
                errno = saved_errno != 0 ? saved_errno : EIO;
                return NULL;
            }
            break;
        }
    }

    buffer[length] = '\0';
    if (length == 0U) {
        free(buffer);
        errno = EINVAL;
        return NULL;
    }
    return buffer;
}

int main(int argc, char **argv)
{
    char *commands;
    int dry_run = 0;
    int result;

    if (argc == 2 && strcmp(argv[1], "--dry-run") == 0) {
        dry_run = 1;
    } else if (argc != 1) {
        fprintf(stderr, "usage: %s [--dry-run] < commands\n", argv[0]);
        return EXIT_FAILURE;
    }

    commands = read_commands();
    if (commands == NULL) {
        perror("read nftables commands");
        return EXIT_FAILURE;
    }

    result = tcp_shift_nft_control_run(commands, dry_run, stdout, stderr);
    free(commands);
    if (result < 0) {
        if (errno == EOPNOTSUPP) {
            fprintf(stderr, "libnftables control path unavailable\n");
        }
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
