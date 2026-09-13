#include "bridge/bridge.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include "lwip/err.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#define TCP_SHIFT_BRIDGE_BACKEND_READ_CHUNK 4096U
#define TCP_SHIFT_BRIDGE_BACKEND_READ_BUDGET 16U
#define TCP_SHIFT_BRIDGE_BACKEND_WRITE_IOV 64U

struct tcp_shift_bridge_flow {
    struct tcp_shift_bridge *bridge;
    struct tcp_shift_bridge_flow *next;
    struct tcp_pcb *pcb;
    int backend_fd;
    struct tcp_shift_lwip_loop_watch backend_watch;
    struct pbuf *public_rx;
    unsigned backend_connecting;
    unsigned backend_connected;
    unsigned backend_eof;
    unsigned public_eof;
    unsigned backend_write_shutdown;
    unsigned public_write_shutdown;
    unsigned backend_read_blocked;
};

static err_t tcp_shift_bridge_public_recv(void *arg,
                                          struct tcp_pcb *pcb,
                                          struct pbuf *p,
                                          err_t err);
static err_t tcp_shift_bridge_public_sent(void *arg,
                                          struct tcp_pcb *pcb,
                                          u16_t len);
static err_t tcp_shift_bridge_public_poll(void *arg, struct tcp_pcb *pcb);
static void tcp_shift_bridge_public_error(void *arg, err_t err);

static void tcp_shift_bridge_flow_set_callbacks(
    struct tcp_shift_bridge_flow *flow)
{
    tcp_arg(flow->pcb, flow);
    tcp_recv(flow->pcb, tcp_shift_bridge_public_recv);
    tcp_sent(flow->pcb, tcp_shift_bridge_public_sent);
    tcp_err(flow->pcb, tcp_shift_bridge_public_error);
    tcp_poll(flow->pcb, tcp_shift_bridge_public_poll, 2);
}

static void tcp_shift_bridge_flow_clear_callbacks(struct tcp_pcb *pcb)
{
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_poll(pcb, NULL, 0);
}

static void tcp_shift_bridge_flow_unlink(struct tcp_shift_bridge_flow *flow)
{
    struct tcp_shift_bridge_flow **cursor = &flow->bridge->flows;

    while (*cursor != NULL && *cursor != flow) {
        cursor = &(*cursor)->next;
    }
    if (*cursor == flow) {
        *cursor = flow->next;
        if (flow->bridge->active_flows != 0U) {
            flow->bridge->active_flows--;
        }
    }
}

static void tcp_shift_bridge_release_pending_public(
    struct tcp_shift_bridge_flow *flow)
{
    if (flow->public_rx == NULL) {
        return;
    }

    if (flow->bridge->pending_public_bytes >= flow->public_rx->tot_len) {
        flow->bridge->pending_public_bytes -= flow->public_rx->tot_len;
    } else {
        flow->bridge->pending_public_bytes = 0U;
    }
    pbuf_free(flow->public_rx);
    flow->public_rx = NULL;
}

static void tcp_shift_bridge_flow_release(struct tcp_shift_bridge_flow *flow)
{
    if (flow->backend_watch.registered != 0U) {
        (void)tcp_shift_lwip_loop_watch_remove(flow->bridge->loop,
                                               &flow->backend_watch);
    }
    if (flow->backend_fd >= 0) {
        close(flow->backend_fd);
        flow->backend_fd = -1;
    }
    tcp_shift_bridge_release_pending_public(flow);
    tcp_shift_bridge_flow_unlink(flow);
    free(flow);
}

static void tcp_shift_bridge_flow_abort(struct tcp_shift_bridge_flow *flow)
{
    struct tcp_pcb *pcb = flow->pcb;

    flow->pcb = NULL;
    if (pcb != NULL) {
        tcp_shift_bridge_flow_clear_callbacks(pcb);
        tcp_abort(pcb);
    }
    tcp_shift_bridge_flow_release(flow);
}

static void tcp_shift_bridge_mark_backend_read_blocked(
    struct tcp_shift_bridge_flow *flow)
{
    if (flow->backend_read_blocked == 0U) {
        flow->backend_read_blocked = 1U;
        flow->bridge->backend_read_blocked_events++;
    }
}

static void tcp_shift_bridge_track_public_rx(struct tcp_shift_bridge_flow *flow,
                                             const struct pbuf *p)
{
    flow->bridge->pending_public_bytes += p->tot_len;
    if (flow->bridge->pending_public_bytes >
        flow->bridge->peak_pending_public_bytes) {
        flow->bridge->peak_pending_public_bytes =
            flow->bridge->pending_public_bytes;
    }
}

static int tcp_shift_bridge_configure_backend_socket(
    struct tcp_shift_bridge_flow *flow)
{
    int requested = (int)TCP_SHIFT_BRIDGE_BACKEND_SOCKET_BUFFER;
    int actual;
    socklen_t actual_length;

    if (setsockopt(flow->backend_fd, SOL_SOCKET, SO_SNDBUF,
                   &requested, sizeof(requested)) < 0 ||
        setsockopt(flow->backend_fd, SOL_SOCKET, SO_RCVBUF,
                   &requested, sizeof(requested)) < 0) {
        return -1;
    }

    actual = 0;
    actual_length = sizeof(actual);
    if (getsockopt(flow->backend_fd, SOL_SOCKET, SO_SNDBUF,
                   &actual, &actual_length) < 0) {
        return -1;
    }
    if (actual > 0 && (uint32_t)actual > flow->bridge->backend_socket_sndbuf_bytes) {
        flow->bridge->backend_socket_sndbuf_bytes = (uint32_t)actual;
    }

    actual = 0;
    actual_length = sizeof(actual);
    if (getsockopt(flow->backend_fd, SOL_SOCKET, SO_RCVBUF,
                   &actual, &actual_length) < 0) {
        return -1;
    }
    if (actual > 0 && (uint32_t)actual > flow->bridge->backend_socket_rcvbuf_bytes) {
        flow->bridge->backend_socket_rcvbuf_bytes = (uint32_t)actual;
    }
    return 0;
}

static uint32_t tcp_shift_bridge_backend_events(
    const struct tcp_shift_bridge_flow *flow)
{
    uint32_t events;

    if (flow->backend_connecting != 0U) {
        return EPOLLOUT;
    }

    events = EPOLLRDHUP;
    if (flow->backend_eof == 0U && flow->backend_read_blocked == 0U) {
        events |= EPOLLIN;
    }
    if (flow->public_rx != NULL) {
        events |= EPOLLOUT;
    }
    return events;
}

static int tcp_shift_bridge_sync_backend_watch(
    struct tcp_shift_bridge_flow *flow)
{
    uint32_t events;

    if (flow->backend_watch.registered == 0U) {
        return 0;
    }
    events = tcp_shift_bridge_backend_events(flow);
    return tcp_shift_lwip_loop_watch_mod(flow->bridge->loop,
                                         &flow->backend_watch, events);
}

static int tcp_shift_bridge_flush_public_to_backend(
    struct tcp_shift_bridge_flow *flow)
{
    while (flow->backend_connected != 0U && flow->public_rx != NULL) {
        struct iovec iov[TCP_SHIFT_BRIDGE_BACKEND_WRITE_IOV];
        struct pbuf *p;
        size_t iov_count = 0;
        ssize_t written;

        for (p = flow->public_rx;
             p != NULL && iov_count < TCP_SHIFT_BRIDGE_BACKEND_WRITE_IOV;
             p = p->next) {
            if (p->len == 0U) {
                continue;
            }
            iov[iov_count].iov_base = p->payload;
            iov[iov_count].iov_len = p->len;
            iov_count++;
        }
        if (iov_count == 0U) {
            tcp_shift_bridge_release_pending_public(flow);
            break;
        }

        do {
            written = writev(flow->backend_fd, iov, (int)iov_count);
        } while (written < 0 && errno == EINTR);

        if (written < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                flow->bridge->backend_write_blocked_events++;
                return 0;
            }
            return -1;
        }
        if (written == 0) {
            errno = EPIPE;
            return -1;
        }

        flow->bridge->public_to_backend_bytes += (uint64_t)written;
        if (flow->bridge->pending_public_bytes >= (uint64_t)written) {
            flow->bridge->pending_public_bytes -= (uint64_t)written;
        } else {
            flow->bridge->pending_public_bytes = 0U;
        }
        tcp_recved(flow->pcb, (u16_t)written);
        flow->public_rx = pbuf_free_header(flow->public_rx, (u16_t)written);
    }
    return 0;
}

static int tcp_shift_bridge_consume_backend(struct tcp_shift_bridge_flow *flow,
                                            unsigned char *buffer,
                                            size_t length)
{
    size_t consumed = 0;

    while (consumed < length) {
        ssize_t result;

        do {
            result = recv(flow->backend_fd, buffer + consumed,
                          length - consumed, 0);
        } while (result < 0 && errno == EINTR);
        if (result <= 0) {
            if (result == 0) {
                errno = EPIPE;
            }
            return -1;
        }
        consumed += (size_t)result;
    }
    return 0;
}

static int tcp_shift_bridge_pump_backend_to_public(
    struct tcp_shift_bridge_flow *flow)
{
    unsigned budget;

    if (flow->backend_connected == 0U || flow->backend_eof != 0U ||
        flow->pcb == NULL) {
        return 0;
    }

    for (budget = 0; budget < TCP_SHIFT_BRIDGE_BACKEND_READ_BUDGET; budget++) {
        unsigned char buffer[TCP_SHIFT_BRIDGE_BACKEND_READ_CHUNK];
        u16_t sndbuf = tcp_sndbuf(flow->pcb);
        size_t wanted;
        ssize_t available;
        err_t err;

        if (sndbuf == 0U) {
            tcp_shift_bridge_mark_backend_read_blocked(flow);
            return 0;
        }
        wanted = sizeof(buffer);
        if (wanted > sndbuf) {
            wanted = sndbuf;
        }

        do {
            available = recv(flow->backend_fd, buffer, wanted,
                             MSG_PEEK | MSG_DONTWAIT);
        } while (available < 0 && errno == EINTR);

        if (available < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            return -1;
        }
        if (available == 0) {
            flow->backend_eof = 1U;
            return 0;
        }

        err = tcp_write(flow->pcb, buffer, (u16_t)available,
                        TCP_WRITE_FLAG_COPY);
        if (err == ERR_MEM) {
            tcp_shift_bridge_mark_backend_read_blocked(flow);
            return 0;
        }
        if (err != ERR_OK) {
            errno = EIO;
            return -1;
        }

        if (tcp_shift_bridge_consume_backend(flow, buffer,
                                             (size_t)available) < 0) {
            return -1;
        }
        flow->bridge->backend_to_public_bytes += (uint64_t)available;
        (void)tcp_output(flow->pcb);
    }
    return 0;
}

static int tcp_shift_bridge_shutdown_backend_write(
    struct tcp_shift_bridge_flow *flow)
{
    if (flow->public_eof == 0U || flow->public_rx != NULL ||
        flow->backend_connected == 0U || flow->backend_write_shutdown != 0U) {
        return 0;
    }

    if (shutdown(flow->backend_fd, SHUT_WR) < 0 && errno != ENOTCONN) {
        return -1;
    }
    flow->backend_write_shutdown = 1U;
    return 0;
}

static int tcp_shift_bridge_shutdown_public_write(
    struct tcp_shift_bridge_flow *flow)
{
    err_t err;

    if (flow->backend_eof == 0U || flow->public_write_shutdown != 0U ||
        flow->pcb == NULL) {
        return 0;
    }

    err = tcp_shutdown(flow->pcb, 0, 1);
    if (err == ERR_MEM) {
        return 0;
    }
    if (err != ERR_OK) {
        errno = EIO;
        return -1;
    }
    flow->public_write_shutdown = 1U;
    return 0;
}

/* Returns 1 if the flow was released, 0 if it remains alive, -1 on failure. */
static int tcp_shift_bridge_finish_if_done(struct tcp_shift_bridge_flow *flow)
{
    struct tcp_pcb *pcb;
    err_t err;

    if (tcp_shift_bridge_shutdown_backend_write(flow) < 0 ||
        tcp_shift_bridge_shutdown_public_write(flow) < 0) {
        return -1;
    }

    if (flow->public_eof == 0U || flow->backend_eof == 0U ||
        flow->public_rx != NULL || flow->backend_write_shutdown == 0U ||
        flow->public_write_shutdown == 0U || flow->pcb == NULL) {
        return 0;
    }

    pcb = flow->pcb;
    tcp_shift_bridge_flow_clear_callbacks(pcb);
    err = tcp_close(pcb);
    if (err != ERR_OK) {
        tcp_shift_bridge_flow_set_callbacks(flow);
        return 0;
    }

    flow->pcb = NULL;
    tcp_shift_bridge_flow_release(flow);
    return 1;
}

static int tcp_shift_bridge_progress(struct tcp_shift_bridge_flow *flow)
{
    int finished;

    if (tcp_shift_bridge_flush_public_to_backend(flow) < 0) {
        return -1;
    }
    if (tcp_shift_bridge_pump_backend_to_public(flow) < 0) {
        return -1;
    }

    finished = tcp_shift_bridge_finish_if_done(flow);
    if (finished != 0) {
        return finished;
    }
    if (tcp_shift_bridge_sync_backend_watch(flow) < 0) {
        return -1;
    }
    return 0;
}

static int tcp_shift_bridge_complete_backend_connect(
    struct tcp_shift_bridge_flow *flow)
{
    int error = 0;
    socklen_t error_length = sizeof(error);

    if (getsockopt(flow->backend_fd, SOL_SOCKET, SO_ERROR, &error,
                   &error_length) < 0) {
        return -1;
    }
    if (error != 0) {
        errno = error;
        return -1;
    }

    flow->backend_connecting = 0U;
    flow->backend_connected = 1U;
    flow->bridge->backend_connects++;
    return 0;
}

static int tcp_shift_bridge_backend_ready(void *arg, uint32_t events)
{
    struct tcp_shift_bridge_flow *flow = arg;
    int error = 0;
    socklen_t error_length = sizeof(error);
    int result;

    if (flow->backend_connecting != 0U) {
        if (tcp_shift_bridge_complete_backend_connect(flow) < 0) {
            flow->bridge->backend_failures++;
            tcp_shift_bridge_flow_abort(flow);
            return 0;
        }
    } else if ((events & EPOLLERR) != 0U) {
        if (getsockopt(flow->backend_fd, SOL_SOCKET, SO_ERROR, &error,
                       &error_length) < 0 || error != 0) {
            if (error != 0) {
                errno = error;
            }
            flow->bridge->backend_failures++;
            tcp_shift_bridge_flow_abort(flow);
            return 0;
        }
    }

    if ((events & (EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLHUP)) != 0U) {
        result = tcp_shift_bridge_progress(flow);
        if (result < 0) {
            flow->bridge->backend_failures++;
            tcp_shift_bridge_flow_abort(flow);
        }
    }
    return 0;
}

static err_t tcp_shift_bridge_public_recv(void *arg,
                                          struct tcp_pcb *pcb,
                                          struct pbuf *p,
                                          err_t err)
{
    struct tcp_shift_bridge_flow *flow = arg;
    int result;

    (void)pcb;
    if (flow == NULL) {
        if (p != NULL) {
            pbuf_free(p);
        }
        return ERR_VAL;
    }

    if (err != ERR_OK) {
        if (p != NULL) {
            pbuf_free(p);
        }
        flow->bridge->public_errors++;
        tcp_shift_bridge_flow_abort(flow);
        return ERR_ABRT;
    }

    if (p == NULL) {
        flow->public_eof = 1U;
    } else {
        tcp_shift_bridge_track_public_rx(flow, p);
        if (flow->public_rx == NULL) {
            flow->public_rx = p;
        } else {
            pbuf_cat(flow->public_rx, p);
        }
    }

    result = tcp_shift_bridge_progress(flow);
    if (result < 0) {
        flow->bridge->backend_failures++;
        tcp_shift_bridge_flow_abort(flow);
        return ERR_ABRT;
    }
    return ERR_OK;
}

static err_t tcp_shift_bridge_public_sent(void *arg,
                                          struct tcp_pcb *pcb,
                                          u16_t len)
{
    struct tcp_shift_bridge_flow *flow = arg;
    int result;

    (void)pcb;
    (void)len;
    if (flow == NULL) {
        return ERR_OK;
    }

    flow->backend_read_blocked = 0U;
    result = tcp_shift_bridge_progress(flow);
    if (result < 0) {
        flow->bridge->backend_failures++;
        tcp_shift_bridge_flow_abort(flow);
        return ERR_ABRT;
    }
    return ERR_OK;
}

static err_t tcp_shift_bridge_public_poll(void *arg, struct tcp_pcb *pcb)
{
    struct tcp_shift_bridge_flow *flow = arg;
    int result;

    (void)pcb;
    if (flow == NULL) {
        return ERR_OK;
    }

    flow->backend_read_blocked = 0U;
    result = tcp_shift_bridge_progress(flow);
    if (result < 0) {
        flow->bridge->backend_failures++;
        tcp_shift_bridge_flow_abort(flow);
        return ERR_ABRT;
    }
    return ERR_OK;
}

static void tcp_shift_bridge_public_error(void *arg, err_t err)
{
    struct tcp_shift_bridge_flow *flow = arg;

    (void)err;
    if (flow == NULL) {
        return;
    }
    flow->bridge->public_errors++;
    flow->pcb = NULL;
    tcp_shift_bridge_flow_release(flow);
}

static err_t tcp_shift_bridge_accept(void *arg,
                                     struct tcp_pcb *newpcb,
                                     err_t err)
{
    struct tcp_shift_bridge *bridge = arg;
    struct tcp_shift_bridge_flow *flow;
    struct sockaddr_in backend_address;
    int connect_result;
    uint32_t events;

    if (bridge == NULL || newpcb == NULL || err != ERR_OK) {
        return ERR_VAL;
    }

    bridge->accepts++;
    flow = calloc(1, sizeof(*flow));
    if (flow == NULL) {
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    flow->bridge = bridge;
    flow->pcb = newpcb;
    flow->backend_fd = socket(AF_INET,
                              SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    flow->backend_watch.fd = -1;
    if (flow->backend_fd < 0) {
        free(flow);
        bridge->backend_failures++;
        tcp_abort(newpcb);
        return ERR_ABRT;
    }
    if (tcp_shift_bridge_configure_backend_socket(flow) < 0) {
        close(flow->backend_fd);
        free(flow);
        bridge->backend_failures++;
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    memset(&backend_address, 0, sizeof(backend_address));
    backend_address.sin_family = AF_INET;
    backend_address.sin_port = htons(bridge->backend_port);
    backend_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    connect_result = connect(flow->backend_fd,
                             (const struct sockaddr *)&backend_address,
                             sizeof(backend_address));
    if (connect_result == 0) {
        flow->backend_connected = 1U;
        bridge->backend_connects++;
    } else if (errno == EINPROGRESS) {
        flow->backend_connecting = 1U;
    } else {
        close(flow->backend_fd);
        free(flow);
        bridge->backend_failures++;
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    flow->next = bridge->flows;
    bridge->flows = flow;
    bridge->active_flows++;
    if (bridge->active_flows > bridge->peak_active_flows) {
        bridge->peak_active_flows = bridge->active_flows;
    }
    tcp_shift_bridge_flow_set_callbacks(flow);

    events = tcp_shift_bridge_backend_events(flow);
    if (tcp_shift_lwip_loop_watch_add(bridge->loop, &flow->backend_watch,
                                      flow->backend_fd, events,
                                      tcp_shift_bridge_backend_ready, flow) < 0) {
        bridge->backend_failures++;
        tcp_shift_bridge_flow_abort(flow);
        return ERR_ABRT;
    }
    return ERR_OK;
}

static int tcp_shift_bridge_start_type(struct tcp_shift_bridge *bridge,
                                       struct tcp_shift_lwip_loop *loop,
                                       uint16_t public_port,
                                       uint16_t backend_port,
                                       u8_t ip_type,
                                       const ip_addr_t *bind_address)
{
    struct tcp_pcb *pcb;
    struct tcp_pcb *listener;
    err_t err;

    if (bridge == NULL || loop == NULL || loop->epoll_fd < 0 ||
        public_port == 0U || backend_port == 0U || bind_address == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(bridge, 0, sizeof(*bridge));
    bridge->loop = loop;
    bridge->public_port = public_port;
    bridge->backend_port = backend_port;

    pcb = tcp_new_ip_type(ip_type);
    if (pcb == NULL) {
        errno = ENOMEM;
        return -1;
    }
    err = tcp_bind(pcb, bind_address, public_port);
    if (err != ERR_OK) {
        tcp_abort(pcb);
        errno = EADDRINUSE;
        return -1;
    }

    listener = tcp_listen_with_backlog_and_err(pcb, 16, &err);
    if (listener == NULL || err != ERR_OK) {
        if (listener == NULL) {
            tcp_abort(pcb);
        }
        errno = ENOMEM;
        return -1;
    }

    bridge->listener = listener;
    tcp_arg(listener, bridge);
    tcp_accept(listener, tcp_shift_bridge_accept);
    return 0;
}

int tcp_shift_bridge_start_ipv4(struct tcp_shift_bridge *bridge,
                                struct tcp_shift_lwip_loop *loop,
                                uint16_t public_port,
                                uint16_t backend_port)
{
    return tcp_shift_bridge_start_type(bridge, loop, public_port, backend_port,
                                       IPADDR_TYPE_V4, IP4_ADDR_ANY);
}

int tcp_shift_bridge_start_ipv6(struct tcp_shift_bridge *bridge,
                                struct tcp_shift_lwip_loop *loop,
                                uint16_t public_port,
                                uint16_t backend_port)
{
    return tcp_shift_bridge_start_type(bridge, loop, public_port, backend_port,
                                       IPADDR_TYPE_V6, IP6_ADDR_ANY);
}

void tcp_shift_bridge_stop(struct tcp_shift_bridge *bridge)
{
    if (bridge == NULL) {
        return;
    }

    if (bridge->listener != NULL) {
        struct tcp_pcb *listener = bridge->listener;

        bridge->listener = NULL;
        tcp_arg(listener, NULL);
        tcp_accept(listener, NULL);
        if (tcp_close(listener) != ERR_OK) {
            tcp_abort(listener);
        }
    }

    while (bridge->flows != NULL) {
        tcp_shift_bridge_flow_abort(bridge->flows);
    }
    bridge->loop = NULL;
}
