#include "http_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/tcp.h"

#include "wifi_ap.h"

#define HTTP_PORT 80
#define RX_MAX 6144
#define HDR_MAX 256

typedef struct {
    struct tcp_pcb *pcb;
    char rx[RX_MAX + 1];
    size_t rx_len;
    bool responded;
    char hdr[HDR_MAX];
    size_t hdr_len, hdr_sent;
    http_response_t resp;
    size_t body_sent;
    size_t unacked;
} conn_t;

static struct tcp_pcb *s_listen;

static void conn_free(conn_t *c) {
    if (c->resp.body_owned && c->resp.body) free((void *)c->resp.body);
    free(c);
}

static void conn_close(conn_t *c) {
    struct tcp_pcb *pcb = c->pcb;
    if (pcb) {
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_sent(pcb, NULL);
        tcp_err(pcb, NULL);
        tcp_poll(pcb, NULL, 0);
        if (tcp_close(pcb) != ERR_OK) tcp_abort(pcb);
    }
    conn_free(c);
}

static const char *status_text(int s) {
    switch (s) {
    case 200: return "OK";
    case 204: return "No Content";
    case 302: return "Found";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 413: return "Payload Too Large";
    default: return "Error";
    }
}

// Push as much as the send buffer allows. Static data (the page in flash) and
// owned buffers stay valid until the connection closes, so no copy is needed.
static void conn_pump(conn_t *c) {
    struct tcp_pcb *pcb = c->pcb;
    while (c->hdr_sent < c->hdr_len) {
        u16_t room = tcp_sndbuf(pcb);
        if (!room) break;
        size_t n = c->hdr_len - c->hdr_sent;
        if (n > room) n = room;
        if (tcp_write(pcb, c->hdr + c->hdr_sent, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK) break;
        c->hdr_sent += n;
        c->unacked += n;
    }
    if (c->hdr_sent == c->hdr_len) {
        while (c->body_sent < c->resp.body_len) {
            u16_t room = tcp_sndbuf(pcb);
            if (room < 64) break;
            size_t n = c->resp.body_len - c->body_sent;
            if (n > room) n = room;
            if (n > 2048) n = 2048;
            u8_t flags = c->body_sent + n < c->resp.body_len ? TCP_WRITE_FLAG_MORE : 0;
            if (tcp_write(pcb, c->resp.body + c->body_sent, (u16_t)n, flags) != ERR_OK) break;
            c->body_sent += n;
            c->unacked += n;
        }
    }
    tcp_output(pcb);
}

static void handle_request(conn_t *c) {
    char *hdr_end = strstr(c->rx, "\r\n\r\n");
    if (!hdr_end) return;
    size_t hdr_len = (size_t)(hdr_end - c->rx) + 4;

    size_t content_length = 0;
    char *cl = strstr(c->rx, "Content-Length:");
    if (!cl) cl = strstr(c->rx, "content-length:");
    if (cl && cl < hdr_end) content_length = (size_t)strtoul(cl + 15, NULL, 10);

    http_request_t req;
    memset(&req, 0, sizeof req);
    memset(&c->resp, 0, sizeof c->resp);

    if (hdr_len + content_length > RX_MAX) {
        c->resp.status = 413;
    } else {
        if (c->rx_len < hdr_len + content_length) return;   // wait for the body
        c->rx[hdr_len + content_length] = 0;
        *hdr_end = 0;
        // Request line: METHOD SP TARGET SP VERSION
        char *sp1 = strchr(c->rx, ' ');
        char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
        if (!sp1 || !sp2) {
            c->resp.status = 400;
        } else {
            *sp1 = 0;
            *sp2 = 0;
            req.method = c->rx;
            req.path = sp1 + 1;
            char *q = strchr(sp1 + 1, '?');
            if (q) {
                *q = 0;
                req.query = q + 1;
            } else {
                req.query = "";
            }
            req.body = c->rx + hdr_len;
            req.body_len = content_length;
            web_api_handle(&req, &c->resp);
        }
    }
    c->responded = true;
    wifi_ap_touch();

    if (!c->resp.content_type) c->resp.content_type = "text/plain";
    c->hdr_len = (size_t)snprintf(c->hdr, sizeof c->hdr,
                                  "HTTP/1.0 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
                                  "Cache-Control: no-store\r\nConnection: close\r\n%s\r\n",
                                  c->resp.status, status_text(c->resp.status), c->resp.content_type,
                                  (unsigned)c->resp.body_len, c->resp.extra_headers ? c->resp.extra_headers : "");
    if (c->hdr_len >= sizeof c->hdr) c->hdr_len = sizeof c->hdr - 1;
    conn_pump(c);
}

static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t len) {
    (void)pcb;
    conn_t *c = arg;
    if (!c) return ERR_OK;
    c->unacked = c->unacked > len ? c->unacked - len : 0;
    if (c->hdr_sent == c->hdr_len && c->body_sent == c->resp.body_len && c->unacked == 0) {
        conn_close(c);
        return ERR_OK;
    }
    conn_pump(c);
    return ERR_OK;
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    conn_t *c = arg;
    if (!c) {
        if (p) pbuf_free(p);
        return ERR_OK;
    }
    if (!p || err != ERR_OK) {
        if (p) pbuf_free(p);
        conn_close(c);
        return ERR_OK;
    }
    if (!c->responded) {
        size_t room = RX_MAX - c->rx_len;
        size_t n = p->tot_len < room ? p->tot_len : room;
        pbuf_copy_partial(p, c->rx + c->rx_len, (u16_t)n, 0);
        c->rx_len += n;
        c->rx[c->rx_len] = 0;
        handle_request(c);
        if (!c->responded && c->rx_len >= RX_MAX) {
            c->rx[RX_MAX] = 0;
            memset(&c->resp, 0, sizeof c->resp);
            c->resp.status = 413;
            c->responded = true;
        }
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void on_err(void *arg, err_t err) {
    (void)err;
    conn_t *c = arg;
    if (c) {
        c->pcb = NULL;   // already freed by lwIP
        conn_free(c);
    }
}

static err_t on_poll(void *arg, struct tcp_pcb *pcb) {
    conn_t *c = arg;
    if (!c) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    // Idle for too long (~10 s): give up on this connection.
    tcp_abort(pcb);
    c->pcb = NULL;
    conn_free(c);
    return ERR_ABRT;
}

static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK || !pcb) return ERR_VAL;
    conn_t *c = calloc(1, sizeof *c);
    if (!c) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    c->pcb = pcb;
    tcp_arg(pcb, c);
    tcp_recv(pcb, on_recv);
    tcp_sent(pcb, on_sent);
    tcp_err(pcb, on_err);
    tcp_poll(pcb, on_poll, 20);
    return ERR_OK;
}

void http_server_start(void) {
    if (s_listen) return;
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb) return;
    if (tcp_bind(pcb, IP_ANY_TYPE, HTTP_PORT) != ERR_OK) {
        tcp_close(pcb);
        return;
    }
    s_listen = tcp_listen_with_backlog(pcb, 4);
    tcp_accept(s_listen, on_accept);
}

void http_server_stop(void) {
    if (s_listen) {
        tcp_close(s_listen);
        s_listen = NULL;
    }
}
