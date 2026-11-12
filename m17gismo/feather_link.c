/* feather_link.c - the host half of the Feather wire format.  Freestanding C99.
 *
 * VENDORED VERBATIM from G4GUO/m17_feather, link/link.c @ sha256 302c5b74b48561e2
 * (with include/link.h as feather_link.h, same bytes).  The firmware on the other
 * end of the cable is built from that copy, and its tests are what prove this
 * codec, so if this file ever needs a change the change belongs there first.
 * Only this include line differs, because the header is not in include/ here.
 */
#include <math.h>
#include <string.h>

#include "feather_link.h"

uint16_t link_crc16(const uint8_t *p, size_t n)
{
    uint16_t crc = 0xFFFFu;
    while (n--) {
        crc ^= (uint16_t)(*p++) << 8;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
    }
    return crc;
}

size_t link_encode(uint8_t *out, size_t cap, uint8_t type, uint8_t seq,
                   const uint8_t *payload, size_t len)
{
    if (len > LINK_MAX_PAYLOAD) return 0;
    size_t total = LINK_OVERHEAD + len;
    if (cap < total) return 0;

    out[0] = LINK_SYNC0;
    out[1] = LINK_SYNC1;
    out[2] = type;
    out[3] = seq;
    out[4] = (uint8_t)(len & 0xFFu);
    out[5] = (uint8_t)((len >> 8) & 0xFFu);
    if (len) memcpy(out + LINK_HDR_LEN, payload, len);

    /* CRC covers type..payload, i.e. everything but the sync word */
    uint16_t crc = link_crc16(out + 2, LINK_HDR_LEN - 2 + len);
    out[LINK_HDR_LEN + len]      = (uint8_t)(crc & 0xFFu);
    out[LINK_HDR_LEN + len + 1]  = (uint8_t)((crc >> 8) & 0xFFu);
    return total;
}

size_t link_pack_dibits(uint8_t *out, const uint8_t *dibits, size_t n)
{
    size_t bytes = (n + 3u) / 4u;
    memset(out, 0, bytes);
    for (size_t i = 0; i < n; i++) {
        /* most significant dibit of the byte is sent first */
        unsigned shift = 6u - 2u * (unsigned)(i & 3u);
        out[i / 4u] |= (uint8_t)((dibits[i] & 3u) << shift);
    }
    return bytes;
}

size_t link_unpack_dibits(uint8_t *dibits, const uint8_t *in, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned shift = 6u - 2u * (unsigned)(i & 3u);
        dibits[i] = (uint8_t)((in[i / 4u] >> shift) & 3u);
    }
    return n;
}

size_t link_pack_symbols(uint8_t *out, const float *sym, size_t n)
{
    if (n > LINK_MAX_PAYLOAD - 1u) return 0;

    /* Choose k so the largest *plausible* magnitude maps to 127. Scaling on the
     * raw peak means one impulse in 192 symbols costs the whole block a bit or
     * two of resolution; bounding the estimate at 4x the mean lets the outlier
     * clip instead. The host applies cor from the sync burst, so absolute level
     * does not matter - only that the block does not clip wholesale. */
    float peak = 0.0f, sum = 0.0f;
    for (size_t i = 0; i < n; i++) {
        float a = fabsf(sym[i]);
        sum += a;
        if (a > peak) peak = a;
    }
    /* M17's levels are 3:1 (outer 1.0, inner 0.333), so a random payload has
     * mean |m| ~ 0.667 x outer -> outer ~ 1.5 x mean. Allow 50% for RRC
     * overshoot: bound = 2.25 x mean. Anything beyond that is an outlier worth
     * clipping rather than worth losing a bit of resolution over. */
    float bound = (n ? 2.25f * sum / (float)n : 0.0f);
    if (peak > bound) peak = bound;

    int k = 0;
    while (peak > 0.0f && peak * (float)(1 << (k + 1)) <= 127.0f && k < 30) k++;

    out[0] = (uint8_t)(int8_t)k;
    float scale = (float)(1 << k);
    for (size_t i = 0; i < n; i++) {
        long v = lrintf(sym[i] * scale);
        if (v > 127) v = 127;
        if (v < -127) v = -127;
        out[1 + i] = (uint8_t)(int8_t)v;
    }
    return n + 1u;
}

size_t link_unpack_symbols(float *sym, const uint8_t *payload, size_t plen)
{
    if (plen < 1u) return 0;
    int k = (int)(int8_t)payload[0];
    if (k < -30 || k > 30) return 0;
    float scale = (float)(1 << (k < 0 ? -k : k));
    if (k < 0) scale = 1.0f / scale;
    size_t n = plen - 1u;
    for (size_t i = 0; i < n; i++)
        sym[i] = (float)(int8_t)payload[1 + i] / scale;
    return n;
}

/* A false sync word in noise would otherwise be accepted on its length field and
 * stall the parser until that many bytes arrive. Known type codes only: an
 * unknown type is treated as a false sync and abandoned. */
static int link_type_known(uint8_t t)
{
    switch (t) {
    case LINK_TX_DIBITS: case LINK_TX_CONTROL: case LINK_PING: case LINK_TX_ABORT:
    case LINK_RX_SYMBOLS: case LINK_RX_SOFT: case LINK_RX_STATUS: case LINK_PONG:
        return 1;
    default:
        return 0;
    }
}

void link_parser_reset(link_parser_t *p)
{
    p->n = 0;
    p->last_seq = 0;
    p->have_seq = 0;
}

int link_parser_feed(link_parser_t *p, const uint8_t *in, size_t n,
                     link_frame_cb cb, void *ctx)
{
    int delivered = 0;

    while (n) {
        /* top up the buffer, never beyond one frame */
        size_t room = sizeof(p->buf) - p->n;
        size_t take = (n < room) ? n : room;
        memcpy(p->buf + p->n, in, take);
        p->n += take;
        in += take;
        n -= take;

        for (;;) {
            /* drop bytes until a sync word starts the buffer */
            size_t skip = 0;
            while (skip + 1 < p->n &&
                   !(p->buf[skip] == LINK_SYNC0 && p->buf[skip + 1] == LINK_SYNC1))
                skip++;
            if (skip) {
                memmove(p->buf, p->buf + skip, p->n - skip);
                p->n -= skip;
            }
            if (p->n < LINK_HDR_LEN) break;

            size_t len = (size_t)p->buf[4] | ((size_t)p->buf[5] << 8);
            if (len > LINK_MAX_PAYLOAD || !link_type_known(p->buf[2])) {
                /* nonsense length: resynchronise past this A5 */
                p->buf[0] = p->buf[1];          /* force a rescan */
                memmove(p->buf, p->buf + 1, p->n - 1);
                p->n--;
                continue;
            }
            size_t total = LINK_OVERHEAD + len;
            if (p->n < total) break;            /* need more bytes */

            uint16_t want = (uint16_t)(p->buf[LINK_HDR_LEN + len] |
                                       (p->buf[LINK_HDR_LEN + len + 1] << 8));
            uint16_t got = link_crc16(p->buf + 2, LINK_HDR_LEN - 2 + len);
            if (want != got) {
                memmove(p->buf, p->buf + 1, p->n - 1);   /* CRC: rescan */
                p->n--;
                continue;
            }

            uint8_t type = p->buf[2];
            uint8_t seq  = p->buf[3];
            p->last_seq = seq;
            p->have_seq = 1;
            if (cb) cb(ctx, type, seq, p->buf + LINK_HDR_LEN, len);
            delivered++;

            memmove(p->buf, p->buf + total, p->n - total);
            p->n -= total;
        }
    }
    return delivered;
}
