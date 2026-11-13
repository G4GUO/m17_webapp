//
// cm108_test.cpp
//
// The CM108 back end with no sound card and no fob: a fake PCM whose playback
// ring is the capture ring (so the modulator's output really does arrive at the
// receiver) and a fake hidraw node that records the vendor reports.
//
// What this proves, and why each one is worth a check:
//
//   - the keying order.  PTT is a HID report and the audio is a separate
//     isochronous stream, so the only thing that makes keying safe is doing it
//     in the right order with silence around it.  A burst that starts before
//     the fob is streaming is a burst the far end never hears.
//   - ten frames per symbol.  4800 symbols/s at 48 kHz, no exceptions, or the
//     receiver's timing loop is chasing a signal that is not at the rate it
//     thinks it is.
//   - the symbols come back.  Dibits in, symbols out of the receive chain at the
//     right rate, and the hard decisions match what went in.
//   - the tail goes out before the carrier drops, and PTT is released after.
//
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cm108.h"
#include "cm108_io.h"

// ------------------------------------------------------------------- stubs
static float g_sym[65536];
static int   g_sym_n;

void m17_rx_symbols(float *sym, int len)
{
    for (int i = 0; i < len && g_sym_n < (int)(sizeof(g_sym) / sizeof(g_sym[0])); i++)
        g_sym[g_sym_n++] = sym[i];
}
bool  m17_rx_lock(void) { return false; }
void  radio_afc(float mean) { (void)mean; }
float radio_get_afc_delta(void) { return 0.0f; }
bool  radio_get_afc_status(void) { return false; }

void gui_log_event(const char *kind, const char *fmt, ...)
{
    va_list ap;
    (void)kind;
    printf("  log: ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

// -------------------------------------------------------------- fake PCM
// One ring, two directions: what the modulator writes, the receiver reads.  The
// hardware does the same thing through a cable and the analogue board.
#define RING 262144
static float  g_ring[RING];
static int    g_ring_r, g_ring_w, g_ring_n;
static unsigned g_frames_written, g_frames_read, g_fake_underruns;

struct cm108_pcm { int capture; char name[32]; };

cm108_pcm *cm108_pcm_open(const char *dev, unsigned rate, int capture,
                          char *err, int errlen)
{
    if (rate != 48000) { snprintf(err, (size_t)errlen, "rate %u", rate); return NULL; }
    if (strcmp(dev, "fake") != 0) { snprintf(err, (size_t)errlen, "no such device"); return NULL; }
    cm108_pcm *p = (cm108_pcm *)calloc(1, sizeof(*p));
    p->capture = capture;
    snprintf(p->name, sizeof(p->name), "%s %s", dev, capture ? "in" : "out");
    return p;
}
void cm108_pcm_close(cm108_pcm *p) { free(p); }

int cm108_pcm_write(cm108_pcm *p, const float *mono, int frames)
{
    (void)p;
    int done = 0;
    while (done < frames && g_ring_n < RING) {
        g_ring[g_ring_w] = mono[done++];
        g_ring_w = (g_ring_w + 1) % RING;
        g_ring_n++;
        g_frames_written++;
    }
    if (done < frames) g_fake_underruns++;
    return done;
}

int cm108_pcm_read(cm108_pcm *p, float *mono, int frames)
{
    (void)p;
    int done = 0;
    while (done < frames && g_ring_n > 0) {
        mono[done++] = g_ring[g_ring_r];
        g_ring_r = (g_ring_r + 1) % RING;
        g_ring_n--;
        g_frames_read++;
    }
    if (done < frames) g_fake_underruns++;      // an underrun in the fake
    return done;
}

int  cm108_pcm_pending(cm108_pcm *p) { (void)p; return g_ring_n; }
void cm108_pcm_drop(cm108_pcm *p)    { (void)p; g_ring_r = g_ring_w = g_ring_n = 0; }
const char *cm108_pcm_name(cm108_pcm *p) { return p->name; }
unsigned cm108_pcm_underruns(void) { return g_fake_underruns; }
unsigned cm108_pcm_overruns(void)  { return 0; }

// -------------------------------------------------------------- fake HID
struct cm108_hid_rep { unsigned char io[5]; unsigned frames_at_write; };
static cm108_hid_rep g_rep[64];
static unsigned      g_rep_n;

struct cm108_hid { int fd; char name[32]; };

void *cm108_hid_open(const char *path, char *err, int errlen)
{
    (void)path;
    if (err && errlen) err[0] = '\0';
    cm108_hid *h = (cm108_hid *)calloc(1, sizeof(*h));
    h->fd = 42;
    snprintf(h->name, sizeof(h->name), "fake-hidraw gpio");
    return h;
}
void cm108_hid_close(void *pv) { free(pv); }

int cm108_hid_gpio(void *pv, int pin, int on)
{
    (void)pv;
    if (g_rep_n < sizeof(g_rep) / sizeof(g_rep[0])) {
        unsigned char *io = g_rep[g_rep_n].io;
        io[0] = 0; io[1] = 0;
        io[2] = (unsigned char)(on ? (1 << (pin - 1)) : 0);
        io[3] = (unsigned char)(1 << (pin - 1));
        io[4] = 0;
        g_rep[g_rep_n].frames_at_write = g_frames_written;
        g_rep_n++;
    }
    return 0;
}
const char *cm108_hid_name(void *pv) { (void)pv; return "fake-hidraw gpio"; }
unsigned    cm108_hid_writes(void)   { return g_rep_n; }

// ------------------------------------------------------------------ checks
static int g_checks, g_fails;
#define CHECK(cond, ...)                                                \
    do {                                                                \
        g_checks++;                                                     \
        if (!(cond)) {                                                  \
            g_fails++;                                                  \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                 \
            printf(__VA_ARGS__);                                        \
            printf("\n");                                               \
        } else {                                                        \
            printf("ok   ");                                            \
            printf(__VA_ARGS__);                                        \
            printf("\n");                                               \
        }                                                               \
    } while (0)

// TX symbol map, from fw/tx_chain.cpp: dibit -> amplitude.
static const float kSym[4] = { 1.0f / 3.0f, 1.0f, -1.0f / 3.0f, -1.0f };

static int nearest_dibit(float s, float scale)
{
    int   best = 0;
    float bestd = 1e30f;
    for (int i = 0; i < 4; i++) {
        float d = fabsf(s - kSym[i] * scale);
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}

int main(void)
{
    printf("cm108: the sound-fob back end, no sound card\n");

    CHECK(!cm108_open("nosuch", NULL), "open fails cleanly on a device that is not there");
    CHECK(!cm108_open_ok(), "a failed open leaves the back end closed");

    CHECK(cm108_open("fake", NULL), "open: %s", cm108_status());
    CHECK(cm108_open_ok(), "back end reports itself open");

    // ---- keying order -----------------------------------------------------
    unsigned before = g_rep_n;
    cm108_key(true);
    CHECK(g_rep_n == before + 1, "key: exactly one PTT report");
    CHECK(g_rep[before].io[2] == 0x04 && g_rep[before].io[3] == 0x04,
          "key: report drives GPIO3 (data 0x%02x mask 0x%02x)",
          g_rep[before].io[2], g_rep[before].io[3]);
    CHECK(g_rep[before].frames_at_write >= 480,
          "key: %u frames of audio already in the stream when PTT moved (>=480)",
          g_rep[before].frames_at_write);

    int quiet = 1;
    for (unsigned i = 0; i < g_rep[before].frames_at_write && i < (unsigned)RING; i++)
        if (g_ring[i] != 0.0f) { quiet = 0; break; }
    CHECK(quiet, "key: that padding is silence, not the tail of the last burst");

    // ---- ten frames per symbol -------------------------------------------
    uint8_t dibits[192];
    srand(1234);
    for (int i = 0; i < 192; i++) dibits[i] = (uint8_t)(rand() & 3);

    unsigned w0 = g_frames_written;
    cm108_tx_dibits(dibits, 192);
    unsigned tx_frames = g_frames_written - w0;
    CHECK(tx_frames == 192 * 10, "tx: 192 dibits made %u frames (want 1920)", tx_frames);
    CHECK(cm108_dibits_tx() == 192, "tx: dibit counter %u", cm108_dibits_tx());

    // ---- what the modulator put out --------------------------------------
    float peak = 0.0f, dmax = 0.0f, prev = 0.0f;
    for (unsigned i = 0; i < RING; i++) {
        float v = g_ring[i];
        if (fabsf(v) > peak) peak = fabsf(v);
        if (i) { float d = fabsf(v - prev); if (d > dmax) dmax = d; }
        prev = v;
    }
    CHECK(peak > 0.9f && peak < 1.6f, "tx: peak %.3f is a full-deviation symbol, not a fragment", peak);
    CHECK(dmax < 0.6f, "tx: largest sample step %.3f - the RRC output is band-limited", dmax);

    // ---- receive the same stream back ------------------------------------
    // A 40 ms pre-key pad plus 40 ms of symbols: ask for all of it, twice, so
    // the timing loop has the whole burst to settle on.
    cm108_poll(1920);
    cm108_poll(1920);

    CHECK(g_sym_n > 150, "rx: %d symbols out of the chain", g_sym_n);

    // Find where the burst starts in the symbol stream and take the best lag,
    // because the chain's output does not begin at the first transmitted symbol.
    int start = -1;
    for (int i = 0; i < g_sym_n; i++)
        if (fabsf(g_sym[i]) > 0.25f) { start = i; break; }
    CHECK(start >= 0, "rx: burst found at symbol %d", start);

    // Scale from the received symbols themselves: the matched filter's output
    // amplitude is not the modulator's input amplitude.
    float mag = 0.0f;
    int   cnt = 0;
    for (int i = start; i < g_sym_n; i++) { mag += fabsf(g_sym[i]); cnt++; }
    float scale = cnt > 0 ? (mag / cnt) / 0.6667f : 1.0f;   // mean |level| = 2/3

    // 150 of the 192: the last 31 symbols of the burst are still inside the
    // modulator's filter when this runs, and only leave it on unkey.
    const int NCMP = 150;
    int best_lag = 0, best_hit = 0, best_cmp = 0;
    for (int lag = -10; lag < 40; lag++) {
        int hit = 0, cmp = 0;
        for (int i = 0; i < NCMP; i++) {
            int idx = start + i + lag;
            if (idx < 0 || idx >= g_sym_n) continue;
            if (fabsf(g_sym[idx]) < 0.1f) continue;
            if (nearest_dibit(g_sym[idx], scale) == dibits[i]) hit++;
            cmp++;
        }
        // best_cmp == 0 for the first scored lag, or 0 * hit == 0 * hit and
        // nothing is ever chosen.
        if (cmp > 0 && (best_cmp == 0 || hit * best_cmp > best_hit * cmp)) {
            best_hit = hit; best_cmp = cmp; best_lag = lag;
        }
    }
    printf("     rx: best lag %d, %d/%d symbols agree (%.0f%%)\n",
           best_lag, best_hit, best_cmp, best_cmp ? 100.0 * best_hit / best_cmp : 0.0);
    CHECK(best_cmp > 120 && best_hit * 100 >= best_cmp * 90,
          "rx: hard decisions match the transmitted dibits");

    // ---- unkey -----------------------------------------------------------
    unsigned w1 = g_frames_written;
    unsigned rep1 = g_rep_n;
    cm108_key(false);
    unsigned tail = g_frames_written - w1;
    CHECK(g_rep_n == rep1 + 1 && g_rep[rep1].io[2] == 0x00,
          "unkey: one report, GPIO3 driven low");
    CHECK(tail >= 1920, "unkey: %u frames after the last symbol (tail + padding)", tail);
    CHECK(!cm108_keyed(), "unkey: back end reports it is not keyed");

    // ---- status and close ------------------------------------------------
    printf("     %s\n", cm108_status());
    cm108_close();
    CHECK(!cm108_open_ok(), "close: back end closed");

    printf("%d checks, %d failed\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
