#include <stdio.h>

#include "cc/registry.h"
#include "cc/reno.h"

#define CHECK(expr)                                                           \
    do {                                                                      \
        if (!(expr)) {                                                        \
            fprintf(stderr, "cc-registry: check failed at %s:%d: %s\n",       \
                    __FILE__, __LINE__, #expr);                               \
            return 1;                                                         \
        }                                                                     \
    } while (0)

int main(void)
{
    CHECK(tcp_shift_cc_default_ops() == &tcp_shift_reno_ops);
    CHECK(tcp_shift_cc_find_ops("reno") == &tcp_shift_reno_ops);
    CHECK(tcp_shift_cc_find_ops("") == NULL);
    CHECK(tcp_shift_cc_find_ops("cubic") == NULL);
    CHECK(tcp_shift_cc_find_ops("bbr") == NULL);
    CHECK(tcp_shift_cc_find_ops("bbrv3") == NULL);
    CHECK(tcp_shift_cc_find_ops(NULL) == NULL);

    printf("cc_registry=ok default=reno available=reno unavailable=cubic,bbr,bbrv3\n");
    return 0;
}
