// m17_meta_text.cpp
//
// Text Data in the LSF META field.
//
// There is one 14-byte META field and the specification puts extended data in
// it by round robin (2.0.10, "Null Encryption Subtype Bits"): a message is cut
// into up to four blocks of 13 bytes, one block travels per superframe 240 ms
// apart, and the first byte of the block is a control byte.  Its high nibble is
// a bitmap of how many blocks the message has (0001, 0011, 0111, 1111) and its
// low nibble is a bitmap of which block this is (0001, 0010, 0100, 1000).
// A receiver ORs the control bytes together: the message is complete when the
// two halves agree.  A control byte of zero means "no text".
//
// This station uses it for one thing - a fixed text it announces while it has
// no position to announce.  A position and text cannot both be on air, because
// the subtype bits say which of them is in META, so the position wins whenever
// the fix is fresh (see m17_tx_meta_from_gps() in m17_tx_rx.cpp).
//
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "m17defines.h"     // uint48_t, which ui.h needs
#include "m17_meta_text.h"
#include "ui.h"

/* --------------------------------------------------------------- helpers */

static uint32_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static int popcount4(uint8_t v)
{
    int n = 0;
    for (int i = 0; i < 4; i++) if (v & (1u << i)) n++;
    return n;
}

/* -------------------------------------------------------------- transmit */

#define TX_LOCK()   pthread_mutex_lock(&m_tx_lock)
#define TX_UNLOCK() pthread_mutex_unlock(&m_tx_lock)

static pthread_mutex_t m_tx_lock = PTHREAD_MUTEX_INITIALIZER;
static char     m_text[META_TEXT_MAX + 1] = "";
static int      m_block;                    /* next block to send */

bool meta_text_set(const char *text)
{
    char  buf[META_TEXT_MAX + 1];
    int   n = 0;

    if (text == NULL) text = "";
    while (*text == ' ' || *text == '\t') text++;      /* a config line ends up
                                                        * here with its spaces */
    while (text[n] && n < META_TEXT_MAX) {
        char c = text[n];
        buf[n++] = (c == '\r' || c == '\n' || c == '\t') ? ' ' : c;
    }
    buf[n] = 0;
    while (n > 0 && buf[n - 1] == ' ') buf[--n] = 0;
    /* Do not split a UTF-8 character across the 52-byte limit: drop trailing
     * continuation bytes (0b10xxxxxx) that lost their lead byte. */
    if (text[n] != 0) {
        while (n > 0 && ((unsigned char)buf[n - 1] & 0xC0) == 0x80) buf[--n] = 0;
    }

    TX_LOCK();
    snprintf(m_text, sizeof m_text, "%s", buf);
    m_block = 0;
    TX_UNLOCK();
    return n > 0;
}

void meta_text_clear(void)
{
    TX_LOCK();
    m_text[0] = 0;
    m_block   = 0;
    TX_UNLOCK();
}

const char *meta_text_get(void)
{
    return m_text;
}

bool meta_text_fill(uint8_t *meta)
{
    int len, nblk, blk;
    uint8_t hi, lo;

    if (meta == NULL) return false;

    TX_LOCK();
    len = (int)strlen(m_text);
    if (len == 0) {
        m_block = 0;
        TX_UNLOCK();
        return false;
    }
    nblk = (len + META_TEXT_BLOCK - 1) / META_TEXT_BLOCK;
    if (nblk < 1) nblk = 1;
    if (nblk > META_TEXT_BLOCKS) nblk = META_TEXT_BLOCKS;
    if (m_block >= nblk) m_block = 0;
    blk = m_block;
    m_block = (m_block + 1) % nblk;

    hi = (uint8_t)((1u << nblk) - 1u);          /* 0001 .. 1111 */
    lo = (uint8_t)(1u << blk);                  /* 0001,0010,0100,1000 */

    memset(meta, ' ', META_TEXT_BLOCK + 1);     /* unused space is spaces */
    meta[0] = (uint8_t)((hi << 4) | lo);
    for (int i = 0; i < META_TEXT_BLOCK; i++) {
        int k = blk * META_TEXT_BLOCK + i;
        if (k < len) meta[1 + i] = (uint8_t)m_text[k];
    }
    TX_UNLOCK();
    return true;
}

/* --------------------------------------------------------------- receive */

/* One accumulator per transmitting station: blocks of two stations heard at
 * once would otherwise be spliced into one message.  Four is plenty on one
 * channel and keeps this a table rather than a data structure. */
#define RX_SLOTS 4

typedef struct {
    char     src[16];
    uint8_t  blk[META_TEXT_BLOCKS][META_TEXT_BLOCK];
    uint8_t  have;                  /* blocks arrived   (bitmap) */
    uint8_t  want;                  /* blocks expected  (bitmap) */
    uint32_t ms;                    /* last block from this station */
} RxSlot;

static RxSlot          m_rx[RX_SLOTS];
static pthread_mutex_t m_rx_lock = PTHREAD_MUTEX_INITIALIZER;

static char     m_last_src[16];
static char     m_last_text[META_TEXT_MAX + 1];
static uint32_t m_last_ms;
static char     m_logged_src[16];
static char     m_logged_text[META_TEXT_MAX + 1];
static uint32_t m_logged_ms;

bool meta_text_last(char *src, int src_len, char *text, int text_len,
                    uint32_t *age_ms)
{
    bool any;

    pthread_mutex_lock(&m_rx_lock);
    any = (m_last_ms != 0);
    if (any) {
        if (src && src_len > 0)  snprintf(src, (size_t)src_len, "%s", m_last_src);
        if (text && text_len > 0) snprintf(text, (size_t)text_len, "%s", m_last_text);
        if (age_ms) *age_ms = now_ms() - m_last_ms;
    }
    pthread_mutex_unlock(&m_rx_lock);
    return any;
}

static RxSlot *slot_for(const char *src, uint32_t ms)
{
    RxSlot *free_slot = NULL;
    RxSlot *oldest    = &m_rx[0];

    for (int i = 0; i < RX_SLOTS; i++) {
        if (m_rx[i].src[0] && strcmp(m_rx[i].src, src) == 0) return &m_rx[i];
        if (!m_rx[i].src[0] && free_slot == NULL) free_slot = &m_rx[i];
        if (m_rx[i].ms < oldest->ms) oldest = &m_rx[i];
    }
    RxSlot *s = free_slot ? free_slot : oldest;
    memset(s, 0, sizeof *s);
    snprintf(s->src, sizeof s->src, "%s", src);
    s->ms = ms;
    return s;
}

void meta_text_rx(const uint8_t *meta, const char *src)
{
    uint8_t ctl, hi, lo;
    int     idx, nblk, len;
    uint32_t ms = now_ms();
    RxSlot *s;
    char     text[META_TEXT_MAX + 1];

    if (meta == NULL || src == NULL) return;
    ctl = meta[0];
    if (ctl == 0) return;                       /* "no text" - the normal case */
    hi = (uint8_t)(ctl >> 4);
    lo = (uint8_t)(ctl & 0x0F);
    if (hi != 0x1 && hi != 0x3 && hi != 0x7 && hi != 0xF) return;
    if (lo != 0x1 && lo != 0x2 && lo != 0x4 && lo != 0x8) return;
    for (idx = 0; idx < META_TEXT_BLOCKS; idx++) if (lo == (1u << idx)) break;
    nblk = popcount4(hi);
    if (idx < 0 || idx >= nblk) return;         /* not a block of this message */

    pthread_mutex_lock(&m_rx_lock);

    s = slot_for(src, ms);
    if ((s->want & hi) == 0) {                  /* a different message started */
        s->have = 0;
        s->want = 0;
    }
    memcpy(s->blk[idx], &meta[1], META_TEXT_BLOCK);
    s->have |= lo;
    s->want |= hi;
    s->ms    = ms;

    if ((s->have & s->want) != s->want) {       /* not complete yet */
        pthread_mutex_unlock(&m_rx_lock);
        return;
    }

    len = 0;
    for (int b = 0; b < nblk && len < META_TEXT_MAX; b++) {
        for (int i = 0; i < META_TEXT_BLOCK && len < META_TEXT_MAX; i++) {
            char c = (char)s->blk[b][i];
            if (c == 0) break;                  /* tolerate null padding too */
            text[len++] = c;
        }
    }
    text[len] = 0;
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\r' ||
                       text[len - 1] == '\n' || text[len - 1] == '\t'))
        text[--len] = 0;

    if (len > 0) {
        snprintf(m_last_src, sizeof m_last_src, "%s", s->src);
        snprintf(m_last_text, sizeof m_last_text, "%s", text);
        m_last_ms = ms;
    }

    /* The same message is repeated for as long as the other station transmits,
     * so only a message that is new - or one heard again after half a minute -
     * is worth a line in the console. */
    bool log_it = len > 0 &&
                  (strcmp(m_logged_src, s->src) != 0 ||
                   strcmp(m_logged_text, text) != 0 ||
                   ms - m_logged_ms > 30000);
    if (log_it) {
        snprintf(m_logged_src, sizeof m_logged_src, "%s", s->src);
        snprintf(m_logged_text, sizeof m_logged_text, "%s", text);
        m_logged_ms = ms;
    }
    pthread_mutex_unlock(&m_rx_lock);

    if (log_it) {
        printf("META text from %s: %s\n", src, text);
        fflush(stdout);
        gui_log_event("rx", "RX  META  [%s] %s", src, text);
    }
}
