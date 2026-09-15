#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lwip/cc_adapter.h"
#include "lwip/init.h"
#include "lwip/tcp.h"

#define CHECK(expr)                                                           \
    do {                                                                      \
        if (!(expr)) {                                                        \
            fprintf(stderr,                                                   \
                    "cc-live-selector: check failed at %s:%d: %s\n",         \
                    __FILE__, __LINE__, #expr);                               \
            return 1;                                                         \
        }                                                                     \
    } while (0)

int main(void)
{
    struct tcp_shift_lwip_cc_adapter adapter;
    struct tcp_shift_lwip_cc_stats stats;
    struct tcp_pcb *pcb;
    struct timespec delay;
    uint32_t seq = UINT32_C(200000);
    uint16_t payload;
    unsigned char segment;

    CHECK(strcmp(tcp_shift_lwip_cc_configured_controller_name(), "reno") == 0);
    CHECK(tcp_shift_lwip_cc_configure_controller("not-built") < 0);
    CHECK(strcmp(tcp_shift_lwip_cc_configured_controller_name(), "reno") == 0);
    CHECK(tcp_shift_lwip_cc_configure_controller("cubic") == 0);
    CHECK(strcmp(tcp_shift_lwip_cc_configured_controller_name(), "cubic") == 0);

    memset(&adapter, 0, sizeof(adapter));
    memset(&stats, 0, sizeof(stats));
    lwip_init();
    pcb = tcp_new();
    CHECK(pcb != NULL);
    CHECK(pcb->mss != 0U);

    payload = pcb->mss;
    pcb->cwnd = (tcpwnd_size_t)(payload * 4U);
    pcb->ssthresh = (tcpwnd_size_t)(16U * 1024U);
    pcb->snd_wnd = (tcpwnd_size_t)(16U * 1024U);
    pcb->lastack = seq;
    pcb->snd_nxt = seq;

    /* The base adapter deliberately remains Reno-owned. Selection is a thin
     * listener/runtime layer applied after ordinary lifecycle binding. */
    CHECK(tcp_shift_lwip_cc_adapter_bind(&adapter, pcb, &stats) == 0);
    CHECK(adapter.controller.ops == &tcp_shift_reno_ops);
    CHECK(tcp_shift_lwip_cc_apply_configured_controller(&adapter) == 0);
    CHECK(adapter.controller.ops == &tcp_shift_cubic_ops);
    CHECK(adapter.controller.state == &adapter.controller_state);

    tcp_shift_lwip_cc_hook_segment_tx(pcb, &segment, seq, payload);
    CHECK(stats.delivery_first_tx_events == 1U);
    delay.tv_sec = 0;
    delay.tv_nsec = 1000000L;
    CHECK(nanosleep(&delay, NULL) == 0);
    pcb->snd_nxt = seq + payload;
    pcb->lastack = seq + payload;
    CHECK(tcp_shift_lwip_cc_hook_ack(pcb, payload) != 0);
    CHECK(adapter.controller.ops == &tcp_shift_cubic_ops);
    CHECK(stats.ack_observation_events == 1U);
    CHECK(stats.srtt_updates == 1U);
    CHECK(stats.ack_last_time_ns != 0U);
    CHECK(stats.ack_last_smoothed_rtt_ns != 0U);
    CHECK(stats.controller_errors == 0U);
    tcp_shift_lwip_cc_hook_segment_acked(pcb, &segment, payload);
    CHECK(stats.delivery_live_slots == 0U);

    tcp_shift_lwip_cc_adapter_unbind(&adapter);
    tcp_abort(pcb);

    CHECK(tcp_shift_lwip_cc_configure_controller(NULL) == 0);
    CHECK(strcmp(tcp_shift_lwip_cc_configured_controller_name(), "reno") == 0);

    printf("cc_live_selector=ok selected=cubic ack_observations=%llu "
           "srtt_updates=%llu srtt_ns=%llu cwnd=%u\n",
           (unsigned long long)stats.ack_observation_events,
           (unsigned long long)stats.srtt_updates,
           (unsigned long long)stats.ack_last_smoothed_rtt_ns,
           stats.last_cwnd_bytes);
    return 0;
}
