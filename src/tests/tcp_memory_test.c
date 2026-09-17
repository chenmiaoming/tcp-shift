#include "lwip/tcp_memory.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MIB(value) ((uint64_t)(value) * 1024U * 1024U)
#define KIB(value) ((uint32_t)(value) * 1024U)

/* The policy contract links against pinned lwIP because struct tcp_pcb is part
 * of the runtime boundary. These two port hooks are irrelevant to the memory
 * policy itself; deterministic stubs keep the contract focused and standalone. */
u32_t sys_now(void)
{
    return 1U;
}

unsigned int lwip_port_rand(void)
{
    return 1U;
}

static int expect(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "tcp memory contract failed: %s\n", message);
        return -1;
    }
    return 0;
}

int main(void)
{
    struct tcp_shift_tcp_memory_config defaults;
    struct tcp_shift_tcp_memory_config small;
    struct tcp_shift_tcp_memory_manager manager;
    struct tcp_shift_tcp_memory_flow flow;
    struct tcp_shift_tcp_wmem_policy parsed_wmem;
    struct tcp_shift_tcp_mem_policy parsed_mem;
    struct tcp_pcb pcb;

    if (tcp_shift_tcp_memory_config_default(
            &defaults, MIB(512), 4096U, (uint32_t)MIB(4)) < 0 ||
        expect(defaults.wmem.min_bytes == KIB(4), "default wmem min") < 0 ||
        expect(defaults.wmem.initial_bytes == KIB(32), "default wmem initial") < 0 ||
        expect(defaults.wmem.max_bytes == (uint32_t)MIB(4), "default wmem max") < 0 ||
        expect(defaults.mem.low_bytes == MIB(24), "default tcp_mem low") < 0 ||
        expect(defaults.mem.pressure_bytes == MIB(32), "default tcp_mem pressure") < 0 ||
        expect(defaults.mem.high_bytes == MIB(48), "default tcp_mem high") < 0) {
        return 1;
    }

    if (tcp_shift_tcp_memory_parse_wmem("8K,64K,2M", (uint32_t)MIB(4),
                                        &parsed_wmem) < 0 ||
        expect(parsed_wmem.min_bytes == KIB(8), "parsed wmem min") < 0 ||
        expect(parsed_wmem.initial_bytes == KIB(64), "parsed wmem initial") < 0 ||
        expect(parsed_wmem.max_bytes == (uint32_t)MIB(2), "parsed wmem max") < 0 ||
        tcp_shift_tcp_memory_parse_wmem("64K,8K,2M", (uint32_t)MIB(4),
                                        &parsed_wmem) == 0 ||
        tcp_shift_tcp_memory_parse_wmem("8K,64K,8M", (uint32_t)MIB(4),
                                        &parsed_wmem) == 0) {
        fprintf(stderr, "tcp memory contract failed: wmem parser\n");
        return 1;
    }

    if (tcp_shift_tcp_memory_parse_mem("1M,2M,3M", &parsed_mem) < 0 ||
        expect(parsed_mem.low_bytes == MIB(1), "parsed tcp_mem low") < 0 ||
        expect(parsed_mem.pressure_bytes == MIB(2), "parsed tcp_mem pressure") < 0 ||
        expect(parsed_mem.high_bytes == MIB(3), "parsed tcp_mem high") < 0 ||
        tcp_shift_tcp_memory_parse_mem("3M,2M,1M", &parsed_mem) == 0) {
        fprintf(stderr, "tcp memory contract failed: tcp_mem parser\n");
        return 1;
    }

    memset(&small, 0, sizeof(small));
    small.wmem.min_bytes = KIB(4);
    small.wmem.initial_bytes = KIB(8);
    small.wmem.max_bytes = KIB(256);
    small.mem.low_bytes = KIB(4);
    small.mem.pressure_bytes = KIB(8);
    small.mem.high_bytes = KIB(12);
    small.effective_memory_bytes = MIB(64);
    small.compile_ceiling_bytes = KIB(256);

    memset(&pcb, 0, sizeof(pcb));
    pcb.cwnd = KIB(64);
    if (tcp_shift_tcp_memory_manager_init(&manager, &small) < 0 ||
        tcp_shift_tcp_memory_flow_init(&manager, &flow, &pcb) < 0 ||
        expect(flow.capacity_bytes == KIB(8), "initial flow capacity") < 0 ||
        expect(pcb.snd_buf == KIB(8), "initial pcb snd_buf") < 0 ||
        tcp_shift_tcp_memory_flow_maybe_grow(
            &flow, &pcb, TCP_SHIFT_TCP_SNDBUF_EXPAND_NUM,
            TCP_SHIFT_TCP_SNDBUF_EXPAND_DEN) != 1 ||
        expect(flow.capacity_bytes == KIB(128), "2xcwnd autotune growth") < 0) {
        return 1;
    }

    if (tcp_shift_tcp_memory_flow_can_write(&flow, &pcb, KIB(8)) == 0) {
        fprintf(stderr, "tcp memory contract failed: initial write budget\n");
        return 1;
    }
    tcp_shift_tcp_memory_flow_note_write(&flow, KIB(8));
    if (expect(manager.pressure_active != 0U, "enter pressure") < 0) {
        return 1;
    }

    pcb.cwnd = KIB(200);
    if (tcp_shift_tcp_memory_flow_maybe_grow(
            &flow, &pcb, TCP_SHIFT_TCP_SNDBUF_EXPAND_NUM,
            TCP_SHIFT_TCP_SNDBUF_EXPAND_DEN) != 0 ||
        expect(flow.capacity_bytes == KIB(128), "freeze growth under pressure") < 0 ||
        expect(manager.stats.growth_suppressed == 1U, "pressure suppression counter") < 0 ||
        tcp_shift_tcp_memory_flow_can_write(&flow, &pcb, KIB(5)) != 0 ||
        expect(manager.stats.high_blocks == 1U, "high-water block counter") < 0) {
        return 1;
    }

    tcp_shift_tcp_memory_flow_note_acked(&flow, KIB(4));
    if (expect(manager.pressure_active == 0U, "exit pressure at low watermark") < 0 ||
        tcp_shift_tcp_memory_flow_maybe_grow(
            &flow, &pcb, TCP_SHIFT_TCP_SNDBUF_EXPAND_NUM,
            TCP_SHIFT_TCP_SNDBUF_EXPAND_DEN) != 1 ||
        expect(flow.capacity_bytes == KIB(256), "growth resumes after pressure") < 0) {
        return 1;
    }

    tcp_shift_tcp_memory_flow_release(&flow);
    if (expect(manager.stats.charged_bytes == 0U, "release residual charge") < 0 ||
        expect(manager.stats.pressure_enters == 1U, "pressure enter count") < 0 ||
        expect(manager.stats.pressure_exits == 1U, "pressure exit count") < 0 ||
        expect(manager.stats.accounting_underflows == 0U, "accounting integrity") < 0) {
        return 1;
    }

    printf("tcp_memory_contract=ok wmem=4096,32768,4194304 "
           "tcp_mem=25165824,33554432,50331648 "
           "pressure=ok high=ok autotune=2xcwnd accounting=queued_payload\n");
    return 0;
}
