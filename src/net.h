#ifndef USERTCP_NET_H
#define USERTCP_NET_H

/* Wire-format structures, protocol constants, and checksum helpers shared
 * by every layer of the stack. Everything here is pure byte manipulation:
 * no I/O, no allocation, no platform dependencies beyond <arpa/inet.h> for
 * htons/ntohl. That keeps the whole protocol core unit-testable on the host
 * without a TUN device or root. */

#include <arpa/inet.h> /* htons, htonl, ntohs, ntohl */
#include <stddef.h>
#include <stdint.h>

/* ---- IP protocol numbers (RFC 790) ---------------------------------- */
#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

/* ---- IPv4 header (RFC 791), 20 bytes without options ---------------- */
struct ipv4_hdr {
    uint8_t  ver_ihl;   /* version (high nibble) | IHL in 32-bit words   */
    uint8_t  tos;
    uint16_t total_len; /* header + payload, network order               */
    uint16_t id;
    uint16_t frag_off;  /* flags (high 3 bits) | fragment offset         */
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;  /* over the header only                          */
    uint32_t src;       /* network order                                 */
    uint32_t dst;       /* network order                                 */
} __attribute__((packed));

#define IPV4_VERSION(h)  ((h)->ver_ihl >> 4)
#define IPV4_IHL(h)      ((h)->ver_ihl & 0x0f)
#define IPV4_HDR_LEN(h)  (IPV4_IHL(h) * 4u)
#define IP_DF 0x4000 /* don't fragment */
#define IP_MF 0x2000 /* more fragments */
#define IP_FRAG_OFFSET_MASK 0x1fff

/* ---- ICMP echo (RFC 792), 8-byte header ----------------------------- */
struct icmp_hdr {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum; /* over header + payload                          */
    uint16_t id;
    uint16_t seq;
} __attribute__((packed));

#define ICMP_ECHO_REQUEST 8
#define ICMP_ECHO_REPLY   0

/* ---- UDP (RFC 768), 8-byte header ----------------------------------- */
struct udp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;   /* header + payload                              */
    uint16_t checksum; /* over pseudo-header + UDP header + payload     */
} __attribute__((packed));

/* ---- TCP (RFC 9293), 20-byte header without options ----------------- */
struct tcp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_off; /* data offset in 32-bit words (high nibble)     */
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urg_ptr;
} __attribute__((packed));

#define TCP_DATA_OFFSET(h) (((h)->data_off >> 4) * 4u)

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10
#define TCP_URG 0x20

/* ---- Pseudo-header for TCP/UDP checksums (RFC 793 §3.1) -------------- */
struct pseudo_hdr {
    uint32_t src;
    uint32_t dst;
    uint8_t  zero;
    uint8_t  proto;
    uint16_t len; /* length of the L4 header + payload                  */
} __attribute__((packed));

/* ---- Internet checksum (RFC 1071) ----------------------------------- *
 *
 * The one's-complement sum is associative, so a checksum over several
 * regions is computed by threading the running 32-bit accumulator through
 * csum_partial() for each region, then folding once at the end. This is how
 * the pseudo-header is combined with the L4 segment without copying them
 * into one contiguous buffer. */

uint32_t csum_partial(const void *data, size_t len, uint32_t start);
uint16_t csum_fold(uint32_t sum);

/* Convenience: checksum over a single contiguous region. */
static inline uint16_t inet_checksum(const void *data, size_t len) {
    return csum_fold(csum_partial(data, len, 0));
}

/* L4 checksum over the IPv4 pseudo-header followed by the segment. */
uint16_t l4_checksum(uint32_t src, uint32_t dst, uint8_t proto,
                     const void *segment, size_t seg_len);

/* ---- 32-bit serial-number arithmetic (RFC 1982) --------------------- *
 *
 * TCP sequence and acknowledgement numbers live in a 32-bit space that
 * wraps. Direct < comparison is wrong near the wrap point; these compare
 * via signed difference so "a is before b" stays correct across 2^32. */
static inline int seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static inline int seq_le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }
static inline int seq_gt(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }
static inline int seq_ge(uint32_t a, uint32_t b) { return (int32_t)(a - b) >= 0; }

#endif /* USERTCP_NET_H */
