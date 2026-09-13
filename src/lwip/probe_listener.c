#include "lwip/probe_listener.h"

#include <errno.h>
#include <string.h>

#include "lwip/err.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"

static void tcp_shift_probe_error(void *arg, err_t err)
{
    struct tcp_shift_probe_listener *listener = arg;

    (void)err;
    if (listener != NULL) {
        listener->errors++;
    }
}

static err_t tcp_shift_probe_recv(void *arg,
                                  struct tcp_pcb *pcb,
                                  struct pbuf *p,
                                  err_t err)
{
    struct tcp_shift_probe_listener *listener = arg;

    if (listener == NULL) {
        if (p != NULL) {
            pbuf_free(p);
        }
        tcp_abort(pcb);
        return ERR_ABRT;
    }

    if (p == NULL) {
        err_t close_err = tcp_close(pcb);

        if (close_err != ERR_OK) {
            listener->errors++;
            tcp_abort(pcb);
            return ERR_ABRT;
        }
        return ERR_OK;
    }

    if (err != ERR_OK) {
        listener->errors++;
        pbuf_free(p);
        return err;
    }

    listener->rx_bytes += p->tot_len;
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t tcp_shift_probe_accept(void *arg,
                                    struct tcp_pcb *newpcb,
                                    err_t err)
{
    struct tcp_shift_probe_listener *listener = arg;

    if (listener == NULL || newpcb == NULL || err != ERR_OK) {
        return ERR_VAL;
    }

    listener->accepts++;
    tcp_arg(newpcb, listener);
    tcp_recv(newpcb, tcp_shift_probe_recv);
    tcp_err(newpcb, tcp_shift_probe_error);
    return ERR_OK;
}

static int tcp_shift_probe_listener_start_type(
    struct tcp_shift_probe_listener *listener,
    uint16_t port,
    u8_t ip_type,
    const ip_addr_t *bind_address)
{
    struct tcp_pcb *pcb;
    struct tcp_pcb *listening;
    err_t err;

    if (listener == NULL || port == 0U || bind_address == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(listener, 0, sizeof(*listener));
    listener->port = port;

    pcb = tcp_new_ip_type(ip_type);
    if (pcb == NULL) {
        errno = ENOMEM;
        return -1;
    }

    err = tcp_bind(pcb, bind_address, port);
    if (err != ERR_OK) {
        tcp_abort(pcb);
        errno = EADDRINUSE;
        return -1;
    }

    listening = tcp_listen_with_backlog_and_err(pcb, 4, &err);
    if (listening == NULL || err != ERR_OK) {
        if (listening == NULL) {
            tcp_abort(pcb);
        }
        errno = ENOMEM;
        return -1;
    }

    listener->pcb = listening;
    tcp_arg(listening, listener);
    tcp_accept(listening, tcp_shift_probe_accept);
    return 0;
}

int tcp_shift_probe_listener_start(struct tcp_shift_probe_listener *listener,
                                   uint16_t port)
{
    return tcp_shift_probe_listener_start_type(listener, port, IPADDR_TYPE_V4,
                                                IP4_ADDR_ANY);
}

int tcp_shift_probe_listener_start_ipv6(struct tcp_shift_probe_listener *listener,
                                        uint16_t port)
{
    return tcp_shift_probe_listener_start_type(listener, port, IPADDR_TYPE_V6,
                                                IP6_ADDR_ANY);
}

void tcp_shift_probe_listener_stop(struct tcp_shift_probe_listener *listener)
{
    err_t err;

    if (listener == NULL || listener->pcb == NULL) {
        return;
    }

    tcp_arg(listener->pcb, NULL);
    tcp_accept(listener->pcb, NULL);
    err = tcp_close(listener->pcb);
    if (err != ERR_OK) {
        listener->errors++;
        tcp_abort(listener->pcb);
    }
    listener->pcb = NULL;
}
