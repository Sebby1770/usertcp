#ifndef USERTCP_TCP_H
#define USERTCP_TCP_H

/* TCP connection state and the small callback-based application API.
 *
 * Connections and listeners are stored as fixed arrays inside `struct stack`
 * (see stack.h) so the whole engine is allocation-free and trivially
 * resettable between tests. tcp.h only forward-declares `struct stack` to
 * avoid an include cycle; stack.h includes this header to lay out its
 * connection table. */

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct stack;

/* RFC 9293 connection states. */
enum tcp_state {
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_RCVD,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSING,
    TCP_TIME_WAIT,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
};

#define TCP_MSS          1400
#define TCP_SND_BUF      65535
#define TCP_RCV_WND      65535
#define TCP_MAX_CONNS    64
#define TCP_MAX_LISTENERS 8

/* 2*MSL TIME_WAIT. Real stacks use ~30-120s; shortened here so test and
 * demo cycles do not pile up dormant control blocks. */
#define TCP_TIME_WAIT_MS 10000

struct tcp_conn;

/* A new connection reached ESTABLISHED. Set conn->user here to attach
 * per-connection app state; it is passed back to on_recv/on_close. */
typedef void (*tcp_accept_cb)(struct tcp_conn *c, void *user);
/* In-order bytes arrived. */
typedef void (*tcp_recv_cb)(struct tcp_conn *c, const uint8_t *data,
                            size_t len, void *user);
/* Connection fully closed (peer FIN delivered, or reset). */
typedef void (*tcp_close_cb)(struct tcp_conn *c, void *user);

struct tcp_listener {
    int           used;
    uint16_t      port; /* host order */
    tcp_accept_cb on_accept;
    tcp_recv_cb   on_recv;
    tcp_close_cb  on_close;
    void         *user;
};

struct tcp_conn {
    int           used;
    struct stack *stack;

    uint32_t local_ip, remote_ip;     /* network order */
    uint16_t local_port, remote_port; /* host order    */

    enum tcp_state state;

    /* Send sequence space (RFC 9293 §3.3.1). */
    uint32_t snd_una; /* oldest unacknowledged sequence number */
    uint32_t snd_nxt; /* next sequence number to send          */
    uint32_t iss;     /* our initial send sequence number      */
    uint32_t snd_wnd; /* peer's advertised receive window      */

    /* Congestion control (RFC 5681, TCP Reno). The sender is limited by
     * min(cwnd, snd_wnd). cwnd grows in slow start until it reaches ssthresh,
     * then more slowly in congestion avoidance; 3 duplicate ACKs trigger fast
     * retransmit + fast recovery, and an RTO collapses cwnd back to 1 MSS. */
    uint32_t cwnd;             /* congestion window, bytes              */
    uint32_t ssthresh;         /* slow-start threshold, bytes           */
    int      dup_acks;         /* consecutive duplicate ACKs            */
    int      in_fast_recovery; /* inflating cwnd during fast recovery   */
    uint32_t recover;          /* snd_nxt snapshot at loss (NewReno)    */

    /* Receive sequence space. */
    uint32_t rcv_nxt; /* next sequence number we expect        */
    uint32_t irs;     /* peer's initial sequence number        */

    /* Send buffer: unacknowledged + unsent app bytes. sndbuf[0] carries
     * sequence number snd_data_seq; bytes are popped from the front as they
     * are acknowledged. */
    uint8_t  sndbuf[TCP_SND_BUF];
    size_t   sndbuf_len;
    uint32_t snd_data_seq; /* sequence number of sndbuf[0]            */
    int      fin_queued;   /* app closed; emit FIN after buffered data */
    int      fin_sent;     /* our FIN has been transmitted             */
    uint32_t fin_seq;      /* sequence number of our FIN (if fin_sent) */

    int      accepted;        /* on_accept has fired                   */
    int      closed_notified; /* on_close has fired                    */

    /* Retransmission timer (RFC 6298). */
    int      rto_running;
    uint32_t rto_deadline_ms;
    uint32_t rto_ms;
    int      srtt_valid;
    int32_t  srtt_ms, rttvar_ms;
    int      rtt_timing;  /* a round-trip sample is in flight */
    uint32_t rtt_seq;     /* sequence number being timed (Karn's algorithm) */
    uint32_t rtt_start_ms;

    uint32_t timewait_deadline_ms;

    struct tcp_listener *listener;
    void *user;
};

/* Register a passive listener on `port`. Returns 0 on success, -1 if the
 * listener table is full. */
int tcp_listen(struct stack *s, uint16_t port, tcp_accept_cb on_accept,
               tcp_recv_cb on_recv, tcp_close_cb on_close, void *user);

/* Queue `len` bytes for reliable, ordered delivery. Returns the number of
 * bytes accepted (may be less than len if the send buffer is full). */
ssize_t tcp_send(struct tcp_conn *c, const void *data, size_t len);

/* Begin an active close: flush buffered data, then send FIN. */
void tcp_close(struct tcp_conn *c);

/* ---- called by the IP layer / event loop, not by apps --------------- */
void tcp_input(struct stack *s, uint32_t src, uint32_t dst,
               const uint8_t *seg, size_t len);
void tcp_tick(struct stack *s, uint32_t now_ms);

#endif /* USERTCP_TCP_H */
