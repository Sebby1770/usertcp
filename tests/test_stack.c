/* Host-side tests for the protocol core. No TUN device and no root: the
 * stack's output sink is redirected into an in-memory capture queue, crafted
 * packets are pushed through stack_input(), and the emitted packets are
 * checked field by field. This is the same idea as packetdrill, scoped down
 * to a single self-contained C file. */

#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/net.h"
#include "../src/stack.h"
#include "../src/tcp.h"
#include "../src/app.h"

/* ---- tiny test framework -------------------------------------------- */
static int g_checks = 0, g_fails = 0;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_fails++;                                                         \
            printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__);         \
        }                                                                      \
    } while (0)

/* ---- packet capture sink -------------------------------------------- */
#define CAP_MAX 64
static uint8_t cap_buf[CAP_MAX][2048];
static size_t  cap_len[CAP_MAX];
static int     cap_count;

static void cap_reset(void) { cap_count = 0; }
static void cap_output(void *user, const uint8_t *pkt, size_t len) {
    (void)user;
    if (cap_count < CAP_MAX) {
        memcpy(cap_buf[cap_count], pkt, len);
        cap_len[cap_count] = len;
        cap_count++;
    }
}

static uint32_t g_local, g_peer; /* filled from inet_addr in main */

/* ---- packet builders ------------------------------------------------ */
static size_t build_ip(uint8_t proto, uint32_t src, uint32_t dst,
                       const uint8_t *payload, size_t plen, uint8_t *out) {
    struct ipv4_hdr *ip = (struct ipv4_hdr *)out;
    ip->ver_ihl = 0x45;
    ip->tos = 0;
    ip->total_len = htons((uint16_t)(sizeof(*ip) + plen));
    ip->id = 0;
    ip->frag_off = htons(IP_DF);
    ip->ttl = 64;
    ip->proto = proto;
    ip->checksum = 0;
    ip->src = src;
    ip->dst = dst;
    ip->checksum = htons(inet_checksum(ip, sizeof(*ip)));
    memcpy(out + sizeof(*ip), payload, plen);
    return sizeof(*ip) + plen;
}

static size_t build_tcp(uint32_t seq, uint32_t ack, uint8_t flags,
                        uint16_t sport, uint16_t dport, const uint8_t *data,
                        size_t dlen, uint8_t *out) {
    struct tcp_hdr *t = (struct tcp_hdr *)out;
    memset(t, 0, sizeof(*t));
    t->src_port = htons(sport);
    t->dst_port = htons(dport);
    t->seq = htonl(seq);
    t->ack = htonl(ack);
    t->data_off = (uint8_t)(5 << 4);
    t->flags = flags;
    t->window = htons(65535);
    if (dlen)
        memcpy(out + sizeof(*t), data, dlen);
    t->checksum = 0;
    uint16_t cs = l4_checksum(g_peer, g_local, IP_PROTO_TCP, out,
                              sizeof(*t) + dlen);
    t->checksum = htons(cs == 0 ? 0xffff : cs);
    return sizeof(*t) + dlen;
}

/* Find the first captured packet of a given IP protocol; -1 if none. */
static int cap_find(uint8_t proto) {
    for (int i = 0; i < cap_count; i++) {
        struct ipv4_hdr *ip = (struct ipv4_hdr *)cap_buf[i];
        if (ip->proto == proto)
            return i;
    }
    return -1;
}

static struct tcp_hdr *cap_tcp(int idx) {
    struct ipv4_hdr *ip = (struct ipv4_hdr *)cap_buf[idx];
    return (struct tcp_hdr *)(cap_buf[idx] + IPV4_HDR_LEN(ip));
}

/* ---- tests ---------------------------------------------------------- */

static void test_checksum(void) {
    printf("test_checksum\n");
    /* A correct header must checksum to zero when verified. */
    uint8_t pkt[64];
    uint8_t payload[4] = {1, 2, 3, 4};
    build_ip(IP_PROTO_UDP, g_peer, g_local, payload, sizeof(payload), pkt);
    struct ipv4_hdr *ip = (struct ipv4_hdr *)pkt;
    CHECK(inet_checksum(ip, sizeof(*ip)) == 0, "valid IP header sums to 0");

    /* Flip a byte: checksum must no longer be zero. */
    pkt[1] ^= 0xff;
    CHECK(inet_checksum(ip, sizeof(*ip)) != 0, "corrupt header detected");

    /* The worked example from RFC 1071 §3: this byte sequence checksums to
     * 0x220d. */
    uint8_t v[] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};
    CHECK(inet_checksum(v, sizeof(v)) == 0x220d, "RFC 1071 worked example");
}

static void test_seq_arith(void) {
    printf("test_seq_arith\n");
    CHECK(seq_lt(1, 2), "1 < 2");
    CHECK(!seq_lt(2, 1), "2 !< 1");
    CHECK(seq_lt(0xffffffffu, 0), "wraparound: max < 0");
    CHECK(seq_gt(0, 0xffffffffu), "wraparound: 0 > max");
    CHECK(seq_le(5, 5), "5 <= 5");
}

static void test_icmp_echo(struct stack *s) {
    printf("test_icmp_echo\n");
    cap_reset();
    struct icmp_hdr icmp;
    uint8_t msg[16];
    memset(&icmp, 0, sizeof(icmp));
    icmp.type = ICMP_ECHO_REQUEST;
    icmp.id = htons(0x1234);
    icmp.seq = htons(7);
    memcpy(msg, &icmp, sizeof(icmp));
    const char body[] = "pingpong";
    memcpy(msg + sizeof(icmp), body, 8);
    struct icmp_hdr *m = (struct icmp_hdr *)msg;
    m->checksum = htons(inet_checksum(msg, sizeof(icmp) + 8));

    uint8_t pkt[64];
    size_t n = build_ip(IP_PROTO_ICMP, g_peer, g_local, msg, sizeof(icmp) + 8,
                        pkt);
    stack_input(s, pkt, n);

    int idx = cap_find(IP_PROTO_ICMP);
    CHECK(idx >= 0, "got an ICMP reply");
    if (idx < 0)
        return;
    struct ipv4_hdr *ip = (struct ipv4_hdr *)cap_buf[idx];
    struct icmp_hdr *r = (struct icmp_hdr *)(cap_buf[idx] + IPV4_HDR_LEN(ip));
    CHECK(ip->src == g_local && ip->dst == g_peer, "addresses swapped");
    CHECK(inet_checksum(ip, IPV4_HDR_LEN(ip)) == 0, "reply IP checksum valid");
    CHECK(r->type == ICMP_ECHO_REPLY, "type is echo reply");
    CHECK(inet_checksum(r, sizeof(*r) + 8) == 0, "reply ICMP checksum valid");
    CHECK(r->id == htons(0x1234) && r->seq == htons(7), "id/seq echoed");
}

static void test_udp_echo(struct stack *s) {
    printf("test_udp_echo\n");
    cap_reset();
    uint8_t seg[64];
    struct udp_hdr *u = (struct udp_hdr *)seg;
    const char *payload = "hello-udp";
    size_t plen = strlen(payload);
    u->src_port = htons(40000);
    u->dst_port = htons(APP_UDP_ECHO_PORT);
    u->length = htons((uint16_t)(sizeof(*u) + plen));
    u->checksum = 0;
    memcpy(seg + sizeof(*u), payload, plen);
    uint16_t cs = l4_checksum(g_peer, g_local, IP_PROTO_UDP, seg,
                              sizeof(*u) + plen);
    u->checksum = htons(cs == 0 ? 0xffff : cs);

    uint8_t pkt[128];
    size_t n = build_ip(IP_PROTO_UDP, g_peer, g_local, seg, sizeof(*u) + plen,
                        pkt);
    stack_input(s, pkt, n);

    int idx = cap_find(IP_PROTO_UDP);
    CHECK(idx >= 0, "got a UDP reply");
    if (idx < 0)
        return;
    struct ipv4_hdr *ip = (struct ipv4_hdr *)cap_buf[idx];
    struct udp_hdr *ru = (struct udp_hdr *)(cap_buf[idx] + IPV4_HDR_LEN(ip));
    CHECK(l4_checksum(ip->src, ip->dst, IP_PROTO_UDP, ru, ntohs(ru->length)) ==
              0,
          "reply UDP checksum valid");
    CHECK(ntohs(ru->dst_port) == 40000, "reply goes back to source port");
    CHECK(memcmp(cap_buf[idx] + IPV4_HDR_LEN(ip) + sizeof(*ru), payload,
                 plen) == 0,
          "payload echoed");
}

static void test_tcp_rst_closed_port(struct stack *s) {
    printf("test_tcp_rst_closed_port\n");
    cap_reset();
    uint8_t seg[64], pkt[128];
    size_t sl = build_tcp(1000, 0, TCP_SYN, 50000, 1234 /* closed */, NULL, 0,
                          seg);
    size_t n = build_ip(IP_PROTO_TCP, g_peer, g_local, seg, sl, pkt);
    stack_input(s, pkt, n);

    int idx = cap_find(IP_PROTO_TCP);
    CHECK(idx >= 0, "got a TCP reply to closed port");
    if (idx < 0)
        return;
    struct tcp_hdr *t = cap_tcp(idx);
    CHECK(t->flags & TCP_RST, "reply has RST");
    CHECK(t->flags & TCP_ACK, "RST to SYN carries ACK");
    CHECK(ntohl(t->ack) == 1001, "RST acks SYN sequence + 1");
}

/* Full lifecycle: SYN -> SYN-ACK -> ACK -> data echo -> FIN -> teardown. */
static void test_tcp_lifecycle(struct stack *s) {
    printf("test_tcp_lifecycle\n");
    uint8_t seg[256], pkt[512];
    uint32_t client_isn = 5000;

    /* 1) SYN */
    cap_reset();
    size_t sl = build_tcp(client_isn, 0, TCP_SYN, 50001, APP_TCP_ECHO_PORT,
                          NULL, 0, seg);
    stack_input(s, pkt, build_ip(IP_PROTO_TCP, g_peer, g_local, seg, sl, pkt));
    int idx = cap_find(IP_PROTO_TCP);
    CHECK(idx >= 0, "SYN produced a reply");
    if (idx < 0)
        return;
    struct tcp_hdr *sa = cap_tcp(idx);
    CHECK((sa->flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK),
          "reply is SYN-ACK");
    CHECK(ntohl(sa->ack) == client_isn + 1, "SYN-ACK acks client ISN + 1");
    {
        struct ipv4_hdr *ip = (struct ipv4_hdr *)cap_buf[idx];
        CHECK(l4_checksum(ip->src, ip->dst, IP_PROTO_TCP, sa,
                          ntohs(ip->total_len) - IPV4_HDR_LEN(ip)) == 0,
              "SYN-ACK checksum valid");
    }
    uint32_t server_isn = ntohl(sa->seq);

    /* 2) final ACK -> ESTABLISHED */
    cap_reset();
    sl = build_tcp(client_isn + 1, server_isn + 1, TCP_ACK, 50001,
                   APP_TCP_ECHO_PORT, NULL, 0, seg);
    stack_input(s, pkt, build_ip(IP_PROTO_TCP, g_peer, g_local, seg, sl, pkt));
    CHECK(cap_find(IP_PROTO_TCP) < 0, "bare ACK produces no reply");

    /* 3) send data -> expect echo back */
    cap_reset();
    const char *data = "round-trip";
    size_t dlen = strlen(data);
    sl = build_tcp(client_isn + 1, server_isn + 1, TCP_ACK | TCP_PSH, 50001,
                   APP_TCP_ECHO_PORT, (const uint8_t *)data, dlen, seg);
    stack_input(s, pkt, build_ip(IP_PROTO_TCP, g_peer, g_local, seg, sl, pkt));
    idx = cap_find(IP_PROTO_TCP);
    CHECK(idx >= 0, "data produced a reply");
    int found_echo = 0;
    for (int i = 0; i < cap_count; i++) {
        struct ipv4_hdr *ip = (struct ipv4_hdr *)cap_buf[i];
        size_t hl = IPV4_HDR_LEN(ip);
        struct tcp_hdr *t = (struct tcp_hdr *)(cap_buf[i] + hl);
        size_t tl = ntohs(ip->total_len) - hl;
        size_t tdoff = TCP_DATA_OFFSET(t);
        if (tl > tdoff && memcmp(cap_buf[i] + hl + tdoff, data, dlen) == 0)
            found_echo = 1;
    }
    CHECK(found_echo, "echo server returned the data");

    /* 4) client closes -> server must ACK and FIN back */
    cap_reset();
    sl = build_tcp(client_isn + 1 + dlen, server_isn + 1, TCP_ACK | TCP_FIN,
                   50001, APP_TCP_ECHO_PORT, NULL, 0, seg);
    stack_input(s, pkt, build_ip(IP_PROTO_TCP, g_peer, g_local, seg, sl, pkt));
    int saw_fin = 0;
    for (int i = 0; i < cap_count; i++) {
        struct ipv4_hdr *ip = (struct ipv4_hdr *)cap_buf[i];
        struct tcp_hdr *t = (struct tcp_hdr *)(cap_buf[i] + IPV4_HDR_LEN(ip));
        if (t->flags & TCP_FIN)
            saw_fin = 1;
    }
    CHECK(saw_fin, "server sent its own FIN");
}

static void test_bad_ip_checksum(struct stack *s) {
    printf("test_bad_ip_checksum\n");
    cap_reset();
    uint8_t msg[8] = {ICMP_ECHO_REQUEST, 0, 0, 0, 0, 0, 0, 0};
    uint8_t pkt[64];
    size_t n = build_ip(IP_PROTO_ICMP, g_peer, g_local, msg, sizeof(msg), pkt);
    pkt[10] ^= 0xff; /* corrupt the IP header checksum field */
    stack_input(s, pkt, n);
    CHECK(cap_count == 0, "packet with bad IP checksum is dropped");
}

int main(void) {
    g_local = inet_addr("10.0.0.2");
    g_peer = inet_addr("10.0.0.1");

    struct stack s;
    stack_init(&s, g_local, cap_output, NULL);
    stack_tick(&s, 1000); /* establish a clock base */
    apps_register(&s);

    test_checksum();
    test_seq_arith();
    test_icmp_echo(&s);
    test_udp_echo(&s);
    test_tcp_rst_closed_port(&s);
    test_tcp_lifecycle(&s);
    test_bad_ip_checksum(&s);

    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
