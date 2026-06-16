#define _GNU_SOURCE /* memmem on glibc */
#include "app.h"

#include <stdio.h>
#include <string.h>

#include "stack.h"
#include "tcp.h"

/* ---- UDP echo (port 7) ---------------------------------------------- */
static void udp_echo(struct stack *s, uint32_t src_ip, uint16_t src_port,
                     uint16_t dst_port, const uint8_t *data, size_t len,
                     void *user) {
    (void)user;
    udp_send(s, src_ip, src_port, dst_port, data, len);
}

/* ---- TCP echo (port 9999) ------------------------------------------- */
static void tcp_echo_recv(struct tcp_conn *c, const uint8_t *data, size_t len,
                          void *user) {
    (void)user;
    tcp_send(c, data, len);
}

/* ---- HTTP/1.0 (port 80) --------------------------------------------- *
 *
 * Accumulate request bytes until the blank line that ends the headers,
 * then send a canned 200 and close. Per-connection state comes from a
 * fixed pool so the app stays allocation-free. */
struct http_state {
    int    used;
    char   buf[2048];
    size_t len;
    int    responded;
};
static struct http_state http_pool[TCP_MAX_CONNS];

static void http_accept(struct tcp_conn *c, void *user) {
    (void)user;
    for (size_t i = 0; i < TCP_MAX_CONNS; i++) {
        if (!http_pool[i].used) {
            http_pool[i] = (struct http_state){.used = 1};
            c->user = &http_pool[i];
            return;
        }
    }
    c->user = NULL; /* pool exhausted; request will simply be ignored */
}

static void http_recv(struct tcp_conn *c, const uint8_t *data, size_t len,
                      void *user) {
    (void)user;
    struct http_state *st = c->user;
    if (!st || st->responded)
        return;

    if (st->len + len > sizeof(st->buf))
        len = sizeof(st->buf) - st->len;
    memcpy(st->buf + st->len, data, len);
    st->len += len;

    if (st->len >= 4 && memmem(st->buf, st->len, "\r\n\r\n", 4)) {
        static const char body[] =
            "<!doctype html><title>usertcp</title>"
            "<h1>Served from a user-space TCP/IP stack</h1>"
            "<p>Every byte of this response — IP, TCP, and HTTP — was framed "
            "by hand-written C on top of a TUN device.</p>\n";
        char resp[512];
        int n = snprintf(resp, sizeof(resp),
                         "HTTP/1.0 200 OK\r\n"
                         "Content-Type: text/html; charset=utf-8\r\n"
                         "Content-Length: %zu\r\n"
                         "Connection: close\r\n"
                         "\r\n%s",
                         sizeof(body) - 1, body);
        st->responded = 1;
        tcp_send(c, resp, (size_t)n);
        tcp_close(c);
    }
}

static void http_close(struct tcp_conn *c, void *user) {
    (void)user;
    struct http_state *st = c->user;
    if (st)
        st->used = 0;
    c->user = NULL;
}

void apps_register(struct stack *s) {
    udp_listen(s, APP_UDP_ECHO_PORT, udp_echo, NULL);
    tcp_listen(s, APP_TCP_ECHO_PORT, NULL, tcp_echo_recv, NULL, NULL);
    tcp_listen(s, APP_HTTP_PORT, http_accept, http_recv, http_close, NULL);
}
