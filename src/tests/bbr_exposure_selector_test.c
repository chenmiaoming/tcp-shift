#include <stdio.h>
#include <string.h>

#include "lwip/cc_adapter.h"

int main(void)
{
    const char *name;

#if defined(TCP_SHIFT_EXPERIMENTAL_BBR_EXPOSURE) && \
    TCP_SHIFT_EXPERIMENTAL_BBR_EXPOSURE
    if (tcp_shift_lwip_cc_configure_controller("bbr") != 0) {
        fprintf(stderr, "experimental BBR selector rejected bbr\n");
        return 1;
    }
    name = tcp_shift_lwip_cc_configured_controller_name();
    if (name == NULL || strcmp(name, "bbr") != 0) {
        fprintf(stderr, "experimental BBR selector name mismatch\n");
        return 1;
    }
#else
    if (tcp_shift_lwip_cc_configure_controller("bbr") == 0) {
        fprintf(stderr, "default selector unexpectedly accepted bbr\n");
        return 1;
    }
#endif

    if (tcp_shift_lwip_cc_configure_controller("reno") != 0) {
        fprintf(stderr, "selector rejected reno\n");
        return 1;
    }
    name = tcp_shift_lwip_cc_configured_controller_name();
    if (name == NULL || strcmp(name, "reno") != 0) {
        fprintf(stderr, "reno selector name mismatch\n");
        return 1;
    }

    if (tcp_shift_lwip_cc_configure_controller("cubic") != 0) {
        fprintf(stderr, "selector rejected cubic\n");
        return 1;
    }
    name = tcp_shift_lwip_cc_configured_controller_name();
    if (name == NULL || strcmp(name, "cubic") != 0) {
        fprintf(stderr, "cubic selector name mismatch\n");
        return 1;
    }

    puts("bbr_exposure_selector_contract=ok");
    return 0;
}
