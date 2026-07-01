#include "tcp.h"

#include <string.h>

#include "net.h"
#include "stack.h"

/* A from-scratch TCP implementation covering the connection lifecycle:
 * passive open (3-way handshake), reliable in-order data transfer with a
 * sliding send window and Go-Back-N retransmission, RFC 6298 RTO
 * estimation, RFC 5681 Reno congestion control (slow start, congestion
 * avoidance, fast retransmit + fast recovery), and the full close sequence
 * including TIME_WAIT. It is a learning implementation: in-order receive
 * only (no reassembly queue), no options beyond the implicit 1400 MSS. */

/* RTO bounds (ms). RFC 6298 mandates a 1s minimum; we lower it so demos and
 * tests over a near-zero-RTT TUN do not stall for a full second per loss. */
#define RTO_MIN_MS     200u
#define RTO_MAX_MS     60000u
#define RTO_INITIAL_MS 1000u

/* ---- helpers -------------------------------------------------------- */

static struct tcp_conn *conn_find(struct stack *s, uint32_t remote_ip,
                                  uint16_t remote_port, uint16_t local_port) {
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcp_conn *c = &s->conns[i];
        if (c->used && c->remote_ip == remote_ip &&
            c->remote_port == remote_port && c->local_port == local_port)
            return c;
    }
    return NULL;
}

static struct tcp_conn *conn_alloc(struct stack *s) {
    for (int i = 0; i < TCP_MAX_CONNS; i++)
        if (!s->conns[i].used) {
            memset(&s->conns[i], 0, sizeof(s->conns[i]));
            s->conns[i].used = 1;
            s->conns[i].stack = s;
            return &s->conns[i];
        }
    return NULL;
}

static struct tcp_listener *listener_find(struct stack *s, uint16_t port) {
    for (int i = 0; i < TCP_MAX_LISTENERS; i++)
        if (s->tcp_listeners[i].used && s->tcp_listeners[i].port == port)
            return &s->tcp_listeners[i];
    return NULL;
}

static uint32_t gen_isn(struct stack *s, uint16_t lport, uint16_t rport) {
    /* RFC 6528 recommends a keyed hash of the 4-tuple plus a timer. This is
     * a deterministic stand-in — fine for learning, not for the internet. */
    return s->isn_base + s->now_ms * 250000u +
           ((uint32_t)lport << 16) + rport + (uint32_t)(s->tx_packets * 7u);
}

static void tcp_notify_close(struct tcp_conn *c) {
    if (c->accepted && !c->closed_notified) {
        c->closed_notified = 1;
        if (c->listener && c->listener->on_close)
            c->listener->on_close(c, c->user);
    }
}

static void tcp_free(struct tcp_conn *c) {
    c->used = 0;
}

/* ---- segment emission ----------------------------------------------- */

static void tcp_output(struct tcp_conn *c, uint32_t seq, uint8_t flags,
                       const uint8_t *data, size_t dlen) {
    uint8_t seg[sizeof(struct tcp_hdr) + TCP_MSS];
    if (dlen > TCP_MSS)
        dlen = TCP_MSS;

    struct tcp_hdr *t = (struct tcp_hdr *)seg;
    memset(t, 0, sizeof(*t));
    t->src_port = htons(c->local_port);
    t->dst_port = htons(c->remote_port);
    t->seq = htonl(seq);
    t->ack = htonl(c->rcv_nxt);
    t->data_off = (uint8_t)(5 << 4); /* 20-byte header */
    t->flags = flags;
    t->window = htons(TCP_RCV_WND);
    t->urg_ptr = 0;
    if (dlen)
        memcpy(seg + sizeof(*t), data, dlen);

    t->checksum = 0;
    uint16_t cs = l4_checksum(c->local_ip, c->remote_ip, IP_PROTO_TCP, seg,
                              sizeof(*t) + dlen);
    t->checksum = htons(cs == 0 ? 0xffff : cs);

    stack_send_ip(c->stack, c->remote_ip, IP_PROTO_TCP, seg,
                  sizeof(*t) + dlen);
}

/* Stateless reset for a segment with no matching connection. */
static void tcp_send_rst(struct stack *s, uint32_t peer_ip,
                         const struct tcp_hdr *in, uint32_t seg_seqlen) {
    if (in->flags & TCP_RST)
        return; /* never reset a reset */

    struct tcp_hdr t;
    memset(&t, 0, sizeof(t));
    t.src_port = in->dst_port;
    t.dst_port = in->src_port;
    t.data_off = (uint8_t)(5 << 4);
    t.window = 0;

    if (in->flags & TCP_ACK) {
        t.seq = in->ack; /* seq = SEG.ACK */
        t.flags = TCP_RST;
    } else {
        t.seq = 0;
        t.ack = htonl(ntohl(in->seq) + seg_seqlen);
        t.flags = TCP_RST | TCP_ACK;
    }

    t.checksum = 0;
    uint16_t cs = l4_checksum(s->local_ip, peer_ip, IP_PROTO_TCP, &t, sizeof(t));
    t.checksum = htons(cs == 0 ? 0xffff : cs);
    s->resets++;
    stack_send_ip(s, peer_ip, IP_PROTO_TCP, &t, sizeof(t));
}

/* ---- timers --------------------------------------------------------- */

static void rto_arm(struct tcp_conn *c) {
    /* Run the retransmission timer whenever data is in flight. */
    if (c->snd_una != c->snd_nxt) {
        c->rto_running = 1;
        if (c->rto_ms == 0)
            c->rto_ms = RTO_INITIAL_MS;
        c->rto_deadline_ms = c->stack->now_ms + c->rto_ms;
    }
}

static void rto_stop(struct tcp_conn *c) {
    c->rto_running = 0;
}

static void rtt_sample(struct tcp_conn *c, uint32_t now_ms) {
    int32_t r = (int32_t)(now_ms - c->rtt_start_ms);
    if (r < 0)
        r = 0;
    if (!c->srtt_valid) {
        c->srtt_ms = r;
        c->rttvar_ms = r / 2;
        c->srtt_valid = 1;
    } else {
        int32_t err = r - c->srtt_ms;
        int32_t aerr = err < 0 ? -err : err;
        c->rttvar_ms = (3 * c->rttvar_ms + aerr) / 4;
        c->srtt_ms = (7 * c->srtt_ms + r) / 8;
    }
    int32_t rto = c->srtt_ms + 4 * c->rttvar_ms;
    if (rto < (int32_t)RTO_MIN_MS)
        rto = RTO_MIN_MS;
    if (rto > (int32_t)RTO_MAX_MS)
        rto = RTO_MAX_MS;
    c->rto_ms = (uint32_t)rto;
}

/* ---- congestion control (RFC 5681, Reno) ---------------------------- */

/* Initial window: RFC 6928 allows up to 10*MSS. ssthresh starts effectively
 * unbounded so the connection begins in slow start. */
#define TCP_INIT_CWND (10u * TCP_MSS)

static uint32_t flight_size(const struct tcp_conn *c) {
    return c->snd_nxt - c->snd_una; /* bytes sent but not yet acked */
}

/* Resend just the oldest unacked segment — the one the duplicate ACKs imply
 * was lost. Unlike an RTO this does not rewind snd_nxt, so already-sent data
 * stays "in flight" and fast recovery can clock out new segments. */
static void tcp_retransmit_una(struct tcp_conn *c) {
    c->rtt_timing = 0; /* Karn: never sample RTT from a retransmission */
    uint32_t p = c->snd_una;
    uint32_t data_end = c->snd_data_seq + (uint32_t)c->sndbuf_len;
    if (seq_lt(p, data_end)) {
        size_t off = (size_t)(p - c->snd_data_seq);
        size_t avail = c->sndbuf_len - off;
        size_t chunk = avail < TCP_MSS ? avail : TCP_MSS;
        tcp_output(c, p, TCP_ACK | TCP_PSH, c->sndbuf + off, chunk);
    } else if (c->fin_sent && p == c->fin_seq) {
        tcp_output(c, p, TCP_ACK | TCP_FIN, NULL, 0);
    }
    rto_arm(c);
}

/* A new ACK grew snd_una by `acked` bytes. */
static void cc_on_new_ack(struct tcp_conn *c, uint32_t acked) {
    if (c->in_fast_recovery) {
        /* Full ACK ends recovery: deflate the window back to ssthresh. */
        c->in_fast_recovery = 0;
        c->cwnd = c->ssthresh;
        c->dup_acks = 0;
        return;
    }
    c->dup_acks = 0;
    if (c->cwnd < c->ssthresh) {
        /* Slow start: +MSS per ACK (bounded by bytes actually acked). */
        c->cwnd += acked < TCP_MSS ? acked : TCP_MSS;
    } else {
        /* Congestion avoidance: roughly +MSS per RTT. */
        uint32_t inc = (uint32_t)TCP_MSS * TCP_MSS / c->cwnd;
        c->cwnd += inc ? inc : 1;
    }
}

/* A duplicate ACK (no new data acked, data still in flight). */
static void cc_on_dup_ack(struct tcp_conn *c) {
    c->dup_acks++;
    if (c->dup_acks == 3 && !c->in_fast_recovery) {
        uint32_t half = flight_size(c) / 2;
        c->ssthresh = half > 2u * TCP_MSS ? half : 2u * TCP_MSS;
        c->cwnd = c->ssthresh + 3u * TCP_MSS; /* inflate for the 3 dup ACKs */
        c->in_fast_recovery = 1;
        c->recover = c->snd_nxt;
        tcp_retransmit_una(c); /* fast retransmit, before the RTO fires */
    } else if (c->in_fast_recovery) {
        c->cwnd += TCP_MSS; /* each further dup ACK clocks out one segment */
    }
}

/* ---- send path ------------------------------------------------------ */

/* Emit every segment the window now permits: a pending SYN-ACK, buffered
 * data in MSS chunks, then a queued FIN. Also used for retransmission —
 * the caller rewinds snd_nxt to snd_una and calls back in. */
static void tcp_send_pending(struct tcp_conn *c) {
    for (;;) {
        uint32_t p = c->snd_nxt;
        /* RFC 5681: the sender may have at most min(cwnd, rwnd) bytes in
         * flight. (1 = zero-window probe so a closed window can't deadlock.) */
        uint32_t cong = c->snd_wnd < c->cwnd ? c->snd_wnd : c->cwnd;
        uint32_t window = cong ? cong : 1;
        uint32_t win_end = c->snd_una + window;

        if (c->state == TCP_SYN_RCVD && p == c->iss) {
            tcp_output(c, c->iss, TCP_SYN | TCP_ACK, NULL, 0);
            c->snd_nxt = c->iss + 1;
            rto_arm(c);
            continue;
        }

        uint32_t data_end = c->snd_data_seq + (uint32_t)c->sndbuf_len;
        if (seq_lt(p, data_end)) { /* unsent or to-retransmit data at p */
            if (seq_ge(p, win_end))
                break; /* window closed */
            size_t off = (size_t)(p - c->snd_data_seq);
            size_t avail = c->sndbuf_len - off;
            size_t room = (size_t)(win_end - p);
            size_t chunk = avail < room ? avail : room;
            if (chunk > TCP_MSS)
                chunk = TCP_MSS;
            if (chunk == 0)
                break;
            tcp_output(c, p, TCP_ACK | TCP_PSH, c->sndbuf + off, chunk);
            c->snd_nxt = p + (uint32_t)chunk;
            if (!c->rtt_timing) {
                c->rtt_timing = 1;
                c->rtt_seq = c->snd_nxt;
                c->rtt_start_ms = c->stack->now_ms;
            }
            rto_arm(c);
            continue;
        }

        /* FIN sits just past the last data byte. */
        if (c->fin_queued && p == data_end &&
            (!c->fin_sent || p == c->fin_seq)) {
            if (seq_ge(p, win_end))
                break;
            c->fin_seq = p;
            c->fin_sent = 1;
            tcp_output(c, p, TCP_ACK | TCP_FIN, NULL, 0);
            c->snd_nxt = p + 1;
            rto_arm(c);
            continue;
        }

        break;
    }
}

/* ---- public app API ------------------------------------------------- */

int tcp_listen(struct stack *s, uint16_t port, tcp_accept_cb on_accept,
               tcp_recv_cb on_recv, tcp_close_cb on_close, void *user) {
    for (int i = 0; i < TCP_MAX_LISTENERS; i++) {
        if (!s->tcp_listeners[i].used) {
            s->tcp_listeners[i] = (struct tcp_listener){
                .used = 1,
                .port = port,
                .on_accept = on_accept,
                .on_recv = on_recv,
                .on_close = on_close,
                .user = user,
            };
            return 0;
        }
    }
    return -1;
}

ssize_t tcp_send(struct tcp_conn *c, const void *data, size_t len) {
    if (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT)
        return -1;
    size_t room = TCP_SND_BUF - c->sndbuf_len;
    if (len > room)
        len = room;
    memcpy(c->sndbuf + c->sndbuf_len, data, len);
    c->sndbuf_len += len;
    tcp_send_pending(c);
    return (ssize_t)len;
}

void tcp_close(struct tcp_conn *c) {
    if (c->fin_queued)
        return;
    c->fin_queued = 1;
    if (c->state == TCP_ESTABLISHED)
        c->state = TCP_FIN_WAIT_1;
    else if (c->state == TCP_CLOSE_WAIT)
        c->state = TCP_LAST_ACK;
    tcp_send_pending(c);
}

/* ---- receive path --------------------------------------------------- */

/* Pop acknowledged bytes off the front of the send buffer and advance
 * snd_una. Returns flags describing which control bytes were acked. */
static void ack_update(struct tcp_conn *c, uint32_t ack, int *syn_acked,
                       int *fin_acked) {
    uint32_t old_una = c->snd_una;
    *syn_acked = (old_una == c->iss) && seq_gt(ack, c->iss);
    *fin_acked = c->fin_sent && seq_le(old_una, c->fin_seq) &&
                 seq_gt(ack, c->fin_seq);

    if (c->sndbuf_len > 0 && seq_ge(ack, c->snd_data_seq)) {
        uint32_t popped = ack - c->snd_data_seq;
        if (popped > c->sndbuf_len)
            popped = (uint32_t)c->sndbuf_len; /* ack also covers the FIN */
        memmove(c->sndbuf, c->sndbuf + popped, c->sndbuf_len - popped);
        c->sndbuf_len -= popped;
        c->snd_data_seq += popped;
    }
    c->snd_una = ack;
}

/* Park an in-window segment that arrived ahead of rcv_nxt. */
static void ooo_store(struct tcp_conn *c, uint32_t seq, const uint8_t *data,
                      size_t dlen) {
    if (dlen == 0 || dlen > TCP_MSS)
        return;
    for (int i = 0; i < TCP_OOO_SEGS; i++)
        if (c->ooo[i].used && c->ooo[i].seq == seq)
            return; /* already buffered */
    for (int i = 0; i < TCP_OOO_SEGS; i++) {
        if (!c->ooo[i].used) {
            c->ooo[i].used = 1;
            c->ooo[i].seq = seq;
            c->ooo[i].len = (uint16_t)dlen;
            memcpy(c->ooo[i].data, data, dlen);
            return;
        }
    }
    /* Table full: drop; the peer will retransmit. */
}

/* Deliver any parked segments that are now contiguous with rcv_nxt. Returns
 * non-zero if anything was delivered. */
static int ooo_drain(struct tcp_conn *c) {
    int delivered = 0;
    for (int progress = 1; progress;) {
        progress = 0;
        for (int i = 0; i < TCP_OOO_SEGS; i++) {
            if (!c->ooo[i].used)
                continue;
            if (c->ooo[i].seq == c->rcv_nxt) {
                if (c->listener && c->listener->on_recv)
                    c->listener->on_recv(c, c->ooo[i].data, c->ooo[i].len,
                                         c->user);
                c->rcv_nxt += c->ooo[i].len;
                c->ooo[i].used = 0;
                delivered = 1;
                progress = 1;
            } else if (seq_lt(c->ooo[i].seq, c->rcv_nxt)) {
                c->ooo[i].used = 0; /* stale / already delivered */
            }
        }
    }
    return delivered;
}

static void deliver_segment(struct tcp_conn *c, uint8_t flags, uint32_t seq,
                            const uint8_t *data, size_t dlen) {
    int can_recv = c->state == TCP_ESTABLISHED ||
                   c->state == TCP_FIN_WAIT_1 || c->state == TCP_FIN_WAIT_2;
    if (!can_recv)
        return;

    if (seq != c->rcv_nxt) {
        /* Not the next byte. If it is future data that fits in the window,
         * park it for reassembly; then re-ACK to report our real rcv_nxt. */
        if (seq_gt(seq, c->rcv_nxt) &&
            seq_lt(seq, c->rcv_nxt + TCP_RCV_WND))
            ooo_store(c, seq, data, dlen);
        tcp_output(c, c->snd_nxt, TCP_ACK, NULL, 0);
        return;
    }

    int got_something = 0;
    if (dlen > 0) {
        c->rcv_nxt += (uint32_t)dlen;
        got_something = 1;
        if (c->listener && c->listener->on_recv)
            c->listener->on_recv(c, data, dlen, c->user);
    }

    if (flags & TCP_FIN) {
        c->rcv_nxt += 1; /* FIN consumes a sequence number */
        got_something = 1;
        switch (c->state) {
        case TCP_ESTABLISHED:
            c->state = TCP_CLOSE_WAIT;
            tcp_output(c, c->snd_nxt, TCP_ACK, NULL, 0);
            tcp_notify_close(c);
            tcp_close(c); /* nothing more to send: FIN back immediately */
            return;
        case TCP_FIN_WAIT_1:
            /* Simultaneous close: our FIN is still unacked. */
            c->state = TCP_CLOSING;
            break;
        case TCP_FIN_WAIT_2:
            c->state = TCP_TIME_WAIT;
            c->timewait_deadline_ms = c->stack->now_ms + TCP_TIME_WAIT_MS;
            tcp_notify_close(c);
            break;
        default:
            break;
        }
    } else if (ooo_drain(c)) {
        /* This segment filled a gap: flush the now-contiguous parked data. */
        got_something = 1;
    }

    if (got_something)
        tcp_output(c, c->snd_nxt, TCP_ACK, NULL, 0); /* ACK new data/FIN */
}

static void segment_arrives(struct tcp_conn *c, uint32_t seq, uint32_t ack,
                            uint8_t flags, uint32_t wnd, const uint8_t *data,
                            size_t dlen) {
    /* RST: abort if it falls in the receive window. */
    if (flags & TCP_RST) {
        if (seq == c->rcv_nxt || c->state == TCP_SYN_RCVD) {
            tcp_notify_close(c);
            tcp_free(c);
        }
        return;
    }

    /* Duplicate SYN while we are still establishing: resend SYN-ACK. */
    if ((flags & TCP_SYN) && c->state == TCP_SYN_RCVD && seq == c->irs) {
        c->snd_nxt = c->iss; /* rewind so send_pending re-emits the SYN-ACK */
        tcp_send_pending(c);
        return;
    }

    if (flags & TCP_ACK) {
        if (seq_gt(ack, c->snd_nxt)) {
            /* Acks data we never sent: re-ACK and drop. */
            tcp_output(c, c->snd_nxt, TCP_ACK, NULL, 0);
            return;
        }
        uint32_t old_una = c->snd_una;
        if (seq_gt(ack, c->snd_una)) {
            int syn_acked = 0, fin_acked = 0;
            ack_update(c, ack, &syn_acked, &fin_acked);
            c->snd_wnd = wnd;
            cc_on_new_ack(c, ack - old_una);

            if (c->rtt_timing && seq_ge(ack, c->rtt_seq)) {
                rtt_sample(c, c->stack->now_ms);
                c->rtt_timing = 0;
            }

            if (c->snd_una == c->snd_nxt)
                rto_stop(c);
            else
                rto_arm(c); /* fresh data acked: restart timer */

            /* State transitions driven by what just got acknowledged. */
            if (syn_acked && c->state == TCP_SYN_RCVD) {
                c->state = TCP_ESTABLISHED;
                c->accepted = 1;
                if (c->listener && c->listener->on_accept)
                    c->listener->on_accept(c, c->listener->user);
            }
            if (fin_acked) {
                switch (c->state) {
                case TCP_FIN_WAIT_1:
                    c->state = TCP_FIN_WAIT_2;
                    break;
                case TCP_CLOSING:
                    c->state = TCP_TIME_WAIT;
                    c->timewait_deadline_ms =
                        c->stack->now_ms + TCP_TIME_WAIT_MS;
                    tcp_notify_close(c);
                    break;
                case TCP_LAST_ACK:
                    tcp_notify_close(c);
                    tcp_free(c);
                    return;
                default:
                    break;
                }
            }
        } else if (ack == c->snd_una) {
            /* No new data acked. Still track the peer's window, and treat a
             * bare data-less ACK with data in flight as a duplicate ACK —
             * the fast-retransmit loss signal (RFC 5681). */
            c->snd_wnd = wnd;
            if (c->snd_una != c->snd_nxt && dlen == 0 &&
                !(flags & (TCP_SYN | TCP_FIN)))
                cc_on_dup_ack(c);
        }
        /* else: stale ACK below snd_una — ignore. */
    }

    deliver_segment(c, flags, seq, data, dlen);

    /* The app may have queued a reply inside on_recv; flush it. */
    if (c->used)
        tcp_send_pending(c);
}

void tcp_input(struct stack *s, uint32_t src, uint32_t dst,
               const uint8_t *seg, size_t len) {
    if (len < sizeof(struct tcp_hdr))
        return;
    const struct tcp_hdr *th = (const struct tcp_hdr *)seg;

    size_t doff = TCP_DATA_OFFSET(th);
    if (doff < sizeof(struct tcp_hdr) || doff > len)
        return;
    if (l4_checksum(src, dst, IP_PROTO_TCP, seg, len) != 0)
        return; /* corrupt segment */

    uint8_t flags = th->flags;
    uint32_t seq = ntohl(th->seq);
    uint32_t ack = ntohl(th->ack);
    uint32_t wnd = ntohs(th->window);
    const uint8_t *data = seg + doff;
    size_t dlen = len - doff;
    uint16_t sport = ntohs(th->src_port);
    uint16_t dport = ntohs(th->dst_port);

    struct tcp_conn *c = conn_find(s, src, sport, dport);
    if (c) {
        segment_arrives(c, seq, ack, flags, wnd, data, dlen);
        return;
    }

    /* No connection. A bare SYN to a listening port opens one. */
    if (flags & TCP_RST)
        return;

    if ((flags & TCP_SYN) && !(flags & TCP_ACK)) {
        struct tcp_listener *l = listener_find(s, dport);
        if (!l) {
            uint32_t seglen = dlen + 1; /* SYN counts as one */
            tcp_send_rst(s, src, th, seglen);
            return;
        }
        c = conn_alloc(s);
        if (!c)
            return; /* table full: drop, peer will retransmit the SYN */

        c->local_ip = dst;
        c->remote_ip = src;
        c->local_port = dport;
        c->remote_port = sport;
        c->listener = l;
        c->state = TCP_SYN_RCVD;
        c->irs = seq;
        c->rcv_nxt = seq + 1;
        c->iss = gen_isn(s, dport, sport);
        c->snd_una = c->iss;
        c->snd_nxt = c->iss;
        c->snd_data_seq = c->iss + 1;
        c->snd_wnd = wnd;
        c->rto_ms = RTO_INITIAL_MS;
        c->cwnd = TCP_INIT_CWND;     /* start in slow start (RFC 6928 IW10) */
        c->ssthresh = 0xffffffffu;   /* unbounded until the first loss */
        c->recover = c->iss;
        tcp_send_pending(c); /* emits SYN-ACK */
        return;
    }

    /* Anything else with no connection: reset. */
    uint32_t seglen = dlen + ((flags & TCP_SYN) ? 1 : 0) +
                      ((flags & TCP_FIN) ? 1 : 0);
    tcp_send_rst(s, src, th, seglen);
}

void tcp_tick(struct stack *s, uint32_t now_ms) {
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcp_conn *c = &s->conns[i];
        if (!c->used)
            continue;

        if (c->state == TCP_TIME_WAIT &&
            (int32_t)(now_ms - c->timewait_deadline_ms) >= 0) {
            tcp_free(c);
            continue;
        }

        if (c->rto_running && (int32_t)(now_ms - c->rto_deadline_ms) >= 0) {
            /* Retransmission timeout: back off, rewind, resend (Go-Back-N).
             * Karn's algorithm: do not sample RTT from a retransmission. */
            c->rto_ms = c->rto_ms * 2;
            if (c->rto_ms > RTO_MAX_MS)
                c->rto_ms = RTO_MAX_MS;
            c->rtt_timing = 0;

            /* An RTO is the strongest loss signal: halve ssthresh and drop
             * cwnd to one segment, restarting slow start (RFC 5681). */
            if (c->cwnd) {
                uint32_t half = flight_size(c) / 2;
                c->ssthresh = half > 2u * TCP_MSS ? half : 2u * TCP_MSS;
                c->cwnd = TCP_MSS;
                c->dup_acks = 0;
                c->in_fast_recovery = 0;
            }

            c->snd_nxt = c->snd_una;
            c->rto_running = 0;
            s->retransmits++;
            tcp_send_pending(c);
        }
    }
}
