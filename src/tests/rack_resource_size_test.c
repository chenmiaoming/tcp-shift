#include "lwip/cc_adapter.h"
#include "runtime/lwip_loop.h"

#include <stdio.h>

int main(void)
{
    size_t adapter_bytes = sizeof(struct tcp_shift_lwip_cc_adapter);
    size_t loop_bytes = sizeof(struct tcp_shift_lwip_loop);
    size_t rack_state_bytes = sizeof(struct tcp_shift_rack_tlp_state);
    size_t pcb_bytes = sizeof(struct tcp_pcb);
    unsigned rack_enabled = 0U;

#if defined(TCP_SHIFT_EXPERIMENTAL_RACK_TLP) && TCP_SHIFT_EXPERIMENTAL_RACK_TLP
    rack_enabled = 1U;
#endif

    printf("rack_resource_size=ok rack_enabled=%u adapter_bytes=%zu "
           "pcb_bytes=%zu loop_bytes=%zu rack_state_bytes=%zu "
           "pacer_event_bytes=%zu\n",
           rack_enabled,
           adapter_bytes,
           pcb_bytes,
           loop_bytes,
           rack_state_bytes,
           sizeof(struct tcp_shift_pacer_event));
    return 0;
}
