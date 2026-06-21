#include "net.h"

/* Accumulate the 16-bit one's-complement sum of `data` into `start`.
 *
 * We sum 16-bit big-endian words into a 32-bit accumulator and let the
 * carries pile up in the high half; csum_fold() folds them back in at the
 * end. An odd trailing byte is treated as the high byte of a final word
 * (low byte zero), which matches how the checksum is defined on the wire. */
uint32_t csum_partial(const void *data, size_t len, uint32_t start) {
    const uint8_t *p = data;
    uint32_t sum = start;

    while (len > 1) {
        sum += (uint32_t)((p[0] << 8) | p[1]);
        p += 2;
        len -= 2;
    }
    if (len == 1)
        sum += (uint32_t)(p[0] << 8);

    return sum;
}

/* Fold the carries down to 16 bits and take the one's complement. */
uint16_t csum_fold(uint32_t sum) {
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)(~sum & 0xffff);
}

uint16_t l4_checksum(uint32_t src, uint32_t dst, uint8_t proto,
                     const void *segment, size_t seg_len) {
    struct pseudo_hdr ph = {
        .src   = src,
        .dst   = dst,
        .zero  = 0,
        .proto = proto,
        .len   = htons((uint16_t)seg_len),
    };
    uint32_t sum = csum_partial(&ph, sizeof(ph), 0);
    sum = csum_partial(segment, seg_len, sum);
    return csum_fold(sum);
}
