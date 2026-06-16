#ifndef USERTCP_STACK_H
#define USERTCP_STACK_H

/* The stack object: owns the local address, the packet output sink, and the
 * UDP/TCP listener and connection tables. It is deliberately transport-
 * agnostic — `stack_input` consumes one inbound IPv4 packet and the output
 * callback emits one outbound IPv4 packet. main.c wires those to a TUN
 * device; the tests wire them to in-memory buffers. */

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "tcp.h"

/* Emit one fully-formed IPv4 packet. */
typedef void (*stack_output_fn)(void *user, const uint8_t *pkt, size_t len);

typedef void (*udp_recv_cb)(struct stack *s, uint32_t src_ip,
                            uint16_t src_port, uint16_t dst_port,
                            const uint8_t *data, size_t len, void *user);

#define UDP_MAX_LISTENERS 8

struct udp_listener {
    int         used;
    uint16_t    port; /* host order */
    udp_recv_cb cb;
    void       *user;
};

struct stack {
    uint32_t        local_ip; /* network order */
    stack_output_fn output;
    void           *output_user;

    uint32_t now_ms;   /* advanced by stack_tick */
    uint32_t isn_base; /* seed for initial sequence numbers */

    struct udp_listener udp_listeners[UDP_MAX_LISTENERS];
    struct tcp_listener tcp_listeners[TCP_MAX_LISTENERS];
    struct tcp_conn     conns[TCP_MAX_CONNS];

    /* Observability counters. */
    uint64_t rx_packets, tx_packets;
    uint64_t rx_bytes, tx_bytes;
    uint64_t retransmits, resets;
};

void stack_init(struct stack *s, uint32_t local_ip, stack_output_fn out,
                void *user);

/* Feed one inbound IPv4 packet through the stack. */
void stack_input(struct stack *s, const uint8_t *pkt, size_t len);

/* Advance time; fires retransmission and TIME_WAIT timers. */
void stack_tick(struct stack *s, uint32_t now_ms);

/* Build an IPv4 header around `payload` and emit it via the output sink.
 * Used by the ICMP, UDP and TCP layers. */
void stack_send_ip(struct stack *s, uint32_t dst, uint8_t proto,
                   const void *payload, size_t len);

/* Register a UDP listener. Returns 0 on success, -1 if the table is full. */
int udp_listen(struct stack *s, uint16_t port, udp_recv_cb cb, void *user);

/* Send a UDP datagram. Returns bytes sent or -1. */
ssize_t udp_send(struct stack *s, uint32_t dst_ip, uint16_t dst_port,
                 uint16_t src_port, const void *data, size_t len);

#endif /* USERTCP_STACK_H */
