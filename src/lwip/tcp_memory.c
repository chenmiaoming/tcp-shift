#include "lwip/tcp_memory.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct tcp_shift_lwip_tcp_memory_ext {
    struct tcp_shift_tcp_memory_flow flow;
    tcp_sent_fn app_sent;
    uint32_t pending_expand_num;
    uint32_t pending_expand_den;
};

static struct tcp_shift_tcp_memory_manager tcp_shift_process_tcp_memory;
static int tcp_shift_process_tcp_memory_state;

static uint64_t tcp_shift_u64_min(uint64_t left, uint64_t right)
{
    return left < right ? left : right;
}

static uint64_t tcp_shift_u64_mul_sat(uint64_t value, uint64_t multiplier)
{
    if (value != 0U && multiplier > UINT64_MAX / value) {
        return UINT64_MAX;
    }
    return value * multiplier;
}

static uint64_t tcp_shift_read_limit_file(const char *path)
{
    char buffer[96];
    char *end = NULL;
    unsigned long long parsed;
    FILE *file;

    file = fopen(path, "r");
    if (file == NULL) {
        return 0U;
    }
    if (fgets(buffer, sizeof(buffer), file) == NULL) {
        fclose(file);
        return 0U;
    }
    fclose(file);

    if (strncmp(buffer, "max", 3U) == 0) {
        return UINT64_MAX;
    }

    errno = 0;
    parsed = strtoull(buffer, &end, 10);
    if (errno != 0 || end == buffer || parsed == 0ULL) {
        return 0U;
    }
    return (uint64_t)parsed;
}

uint64_t tcp_shift_tcp_memory_effective_bytes(void)
{
    long pages;
    long page_size;
    uint64_t physical = 0U;
    uint64_t limit;
    uint64_t effective;

    pages = sysconf(_SC_PHYS_PAGES);
    page_size = sysconf(_SC_PAGESIZE);
    if (pages > 0 && page_size > 0 &&
        (uint64_t)pages <= UINT64_MAX / (uint64_t)page_size) {
        physical = (uint64_t)pages * (uint64_t)page_size;
    }

    effective = physical;
    limit = tcp_shift_read_limit_file("/sys/fs/cgroup/memory.max");
    if (limit == 0U) {
        limit = tcp_shift_read_limit_file(
            "/sys/fs/cgroup/memory/memory.limit_in_bytes");
    }
    if (limit != 0U && limit != UINT64_MAX) {
        effective = effective == 0U ? limit : tcp_shift_u64_min(effective, limit);
    }

    if (effective == 0U) {
        /* Conservative fallback when neither the cgroup nor sysconf exposes a
         * usable limit. The policy remains runtime-overridable. */
        effective = UINT64_C(256) * 1024U * 1024U;
    }
    return effective;
}

static int tcp_shift_parse_size_span(const char *begin,
                                     size_t length,
                                     uint64_t *value)
{
    char token[64];
    char *end = NULL;
    unsigned long long parsed;
    uint64_t multiplier = 1U;

    if (begin == NULL || value == NULL || length == 0U) {
        return -1;
    }
    while (length != 0U && isspace((unsigned char)*begin) != 0) {
        begin++;
        length--;
    }
    while (length != 0U &&
           isspace((unsigned char)begin[length - 1U]) != 0) {
        length--;
    }
    if (length == 0U || length >= sizeof(token)) {
        return -1;
    }
    memcpy(token, begin, length);
    token[length] = '\0';

    errno = 0;
    parsed = strtoull(token, &end, 10);
    if (errno != 0 || end == token) {
        return -1;
    }
    while (isspace((unsigned char)*end) != 0) {
        end++;
    }
    if (*end != '\0') {
        switch (*end) {
        case 'k':
        case 'K':
            multiplier = UINT64_C(1024);
            break;
        case 'm':
        case 'M':
            multiplier = UINT64_C(1024) * 1024U;
            break;
        case 'g':
        case 'G':
            multiplier = UINT64_C(1024) * 1024U * 1024U;
            break;
        default:
            return -1;
        }
        end++;
        if (*end == 'i' || *end == 'I') {
            end++;
        }
        if (*end == 'b' || *end == 'B') {
            end++;
        }
        while (isspace((unsigned char)*end) != 0) {
            end++;
        }
        if (*end != '\0') {
            return -1;
        }
    }
    if ((uint64_t)parsed > UINT64_MAX / multiplier) {
        return -1;
    }
    *value = (uint64_t)parsed * multiplier;
    return 0;
}

static int tcp_shift_parse_size(const char *text, uint64_t *value)
{
    return text == NULL ? -1 :
           tcp_shift_parse_size_span(text, strlen(text), value);
}

static int tcp_shift_parse_triplet(const char *text, uint64_t values[3])
{
    const char *cursor;
    const char *separator;
    size_t index;

    if (text == NULL || values == NULL || *text == '\0') {
        return -1;
    }
    cursor = text;
    for (index = 0U; index < 3U; index++) {
        separator = strchr(cursor, ',');
        if (index < 2U) {
            if (separator == NULL || separator == cursor ||
                tcp_shift_parse_size_span(cursor,
                                          (size_t)(separator - cursor),
                                          &values[index]) < 0) {
                return -1;
            }
            cursor = separator + 1;
        } else {
            if (separator != NULL || *cursor == '\0' ||
                tcp_shift_parse_size(cursor, &values[index]) < 0) {
                return -1;
            }
        }
    }
    return 0;
}

int tcp_shift_tcp_memory_config_default(
    struct tcp_shift_tcp_memory_config *config,
    uint64_t effective_memory_bytes,
    uint64_t page_size_bytes,
    uint32_t compile_ceiling_bytes)
{
    uint64_t max_candidate;
    uint64_t pressure;
    uint64_t floor;
    uint32_t initial;
    uint32_t minimum;

    if (config == NULL || effective_memory_bytes == 0U ||
        compile_ceiling_bytes == 0U) {
        return -1;
    }
    if (page_size_bytes == 0U) {
        page_size_bytes = 4096U;
    }

    memset(config, 0, sizeof(*config));
    config->effective_memory_bytes = effective_memory_bytes;
    config->compile_ceiling_bytes = compile_ceiling_bytes;

    minimum = TCP_SHIFT_TCP_WMEM_DEFAULT_MIN_BYTES;
    if (minimum > compile_ceiling_bytes) {
        minimum = compile_ceiling_bytes;
    }
    initial = TCP_SHIFT_TCP_WMEM_DEFAULT_INITIAL_BYTES;
    if (initial > compile_ceiling_bytes) {
        initial = compile_ceiling_bytes;
    }
    if (minimum > initial) {
        minimum = initial;
    }

    max_candidate = effective_memory_bytes /
                    TCP_SHIFT_TCP_WMEM_AUTOTUNE_DIVISOR;
    if (max_candidate < TCP_SHIFT_TCP_WMEM_DEFAULT_MAX_FLOOR_BYTES) {
        max_candidate = TCP_SHIFT_TCP_WMEM_DEFAULT_MAX_FLOOR_BYTES;
    }
    if (max_candidate > compile_ceiling_bytes) {
        max_candidate = compile_ceiling_bytes;
    }
    if (max_candidate < initial) {
        max_candidate = initial;
    }

    config->wmem.min_bytes = minimum;
    config->wmem.initial_bytes = initial;
    config->wmem.max_bytes = (uint32_t)max_candidate;

    pressure = effective_memory_bytes / TCP_SHIFT_TCP_MEM_PRESSURE_DIVISOR;
    floor = tcp_shift_u64_mul_sat(TCP_SHIFT_TCP_MEM_MIN_PAGES,
                                  page_size_bytes);
    if (pressure < floor) {
        pressure = floor;
    }
    config->mem.low_bytes = pressure - (pressure / 4U);
    config->mem.pressure_bytes = pressure;
    config->mem.high_bytes = pressure + (pressure / 2U);
    return 0;
}

int tcp_shift_tcp_memory_parse_wmem(
    const char *text,
    uint32_t compile_ceiling_bytes,
    struct tcp_shift_tcp_wmem_policy *policy)
{
    uint64_t values[3];

    if (policy == NULL || compile_ceiling_bytes == 0U ||
        tcp_shift_parse_triplet(text, values) < 0 ||
        values[0] == 0U || values[0] > values[1] ||
        values[1] > values[2] || values[2] > compile_ceiling_bytes ||
        values[2] > UINT32_MAX) {
        return -1;
    }
    policy->min_bytes = (uint32_t)values[0];
    policy->initial_bytes = (uint32_t)values[1];
    policy->max_bytes = (uint32_t)values[2];
    return 0;
}

int tcp_shift_tcp_memory_parse_mem(
    const char *text,
    struct tcp_shift_tcp_mem_policy *policy)
{
    uint64_t values[3];

    if (policy == NULL || tcp_shift_parse_triplet(text, values) < 0 ||
        values[0] > values[1] || values[1] > values[2] ||
        values[2] == 0U) {
        return -1;
    }
    policy->low_bytes = values[0];
    policy->pressure_bytes = values[1];
    policy->high_bytes = values[2];
    return 0;
}

int tcp_shift_tcp_memory_manager_init(
    struct tcp_shift_tcp_memory_manager *manager,
    const struct tcp_shift_tcp_memory_config *config)
{
    if (manager == NULL || config == NULL ||
        config->wmem.min_bytes == 0U ||
        config->wmem.min_bytes > config->wmem.initial_bytes ||
        config->wmem.initial_bytes > config->wmem.max_bytes ||
        config->wmem.max_bytes > config->compile_ceiling_bytes ||
        config->mem.low_bytes > config->mem.pressure_bytes ||
        config->mem.pressure_bytes > config->mem.high_bytes ||
        config->mem.high_bytes == 0U) {
        return -1;
    }
    memset(manager, 0, sizeof(*manager));
    manager->config = *config;
    return 0;
}

static void tcp_shift_tcp_memory_update_pressure(
    struct tcp_shift_tcp_memory_manager *manager)
{
    if (manager->pressure_active == 0U &&
        manager->stats.charged_bytes >= manager->config.mem.pressure_bytes) {
        manager->pressure_active = 1U;
        manager->stats.pressure_enters++;
    } else if (manager->pressure_active != 0U &&
               manager->stats.charged_bytes <= manager->config.mem.low_bytes) {
        manager->pressure_active = 0U;
        manager->stats.pressure_exits++;
    }
}

int tcp_shift_tcp_memory_flow_init(
    struct tcp_shift_tcp_memory_manager *manager,
    struct tcp_shift_tcp_memory_flow *flow,
    struct tcp_pcb *pcb)
{
    if (manager == NULL || flow == NULL || pcb == NULL ||
        pcb->snd_queuelen != 0U || pcb->unsent != NULL || pcb->unacked != NULL) {
        return -1;
    }
    memset(flow, 0, sizeof(*flow));
    flow->manager = manager;
    flow->capacity_bytes = manager->config.wmem.initial_bytes;
    flow->sndbuf_expand_num = TCP_SHIFT_TCP_SNDBUF_EXPAND_NUM;
    flow->sndbuf_expand_den = TCP_SHIFT_TCP_SNDBUF_EXPAND_DEN;
    pcb->snd_buf = (tcpwnd_size_t)flow->capacity_bytes;
    manager->stats.flow_inits++;
    return 0;
}

int tcp_shift_tcp_memory_flow_set_sndbuf_expand(
    struct tcp_shift_tcp_memory_flow *flow,
    uint32_t expand_num,
    uint32_t expand_den)
{
    if (flow == NULL || flow->manager == NULL ||
        expand_num == 0U || expand_den == 0U) {
        return -1;
    }
    flow->sndbuf_expand_num = expand_num;
    flow->sndbuf_expand_den = expand_den;
    return 0;
}

int tcp_shift_tcp_memory_flow_maybe_grow(
    struct tcp_shift_tcp_memory_flow *flow,
    struct tcp_pcb *pcb,
    uint32_t expand_num,
    uint32_t expand_den)
{
    struct tcp_shift_tcp_memory_manager *manager;
    uint64_t target;
    uint64_t available;
    uint32_t delta;

    if (flow == NULL || pcb == NULL || flow->manager == NULL ||
        expand_num == 0U || expand_den == 0U) {
        return -1;
    }
    manager = flow->manager;
    target = tcp_shift_u64_mul_sat((uint64_t)pcb->cwnd, expand_num) /
             expand_den;
    if (target < manager->config.wmem.initial_bytes) {
        target = manager->config.wmem.initial_bytes;
    }
    if (target > manager->config.wmem.max_bytes) {
        target = manager->config.wmem.max_bytes;
    }
    if (target <= flow->capacity_bytes) {
        return 0;
    }
    if (manager->pressure_active != 0U) {
        manager->stats.growth_suppressed++;
        return 0;
    }

    delta = (uint32_t)(target - flow->capacity_bytes);
    available = (uint64_t)pcb->snd_buf + delta;
    if (available > UINT32_MAX) {
        return -1;
    }
    pcb->snd_buf = (tcpwnd_size_t)available;
    flow->capacity_bytes = (uint32_t)target;
    manager->stats.growth_events++;
    manager->stats.growth_bytes += delta;
    return 1;
}

int tcp_shift_tcp_memory_flow_can_write(
    struct tcp_shift_tcp_memory_flow *flow,
    const struct tcp_pcb *pcb,
    size_t bytes)
{
    struct tcp_shift_tcp_memory_manager *manager;
    uint64_t requested;

    if (flow == NULL || pcb == NULL || flow->manager == NULL) {
        return 0;
    }
    if (bytes == 0U) {
        return 1;
    }
    if (bytes > pcb->snd_buf) {
        manager = flow->manager;
        manager->stats.sndbuf_blocks++;
        manager->stats.last_block_snd_buf_bytes = (uint32_t)pcb->snd_buf;
        manager->stats.last_block_requested_bytes =
            bytes > UINT32_MAX ? UINT32_MAX : (uint32_t)bytes;
        manager->stats.last_block_capacity_bytes = flow->capacity_bytes;
        manager->stats.last_block_queued_bytes = flow->queued_bytes;
        manager->stats.last_block_snd_queuelen = pcb->snd_queuelen;
        return 0;
    }
    manager = flow->manager;
    requested = (uint64_t)bytes;
    if (manager->stats.charged_bytes > manager->config.mem.high_bytes ||
        requested > manager->config.mem.high_bytes -
                        manager->stats.charged_bytes) {
        manager->stats.high_blocks++;
        return 0;
    }
    return 1;
}

void tcp_shift_tcp_memory_flow_note_write(
    struct tcp_shift_tcp_memory_flow *flow,
    size_t bytes)
{
    struct tcp_shift_tcp_memory_manager *manager;
    uint64_t amount;

    if (flow == NULL || flow->manager == NULL || bytes == 0U) {
        return;
    }
    manager = flow->manager;
    amount = (uint64_t)bytes;
    flow->queued_bytes += amount;
    manager->stats.charged_bytes += amount;
    manager->stats.write_events++;
    manager->stats.write_bytes += amount;
    if (manager->stats.charged_bytes > manager->stats.peak_charged_bytes) {
        manager->stats.peak_charged_bytes = manager->stats.charged_bytes;
    }
    tcp_shift_tcp_memory_update_pressure(manager);
}

void tcp_shift_tcp_memory_flow_note_acked(
    struct tcp_shift_tcp_memory_flow *flow,
    size_t bytes)
{
    struct tcp_shift_tcp_memory_manager *manager;
    uint64_t amount;

    if (flow == NULL || flow->manager == NULL || bytes == 0U) {
        return;
    }
    manager = flow->manager;
    amount = (uint64_t)bytes;
    if (amount > flow->queued_bytes) {
        amount = flow->queued_bytes;
        manager->stats.accounting_underflows++;
    }
    if (amount > manager->stats.charged_bytes) {
        amount = manager->stats.charged_bytes;
        manager->stats.accounting_underflows++;
    }
    flow->queued_bytes -= amount;
    manager->stats.charged_bytes -= amount;
    manager->stats.ack_events++;
    manager->stats.ack_bytes += amount;
    tcp_shift_tcp_memory_update_pressure(manager);
}

void tcp_shift_tcp_memory_flow_release(
    struct tcp_shift_tcp_memory_flow *flow)
{
    struct tcp_shift_tcp_memory_manager *manager;
    uint64_t amount;

    if (flow == NULL || flow->manager == NULL) {
        return;
    }
    manager = flow->manager;
    amount = flow->queued_bytes;
    if (amount > manager->stats.charged_bytes) {
        amount = manager->stats.charged_bytes;
        manager->stats.accounting_underflows++;
    }
    manager->stats.charged_bytes -= amount;
    flow->queued_bytes = 0U;
    flow->capacity_bytes = 0U;
    flow->sndbuf_expand_num = 0U;
    flow->sndbuf_expand_den = 0U;
    flow->manager = NULL;
    manager->stats.flow_releases++;
    tcp_shift_tcp_memory_update_pressure(manager);
}

static int tcp_shift_process_tcp_memory_init(void)
{
    struct tcp_shift_tcp_memory_config config;
    struct tcp_shift_tcp_wmem_policy wmem;
    struct tcp_shift_tcp_mem_policy mem;
    const char *text;
    uint64_t effective;
    uint64_t override_effective;
    long page_size;

    if (tcp_shift_process_tcp_memory_state != 0) {
        return tcp_shift_process_tcp_memory_state > 0 ? 0 : -1;
    }

    effective = tcp_shift_tcp_memory_effective_bytes();
    text = getenv("TCP_SHIFT_TCP_MEMORY_EFFECTIVE_BYTES");
    if (text != NULL && *text != '\0') {
        if (tcp_shift_parse_size(text, &override_effective) < 0 ||
            override_effective == 0U) {
            goto invalid;
        }
        effective = override_effective;
    }
    page_size = sysconf(_SC_PAGESIZE);
    if (tcp_shift_tcp_memory_config_default(
            &config, effective,
            page_size > 0 ? (uint64_t)page_size : 4096U,
            (uint32_t)TCP_SND_BUF) < 0) {
        goto invalid;
    }

    text = getenv("TCP_SHIFT_TCP_WMEM");
    if (text != NULL && *text != '\0') {
        if (tcp_shift_tcp_memory_parse_wmem(
                text, config.compile_ceiling_bytes, &wmem) < 0) {
            goto invalid;
        }
        config.wmem = wmem;
    }

    text = getenv("TCP_SHIFT_TCP_MEM");
    if (text != NULL && *text != '\0' && strcmp(text, "auto") != 0) {
        if (tcp_shift_tcp_memory_parse_mem(text, &mem) < 0) {
            goto invalid;
        }
        config.mem = mem;
    }

    if (tcp_shift_tcp_memory_manager_init(&tcp_shift_process_tcp_memory,
                                          &config) < 0) {
        goto invalid;
    }
    tcp_shift_process_tcp_memory_state = 1;
    return 0;

invalid:
    tcp_shift_process_tcp_memory_state = -1;
    errno = EINVAL;
    fprintf(stderr,
            "tcp-shift-tcp-memory: invalid TCP_SHIFT_TCP_WMEM/TCP_SHIFT_TCP_MEM configuration\n");
    return -1;
}

static void tcp_shift_lwip_tcp_memory_destroyed(u8_t id, void *data)
{
    struct tcp_shift_lwip_tcp_memory_ext *ext = data;

    (void)id;
    if (ext == NULL) {
        return;
    }
    tcp_shift_tcp_memory_flow_release(&ext->flow);
    free(ext);
}

static const struct tcp_ext_arg_callbacks tcp_shift_lwip_tcp_memory_callbacks = {
    .destroy = tcp_shift_lwip_tcp_memory_destroyed,
    .passive_open = NULL,
};

static struct tcp_shift_lwip_tcp_memory_ext *
tcp_shift_lwip_tcp_memory_get(struct tcp_pcb *pcb)
{
    if (pcb == NULL) {
        return NULL;
    }
    return tcp_ext_arg_get(pcb,
                           (u8_t)TCP_SHIFT_LWIP_TCP_MEMORY_EXT_ARG_ID);
}

static struct tcp_shift_lwip_tcp_memory_ext *
tcp_shift_lwip_tcp_memory_alloc_ext(struct tcp_pcb *pcb)
{
    struct tcp_shift_lwip_tcp_memory_ext *ext;

    ext = calloc(1U, sizeof(*ext));
    if (ext == NULL) {
        return NULL;
    }
    tcp_ext_arg_set_callbacks(
        pcb, (u8_t)TCP_SHIFT_LWIP_TCP_MEMORY_EXT_ARG_ID,
        &tcp_shift_lwip_tcp_memory_callbacks);
    tcp_ext_arg_set(pcb, (u8_t)TCP_SHIFT_LWIP_TCP_MEMORY_EXT_ARG_ID, ext);
    return ext;
}

static struct tcp_shift_lwip_tcp_memory_ext *
tcp_shift_lwip_tcp_memory_ensure(struct tcp_pcb *pcb)
{
    struct tcp_shift_lwip_tcp_memory_ext *ext;
    uint32_t expand_num;
    uint32_t expand_den;

    ext = tcp_shift_lwip_tcp_memory_get(pcb);
    if (ext != NULL && ext->flow.manager != NULL) {
        return ext;
    }
    if (tcp_shift_process_tcp_memory_init() < 0) {
        return NULL;
    }

    if (ext == NULL) {
        ext = tcp_shift_lwip_tcp_memory_alloc_ext(pcb);
        if (ext == NULL) {
            return NULL;
        }
    }

    /* A controller may publish its sender-buffer hint from the passive-open
     * callback while lwIP still accounts the SYN-ACK in its send queue. Keep
     * that hint in the memory-owned extension, but preserve flow_init()'s
     * empty-data-queue invariant until the first bridge data write. */
    expand_num = ext->pending_expand_num;
    expand_den = ext->pending_expand_den;
    if (tcp_shift_tcp_memory_flow_init(&tcp_shift_process_tcp_memory,
                                       &ext->flow, pcb) < 0) {
        return NULL;
    }
    if (expand_num != 0U &&
        tcp_shift_tcp_memory_flow_set_sndbuf_expand(
            &ext->flow, expand_num, expand_den) < 0) {
        tcp_shift_tcp_memory_flow_release(&ext->flow);
        return NULL;
    }
    ext->pending_expand_num = 0U;
    ext->pending_expand_den = 0U;
    return ext;
}

static err_t tcp_shift_lwip_tcp_memory_sent_dispatch(void *arg,
                                                      struct tcp_pcb *pcb,
                                                      u16_t len)
{
    struct tcp_shift_lwip_tcp_memory_ext *ext;
    tcp_sent_fn app_sent;

    ext = tcp_shift_lwip_tcp_memory_get(pcb);
    if (ext == NULL) {
        return ERR_OK;
    }
    tcp_shift_tcp_memory_flow_note_acked(&ext->flow, len);
    app_sent = ext->app_sent;
    return app_sent == NULL ? ERR_OK : app_sent(arg, pcb, len);
}

err_t tcp_shift_lwip_tcp_memory_write(struct tcp_pcb *pcb,
                                      const void *arg,
                                      u16_t len,
                                      u8_t apiflags)
{
    struct tcp_shift_lwip_tcp_memory_ext *ext;
    err_t err;

    if (pcb == NULL) {
        return ERR_ARG;
    }
    ext = tcp_shift_lwip_tcp_memory_ensure(pcb);
    if (ext == NULL) {
        return ERR_MEM;
    }
    if (tcp_shift_tcp_memory_flow_maybe_grow(
            &ext->flow, pcb,
            ext->flow.sndbuf_expand_num,
            ext->flow.sndbuf_expand_den) < 0) {
        return ERR_MEM;
    }
    if (tcp_shift_tcp_memory_flow_can_write(&ext->flow, pcb, len) == 0) {
        tcp_set_flags(pcb, TF_NAGLEMEMERR);
        return ERR_MEM;
    }

    err = tcp_write(pcb, arg, len, apiflags);
    if (err == ERR_OK) {
        tcp_shift_tcp_memory_flow_note_write(&ext->flow, len);
    } else if (err == ERR_MEM && ext->flow.manager != NULL) {
        struct tcp_shift_tcp_memory_manager *manager = ext->flow.manager;

        manager->stats.upstream_write_mem_errors++;
        manager->stats.last_block_snd_buf_bytes = (uint32_t)pcb->snd_buf;
        manager->stats.last_block_requested_bytes = (uint32_t)len;
        manager->stats.last_block_capacity_bytes = ext->flow.capacity_bytes;
        manager->stats.last_block_queued_bytes = ext->flow.queued_bytes;
        manager->stats.last_block_snd_queuelen = pcb->snd_queuelen;
    }
    return err;
}

void tcp_shift_lwip_tcp_memory_sent(struct tcp_pcb *pcb, tcp_sent_fn sent)
{
    struct tcp_shift_lwip_tcp_memory_ext *ext;

    if (pcb == NULL) {
        tcp_sent(pcb, sent);
        return;
    }
    ext = tcp_shift_lwip_tcp_memory_get(pcb);
    if (sent == NULL) {
        if (ext != NULL) {
            ext->app_sent = NULL;
        }
        tcp_sent(pcb, NULL);
        return;
    }

    if (ext == NULL) {
        ext = tcp_shift_lwip_tcp_memory_ensure(pcb);
    }
    if (ext == NULL) {
        tcp_sent(pcb, sent);
        return;
    }
    ext->app_sent = sent;
    tcp_sent(pcb, tcp_shift_lwip_tcp_memory_sent_dispatch);
}

int tcp_shift_lwip_tcp_memory_set_sndbuf_expand(
    struct tcp_pcb *pcb,
    uint32_t expand_num,
    uint32_t expand_den)
{
    struct tcp_shift_lwip_tcp_memory_ext *ext;

    if (pcb == NULL || expand_num == 0U || expand_den == 0U) {
        return -1;
    }
    ext = tcp_shift_lwip_tcp_memory_get(pcb);
    if (ext != NULL && ext->flow.manager != NULL) {
        return tcp_shift_tcp_memory_flow_set_sndbuf_expand(
            &ext->flow, expand_num, expand_den);
    }
    if (ext == NULL) {
        ext = tcp_shift_lwip_tcp_memory_alloc_ext(pcb);
        if (ext == NULL) {
            return -1;
        }
    }

    /* Do not force flow initialization from a passive-open callback. The
     * handshake may still occupy lwIP's queue bookkeeping at that instant.
     * The first data write consumes this pending ratio after the queue is clean. */
    ext->pending_expand_num = expand_num;
    ext->pending_expand_den = expand_den;
    return 0;
}

const struct tcp_shift_tcp_memory_config *
tcp_shift_lwip_tcp_memory_process_config(void)
{
    return tcp_shift_process_tcp_memory_init() == 0
               ? &tcp_shift_process_tcp_memory.config
               : NULL;
}

const struct tcp_shift_tcp_memory_stats *
tcp_shift_lwip_tcp_memory_process_stats(void)
{
    return tcp_shift_process_tcp_memory_init() == 0
               ? &tcp_shift_process_tcp_memory.stats
               : NULL;
}
