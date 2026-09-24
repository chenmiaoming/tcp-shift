#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lwip/cc_adapter.h"
#include "lwip/init.h"
#include "lwip/tcp.h"

#define NSEC_PER_SEC UINT64_C(1000000000)

#define CHECK(expr)                                                           \
    do {                                                                      \
        if (!(expr)) {                                                        \
            fprintf(stderr,                                                   \
                    "cc-live-selector: check failed at %s:%d: %s\n",         \
                    __FILE__, __LINE__, #expr);                               \
            return 1;                                                         \
        }                                                                     \
    } while (0)

struct fake_pacer {
    uint64_t flow_id;
    uint64_t deadline_ns;
    uint32_t generation;
    uint32_t bytes;
    unsigned schedules;
    unsigned cancels;
};

static uint64_t now_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0U;
    }
    return (uint64_t)now.tv_sec * NSEC_PER_SEC + (uint64_t)now.tv_nsec;
}

static int fake_schedule(void *arg,
                         uint64_t flow_id,
                         uint32_t generation,
                         uint64_t deadline_ns,
                         uint32_t bytes)
{
    struct fake_pacer *pacer = arg;

    if (pacer == NULL || flow_id == 0U || generation == 0U ||
        deadline_ns == 0U || bytes == 0U) {
        return -1;
    }
    pacer->flow_id = flow_id;
    pacer->generation = generation;
    pacer->deadline_ns = deadline_ns;
    pacer->bytes = bytes;
    pacer->schedules++;
    return 0;
}

static int fake_cancel(void *arg,
                       uint64_t flow_id,
                       uint32_t generation,
                       size_t *cancelled)
{
    struct fake_pacer *pacer = arg;

    if (pacer == NULL || cancelled == NULL || flow_id != pacer->flow_id ||
        generation != pacer->generation) {
        return -1;
    }
    pacer->cancels++;
    *cancelled = 1U;
    return 0;
}

static const struct tcp_shift_lwip_cc_pacer_ops fake_pacer_ops = {
    .schedule = fake_schedule,
    .cancel = fake_cancel,
};

int main(void)
{
    struct tcp_shift_lwip_cc_adapter adapter;
    struct tcp_shift_lwip_cc_stats stats;
    struct fake_pacer pacer;
    struct tcp_pcb *pcb;
    struct timespec delay;
    uint64_t startup_pacing_rate;
    uint64_t paced_deadline;
    uint64_t ack_events_before;
    uint64_t policy_updates_before;
    uint64_t observations_before;
    uint64_t delivered_before;
    uint32_t seq = UINT32_C(200000);
    uint32_t large_segments;
    uint32_t large_cwnd;
    uint32_t large_ssthresh;
    uint32_t large_snd_wnd;
    uint32_t cwnd_before;
    uint16_t payload;
    unsigned char segment;
    unsigned char observed_segment;

    CHECK(strcmp(tcp_shift_lwip_cc_configured_controller_name(), "reno") == 0);
    CHECK(tcp_shift_lwip_cc_configure_controller("not-built") < 0);
    CHECK(strcmp(tcp_shift_lwip_cc_configured_controller_name(), "reno") == 0);
    CHECK(tcp_shift_lwip_cc_configure_controller("cubic") == 0);
    CHECK(strcmp(tcp_shift_lwip_cc_configured_controller_name(), "cubic") == 0);

    memset(&adapter, 0, sizeof(adapter));
    memset(&stats, 0, sizeof(stats));
    memset(&pacer, 0, sizeof(pacer));
    lwip_init();
    CHECK(tcp_shift_lwip_cc_configure_pacer(&fake_pacer_ops, &pacer) == 0);

    pcb = tcp_new();
    CHECK(pcb != NULL);
    CHECK(pcb->mss != 0U);
    CHECK(sizeof(tcpwnd_size_t) == sizeof(uint32_t));

    payload = pcb->mss;
    large_segments = UINT16_MAX / (uint32_t)pcb->mss + 64U;
    large_cwnd = (uint32_t)pcb->mss * large_segments;
    large_ssthresh = large_cwnd * 2U;
    large_snd_wnd = large_cwnd * 4U;
    CHECK(large_cwnd > UINT16_MAX);
    CHECK(large_ssthresh > large_cwnd);
    CHECK(large_snd_wnd > large_ssthresh);
    pcb->cwnd = (tcpwnd_size_t)large_cwnd;
    pcb->ssthresh = (tcpwnd_size_t)large_ssthresh;
    pcb->snd_wnd = (tcpwnd_size_t)large_snd_wnd;
    pcb->lastack = seq;
    pcb->snd_nxt = seq;

    /* The base adapter deliberately remains Reno-owned. Selection is a thin
     * listener/runtime layer applied after ordinary lifecycle binding. Build
     * the test window from the fixture's actual MSS so it is both >64 KiB and
     * an exact segment multiple; this keeps CUBIC Q16 rounding out of a
     * transport-width contract. */
    CHECK(tcp_shift_lwip_cc_adapter_bind(&adapter, pcb, &stats) == 0);
    CHECK(adapter.controller.ops == &tcp_shift_reno_ops);
    CHECK(adapter.pacing_flow_id != 0U);
    CHECK(adapter.pacing_generation != 0U);
    CHECK(adapter.pacing_rate_bytes_per_sec == 0U);
    CHECK((uint32_t)pcb->cwnd == large_cwnd);
    CHECK(stats.last_cwnd_bytes == large_cwnd);
    CHECK(stats.last_cwnd_bytes > UINT16_MAX);

    CHECK(tcp_shift_lwip_cc_apply_configured_controller(&adapter) == 0);
    CHECK(adapter.controller.ops == &tcp_shift_cubic_ops);
    CHECK(adapter.controller.state == &adapter.controller_state);
    CHECK((uint32_t)pcb->cwnd == large_cwnd);
    CHECK(stats.last_cwnd_bytes == large_cwnd);

    /* Production selection now installs the same uncapped generic transport
     * fallback previously exercised only by qualification. A real per-flow
     * pacer registration is required before CUBIC can advertise paced
     * HyStart++ growth (RFC 9406 L=infinity). */
    CHECK(adapter.pacing_rate_bytes_per_sec != 0U);
    CHECK(stats.pacing_last_rate_bytes_per_sec ==
          adapter.pacing_rate_bytes_per_sec);
    CHECK(adapter.controller_state.cubic.hystart_pacing_active == 1U);
    startup_pacing_rate = adapter.pacing_rate_bytes_per_sec;

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
    CHECK(stats.last_cwnd_bytes > UINT16_MAX);
    CHECK((uint32_t)pcb->cwnd == stats.last_cwnd_bytes);
    CHECK(adapter.pacing_rate_bytes_per_sec != 0U);
    CHECK(adapter.pacing_rate_bytes_per_sec < startup_pacing_rate);
    CHECK(adapter.controller_state.cubic.hystart_pacing_active == 1U);
    tcp_shift_lwip_cc_hook_segment_acked(pcb, &segment, payload);
    CHECK(stats.delivery_live_slots == 0U);

    /* Recovery partial ACKs on production Reno/CUBIC must still traverse the
     * delivery/rate observation path even when native NewReno owns cwnd policy.
     * The selector pacing wrapper must forward observation-only ACKs without
     * advancing controller ACK/policy state. */
    seq += payload;
    tcp_shift_lwip_cc_hook_segment_tx(pcb, &observed_segment, seq, payload);
    CHECK(stats.delivery_live_slots == 1U);
    delay.tv_sec = 0;
    delay.tv_nsec = 1000000L;
    CHECK(nanosleep(&delay, NULL) == 0);
    pcb->snd_nxt = seq + payload;
    pcb->lastack = seq + payload;
    ack_events_before = stats.ack_events;
    policy_updates_before = stats.policy_updates;
    observations_before = stats.ack_observation_events;
    delivered_before = stats.delivery_payload_bytes;
    cwnd_before = (uint32_t)pcb->cwnd;
    CHECK(tcp_shift_lwip_cc_hook_ack_observe(pcb, payload) != 0);
    CHECK(stats.ack_observation_events == observations_before + 1U);
    CHECK(stats.delivery_payload_bytes == delivered_before + payload);
    CHECK(stats.ack_events == ack_events_before);
    CHECK(stats.policy_updates == policy_updates_before);
    CHECK((uint32_t)pcb->cwnd == cwnd_before);
    tcp_shift_lwip_cc_hook_segment_acked(pcb, &observed_segment, payload);
    CHECK(stats.delivery_live_slots == 0U);
    CHECK(stats.delivery_metadata_misses == 0U);

    /* Prove this is not merely a published number: force the next virtual
     * deadline into the future and require the normal production hook to hand
     * it to the configured shared scheduler. */
    paced_deadline = now_ns();
    CHECK(paced_deadline != 0U);
    paced_deadline += NSEC_PER_SEC;
    adapter.pacing_next_send_ns = paced_deadline;
    CHECK(tcp_shift_lwip_cc_hook_segment_send_eligible(pcb, payload) == 0);
    CHECK(adapter.pacing_scheduled != 0U);
    CHECK(pacer.schedules == 1U);
    CHECK(pacer.flow_id == adapter.pacing_flow_id);
    CHECK(pacer.generation == adapter.pacing_generation);
    CHECK(pacer.deadline_ns == paced_deadline);
    CHECK(pacer.bytes == payload);

    tcp_shift_lwip_cc_adapter_unbind(&adapter);
    CHECK(pacer.cancels == 1U);
    CHECK(tcp_shift_lwip_cc_clear_pacer() == 0);
    tcp_abort(pcb);

    CHECK(tcp_shift_lwip_cc_configure_controller(NULL) == 0);
    CHECK(strcmp(tcp_shift_lwip_cc_configured_controller_name(), "reno") == 0);

    printf("cc_live_selector=ok selected=cubic ack_observations=%llu "
           "srtt_updates=%llu srtt_ns=%llu cwnd=%u cwnd_gt_64k=ok "
           "production_pacing=ok hystart_l=infinity scheduler_exec=ok\n",
           (unsigned long long)stats.ack_observation_events,
           (unsigned long long)stats.srtt_updates,
           (unsigned long long)stats.ack_last_smoothed_rtt_ns,
           stats.last_cwnd_bytes);
    return 0;
}
