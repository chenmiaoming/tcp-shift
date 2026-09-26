#ifndef TCP_SHIFT_LWIP_TCP_MEMORY_H
#define TCP_SHIFT_LWIP_TCP_MEMORY_H

#include <stddef.h>
#include <stdint.h>

#include "lwip/err.h"
#include "lwip/tcp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TCP_SHIFT_LWIP_TCP_MEMORY_EXT_ARG_ID 1U
#define TCP_SHIFT_TCP_WMEM_DEFAULT_MIN_BYTES 4096U
#define TCP_SHIFT_TCP_WMEM_DEFAULT_INITIAL_BYTES (32U * 1024U)
#define TCP_SHIFT_TCP_WMEM_DEFAULT_MAX_FLOOR_BYTES (256U * 1024U)
#define TCP_SHIFT_TCP_WMEM_AUTOTUNE_DIVISOR 128U
#define TCP_SHIFT_TCP_MEM_PRESSURE_DIVISOR 16U
#define TCP_SHIFT_TCP_MEM_MIN_PAGES 128U
#define TCP_SHIFT_TCP_SNDBUF_EXPAND_NUM 2U
#define TCP_SHIFT_TCP_SNDBUF_EXPAND_DEN 1U

struct tcp_shift_tcp_wmem_policy {
    uint32_t min_bytes;
    uint32_t initial_bytes;
    uint32_t max_bytes;
};

struct tcp_shift_tcp_mem_policy {
    uint64_t low_bytes;
    uint64_t pressure_bytes;
    uint64_t high_bytes;
};

struct tcp_shift_tcp_memory_config {
    struct tcp_shift_tcp_wmem_policy wmem;
    struct tcp_shift_tcp_mem_policy mem;
    uint64_t effective_memory_bytes;
    uint32_t compile_ceiling_bytes;
};

struct tcp_shift_tcp_memory_stats {
    uint64_t flow_inits;
    uint64_t flow_releases;
    uint64_t write_events;
    uint64_t write_bytes;
    uint64_t ack_events;
    uint64_t ack_bytes;
    uint64_t charged_bytes;
    uint64_t peak_charged_bytes;
    uint64_t pressure_enters;
    uint64_t pressure_exits;
    uint64_t high_blocks;
    uint64_t growth_events;
    uint64_t growth_bytes;
    uint64_t growth_suppressed;
    uint64_t accounting_underflows;
};

struct tcp_shift_tcp_memory_manager {
    struct tcp_shift_tcp_memory_config config;
    struct tcp_shift_tcp_memory_stats stats;
    unsigned pressure_active;
};

struct tcp_shift_tcp_memory_flow {
    struct tcp_shift_tcp_memory_manager *manager;
    uint32_t capacity_bytes;
    uint32_t sndbuf_expand_num;
    uint32_t sndbuf_expand_den;
    uint64_t queued_bytes;
};

uint64_t tcp_shift_tcp_memory_effective_bytes(void);

int tcp_shift_tcp_memory_config_default(
    struct tcp_shift_tcp_memory_config *config,
    uint64_t effective_memory_bytes,
    uint64_t page_size_bytes,
    uint32_t compile_ceiling_bytes);

int tcp_shift_tcp_memory_parse_wmem(
    const char *text,
    uint32_t compile_ceiling_bytes,
    struct tcp_shift_tcp_wmem_policy *policy);

int tcp_shift_tcp_memory_parse_mem(
    const char *text,
    struct tcp_shift_tcp_mem_policy *policy);

int tcp_shift_tcp_memory_manager_init(
    struct tcp_shift_tcp_memory_manager *manager,
    const struct tcp_shift_tcp_memory_config *config);

int tcp_shift_tcp_memory_flow_init(
    struct tcp_shift_tcp_memory_manager *manager,
    struct tcp_shift_tcp_memory_flow *flow,
    struct tcp_pcb *pcb);

/* Set a controller-provided sender-buffer expansion requirement. Allocation,
 * wmem.max and global tcp_mem pressure/high-water enforcement remain owned by
 * the transport memory manager. */
int tcp_shift_tcp_memory_flow_set_sndbuf_expand(
    struct tcp_shift_tcp_memory_flow *flow,
    uint32_t expand_num,
    uint32_t expand_den);

int tcp_shift_tcp_memory_flow_maybe_grow(
    struct tcp_shift_tcp_memory_flow *flow,
    struct tcp_pcb *pcb,
    uint32_t expand_num,
    uint32_t expand_den);

uint32_t tcp_shift_tcp_memory_flow_available_bytes(
    struct tcp_shift_tcp_memory_flow *flow,
    struct tcp_pcb *pcb);

int tcp_shift_tcp_memory_flow_can_write(
    struct tcp_shift_tcp_memory_flow *flow,
    const struct tcp_pcb *pcb,
    size_t bytes);

void tcp_shift_tcp_memory_flow_note_write(
    struct tcp_shift_tcp_memory_flow *flow,
    size_t bytes);

void tcp_shift_tcp_memory_flow_note_acked(
    struct tcp_shift_tcp_memory_flow *flow,
    size_t bytes);

void tcp_shift_tcp_memory_flow_release(
    struct tcp_shift_tcp_memory_flow *flow);

/* Production raw-lwIP wrappers. The bridge target rewrites tcp_write/tcp_sent
 * to these symbols at compile time; this translation unit itself calls the
 * unwrapped lwIP APIs. */
u16_t tcp_shift_lwip_tcp_memory_sndbuf(struct tcp_pcb *pcb);
err_t tcp_shift_lwip_tcp_memory_write(struct tcp_pcb *pcb,
                                      const void *arg,
                                      u16_t len,
                                      u8_t apiflags);
void tcp_shift_lwip_tcp_memory_sent(struct tcp_pcb *pcb, tcp_sent_fn sent);

/* Apply a per-PCB controller hint. Calling this from passive open is supported
 * even while handshake bookkeeping remains queued: the memory extension stores
 * the ratio without allocating flow capacity, and the first data tcp_write
 * initializes the flow from the configured runtime initial sndbuf. */
int tcp_shift_lwip_tcp_memory_set_sndbuf_expand(
    struct tcp_pcb *pcb,
    uint32_t expand_num,
    uint32_t expand_den);

const struct tcp_shift_tcp_memory_config *
tcp_shift_lwip_tcp_memory_process_config(void);
const struct tcp_shift_tcp_memory_stats *
tcp_shift_lwip_tcp_memory_process_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* TCP_SHIFT_LWIP_TCP_MEMORY_H */
