#include "stack.h"

#include <string.h>

#include "net.h"

void stack_init(struct stack *s, uint32_t local_ip, stack_output_fn out,
                void *user) {
    memset(s, 0, sizeof(*s));
    s->local_ip = local_ip;
    s->output = out;
    s->output_user = user;
    /* Seed for ISN selection; mixed with time and the 4-tuple per RFC 6528
     * spirit (not cryptographic — this is a learning stack). */
    s->isn_base = local_ip * 2654435761u;
}

void stack_send_ip(struct stack *s, uint32_t dst, uint8_t proto,
                   const void *payload, size_t len) {
    uint8_t pkt[2048];
    if (sizeof(struct ipv4_hdr) + len > sizeof(pkt))
        return; /* would need fragmentation; out of scope */

    struct ipv4_hdr *ip = (struct ipv4_hdr *)pkt;
    ip->ver_ihl = 0x45; /* IPv4, 20-byte header */
    ip->tos = 0;
    ip->total_len = htons((uint16_t)(sizeof(*ip) + len));
    ip->id = htons((uint16_t)s->tx_packets);
    ip->frag_off = htons(IP_DF);
    ip->ttl = 64;
    ip->proto = proto;
    ip->checksum = 0;
    ip->src = s->local_ip;
    ip->dst = dst;
    ip->checksum = htons(inet_checksum(ip, sizeof(*ip)));

    memcpy(pkt + sizeof(*ip), payload, len);

    size_t total = sizeof(*ip) + len;
    s->tx_packets++;
    s->tx_bytes += total;
    s->output(s->output_user, pkt, total);
}

/* ---- ICMP ----------------------------------------------------------- */
static void handle_icmp(struct stack *s, uint32_t src, const uint8_t *msg,
                        size_t len) {
    if (len < sizeof(struct icmp_hdr))
        return;
    const struct icmp_hdr *in = (const struct icmp_hdr *)msg;
    if (in->type != ICMP_ECHO_REQUEST || in->code != 0)
        return;

    /* Echo reply: same id/seq/payload, type flipped, checksum recomputed. */
    uint8_t reply[2048];
    if (len > sizeof(reply))
        return;
    memcpy(reply, msg, len);
    struct icmp_hdr *out = (struct icmp_hdr *)reply;
    out->type = ICMP_ECHO_REPLY;
    out->code = 0;
    out->checksum = 0;
    out->checksum = htons(inet_checksum(reply, len));

    stack_send_ip(s, src, IP_PROTO_ICMP, reply, len);
}

/* ---- UDP ------------------------------------------------------------ */
int udp_listen(struct stack *s, uint16_t port, udp_recv_cb cb, void *user) {
    for (int i = 0; i < UDP_MAX_LISTENERS; i++) {
        if (!s->udp_listeners[i].used) {
            s->udp_listeners[i] = (struct udp_listener){
                .used = 1, .port = port, .cb = cb, .user = user};
            return 0;
        }
    }
    return -1;
}

ssize_t udp_send(struct stack *s, uint32_t dst_ip, uint16_t dst_port,
                 uint16_t src_port, const void *data, size_t len) {
    uint8_t seg[2048];
    if (sizeof(struct udp_hdr) + len > sizeof(seg))
        return -1;
    struct udp_hdr *u = (struct udp_hdr *)seg;
    u->src_port = htons(src_port);
    u->dst_port = htons(dst_port);
    u->length = htons((uint16_t)(sizeof(*u) + len));
    u->checksum = 0;
    memcpy(seg + sizeof(*u), data, len);

    uint16_t cs = l4_checksum(s->local_ip, dst_ip, IP_PROTO_UDP, seg,
                              sizeof(*u) + len);
    /* A computed checksum of zero is transmitted as 0xFFFF (RFC 768). */
    u->checksum = htons(cs == 0 ? 0xffff : cs);

    stack_send_ip(s, dst_ip, IP_PROTO_UDP, seg, sizeof(*u) + len);
    return (ssize_t)len;
}

static void handle_udp(struct stack *s, uint32_t src, uint32_t dst,
                       const uint8_t *seg, size_t len) {
    if (len < sizeof(struct udp_hdr))
        return;
    const struct udp_hdr *u = (const struct udp_hdr *)seg;
    uint16_t ulen = ntohs(u->length);
    if (ulen < sizeof(*u) || ulen > len)
        return;

    /* Checksum is optional in IPv4 UDP (0 == not computed). */
    if (u->checksum != 0 && l4_checksum(src, dst, IP_PROTO_UDP, seg, ulen) != 0)
        return;

    uint16_t dport = ntohs(u->dst_port);
    for (int i = 0; i < UDP_MAX_LISTENERS; i++) {
        struct udp_listener *l = &s->udp_listeners[i];
        if (l->used && l->port == dport) {
            l->cb(s, src, ntohs(u->src_port), dport, seg + sizeof(*u),
                  ulen - sizeof(*u), l->user);
            return;
        }
    }
    /* No listener: a real host would send ICMP port-unreachable. */
}

/* ---- IPv4 dispatch -------------------------------------------------- */
void stack_input(struct stack *s, const uint8_t *pkt, size_t len) {
    s->rx_packets++;
    s->rx_bytes += len;

    if (len < sizeof(struct ipv4_hdr))
        return;
    const struct ipv4_hdr *ip = (const struct ipv4_hdr *)pkt;
    if (IPV4_VERSION(ip) != 4)
        return;

    size_t ihl = IPV4_HDR_LEN(ip);
    if (ihl < sizeof(struct ipv4_hdr) || ihl > len)
        return;
    if (inet_checksum(ip, ihl) != 0)
        return; /* bad header checksum */

    uint16_t total = ntohs(ip->total_len);
    if (total < ihl || total > len)
        return;

    /* No fragment reassembly: drop any fragment (MF set or non-zero offset). */
    uint16_t frag = ntohs(ip->frag_off);
    if ((frag & IP_MF) || (frag & IP_FRAG_OFFSET_MASK))
        return;

    /* Only packets addressed to us. */
    if (ip->dst != s->local_ip)
        return;

    const uint8_t *payload = pkt + ihl;
    size_t plen = total - ihl;

    switch (ip->proto) {
    case IP_PROTO_ICMP:
        handle_icmp(s, ip->src, payload, plen);
        break;
    case IP_PROTO_UDP:
        handle_udp(s, ip->src, ip->dst, payload, plen);
        break;
    case IP_PROTO_TCP:
        tcp_input(s, ip->src, ip->dst, payload, plen);
        break;
    default:
        break;
    }
}

void stack_tick(struct stack *s, uint32_t now_ms) {
    s->now_ms = now_ms;
    tcp_tick(s, now_ms);
}
