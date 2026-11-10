// m17_meta_text.h
//
// Text Data in the LSF META field (specification 2.0.10, appendix-free section
// "Null Encryption Subtype Bits" / "Text Data").  One 14-byte block per
// superframe, four blocks to a message, 52 bytes of UTF-8 in all.
//
#ifndef M17_META_TEXT_H
#define M17_META_TEXT_H

#include <stdint.h>
#include <stdbool.h>

#define META_TEXT_BLOCK   13
#define META_TEXT_BLOCKS  4
#define META_TEXT_MAX     (META_TEXT_BLOCKS * META_TEXT_BLOCK)   /* 52 bytes */

/* Transmit side: the text announced whenever there is no position to send. */
bool        meta_text_set(const char *text);   /* trims to 52 bytes; "" clears */
void        meta_text_clear(void);
const char *meta_text_get(void);               /* "" when nothing is set */
/* Fill one 14-byte META field with the next block and advance the round robin.
 * Returns false, leaving the buffer untouched, when no text is configured. */
bool        meta_text_fill(uint8_t *meta14);

/* Receive side: one block from one station. */
void        meta_text_rx(const uint8_t *meta14, const char *src);
/* The last complete message heard.  False only if nothing has ever been
 * decoded; the age is reported separately so the page can mark it stale. */
bool        meta_text_last(char *src, int src_len, char *text, int text_len,
                           uint32_t *age_ms);

#endif
