/* link.h - host <-> Feather wire format for m17_feather.
 *
 * Freestanding C99: no malloc, no libc beyond string.h, no floating point
 * required on the encode side. This file is compiled into the firmware and into
 * the host tools from the same source, so the two ends cannot drift apart.
 *
 * See docs/LINK.md for the rationale. Frame on the wire:
 *
 *   A5 5A | type | seq | len_lo len_hi | payload[len] | crc_lo crc_hi
 *
 * CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over type..payload inclusive.
 * Multi-byte fields little-endian.
 */
#ifndef M17F_LINK_H
#define M17F_LINK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LINK_SYNC0          0xA5u
#define LINK_SYNC1          0x5Au
#define LINK_HDR_LEN        6u          /* A5 5A type seq len_lo len_hi */
#define LINK_CRC_LEN        2u
#define LINK_OVERHEAD       (LINK_HDR_LEN + LINK_CRC_LEN)
#define LINK_MAX_PAYLOAD    1024u
#define LINK_MAX_FRAME      (LINK_MAX_PAYLOAD + LINK_OVERHEAD)

/* one M17 frame: 192 symbols, 4 dibits per byte, 2 soft values per symbol */
#define LINK_SYMBOLS_PER_FRAME  192u
#define LINK_DIBIT_BYTES        (LINK_SYMBOLS_PER_FRAME / 4u)   /* 48  */
#define LINK_SOFT_BYTES         (LINK_SYMBOLS_PER_FRAME * 2u)   /* 384 */

typedef enum {
    LINK_TX_DIBITS  = 0x01,   /* host -> feather: packed dibits            */
    LINK_TX_CONTROL = 0x02,   /* host -> feather: PTT, mode, lock hint     */
    LINK_PING       = 0x03,   /* host -> feather: 4-byte timestamp         */
    LINK_TX_ABORT   = 0x04,   /* host -> feather: unkey at the frame edge  */

    LINK_RX_SYMBOLS = 0x81,   /* feather -> host: int8 symbols + scale     */
    LINK_RX_SOFT    = 0x82,   /* feather -> host: 2 x int8 soft per symbol */
    LINK_RX_STATUS  = 0x83,   /* feather -> host: DCD, timing, counters    */
    LINK_PONG       = 0x84,   /* feather -> host: timestamp echo + build   */
} link_type_t;

/* LINK_TX_CONTROL flags */
#define LINK_CTL_PTT        (1u << 0)
#define LINK_CTL_TXEN       (1u << 1)
#define LINK_CTL_LOOPBACK   (1u << 2)
#define LINK_CTL_RX_LOCK    (1u << 3)   /* host is in frame sync: slow the loop */
#define LINK_CTL_SOFT_4BIT  (1u << 4)   /* request RX_SOFT at 4 bits packed     */
#define LINK_CTL_TESTTONE   (1u << 6)

/* LINK_RX_STATUS flags */
#define LINK_ST_DCD_ENERGY  (1u << 0)   /* carrier present (HF energy test)     */
#define LINK_ST_TX_ACTIVE   (1u << 1)   /* radio is keyed                        */
#define LINK_ST_UNDERRUN    (1u << 2)   /* TX starved, mid-scale emitted         */
#define LINK_ST_OVERRUN     (1u << 3)   /* RX DMA/USB lost samples               */

uint16_t link_crc16(const uint8_t *p, size_t n);

/* Encode one frame into out[cap]. Returns total bytes written, 0 if it will not
 * fit or len exceeds LINK_MAX_PAYLOAD. */
size_t link_encode(uint8_t *out, size_t cap, uint8_t type, uint8_t seq,
                   const uint8_t *payload, size_t len);

/* M17 dibit packing: 4 dibits per byte, most significant dibit sent first.
 * M17_spec.tex: the byte 0xB4 holds dibits 10 11 01 00 and is sent as the
 * symbols (-1, -3, +3, +1). out must hold (n+3)/4 bytes. Returns bytes written. */
size_t link_pack_dibits(uint8_t *out, const uint8_t *dibits, size_t n);

/* Inverse. Reads (n+3)/4 bytes, writes n dibits. */
size_t link_unpack_dibits(uint8_t *dibits, const uint8_t *in, size_t n);

/* Equalised symbols as int8 with a per-block power-of-two scale, so the block
 * cannot clip: value = round(sym * 2^k), payload = { int8 k, int8 values... }.
 * out must hold n + 1 bytes. Returns payload length. */
size_t link_pack_symbols(uint8_t *out, const float *sym, size_t n);

/* Inverse. Returns symbols decoded, 0 on a malformed payload. */
size_t link_unpack_symbols(float *sym, const uint8_t *payload, size_t plen);

/* Streaming parser: tolerates arbitrary garbage before a sync word, and
 * rejects any frame whose CRC fails. */
typedef struct {
    uint8_t buf[LINK_MAX_FRAME];
    size_t  n;
    uint8_t last_seq;
    int     have_seq;
} link_parser_t;

typedef void (*link_frame_cb)(void *ctx, uint8_t type, uint8_t seq,
                              const uint8_t *payload, size_t len);

void link_parser_reset(link_parser_t *p);

/* Feed n bytes; cb is called once per valid frame. Returns frames delivered.
 * A frame with a bad CRC is dropped, and the parser resynchronises on the next
 * sync word rather than discarding the following frame. */
int link_parser_feed(link_parser_t *p, const uint8_t *in, size_t n,
                     link_frame_cb cb, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* M17F_LINK_H */
