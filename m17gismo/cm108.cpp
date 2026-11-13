//
// cm108.cpp
//
// The CM108 back end: the Feather's two DSP chains, a sound fob, and the
// keying discipline the fob needs because its PTT is not part of its audio.
//
// Everything about the modulation is already written and proven - the chains
// are the vendored copies of fw/tx_chain.* and fw/rx_chain.* from m17_feather,
// the same ones -r feather-loop uses.  What is new here is plumbing:
//
//   4800 symbols/s x 10 samples = 48000 frames/s, which is the rate the fob
//   runs at, so a dibit is ten frames and a 192-dibit frame is 1920.
//
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m17defines.h"
#include "ui.h"
#include "cm108.h"
#include "cm108_io.h"
#include "feather_tx_chain.h"
#include "feather_rx_chain.h"

#define RATE            48000
#define SAMPLES_PER_SYM 10
#define BLOCK           480         // 10 ms, the period the fob is happiest with
#define MAX_BLOCK       4096
#define MAX_SYM         512

static m17f::TxChain g_txc;
static m17f::RxChain g_rxc;

static cm108_pcm *g_out;
static cm108_pcm *g_in;
static void      *g_hid;

static bool g_ready;
static bool g_keyed;
static int  g_pin = CM108_GPIO_PIN;

// Transmit staging: samples accumulate here until a whole 10 ms block is ready.
static float g_play[BLOCK];
static int   g_play_n;

// Receive working buffers.
static float g_cap[MAX_BLOCK];
static float g_sym[MAX_SYM];

static unsigned g_prekey_ms = 40;
static unsigned g_tail_ms   = 40;

static uint32_t g_syms;
static uint32_t g_dibits;
static uint32_t g_blocks;
static char     g_status[160];

// ------------------------------------------------------------------ transmit
static void write_block(const float *s, int n)
{
    if (!g_out || n <= 0) return;
    if (cm108_pcm_write(g_out, s, n) < 0) {
        // The stream stopped under us: counted in the IO layer, nothing to do
        // here except keep going, which is what a burst of symbols wants.
    }
    g_blocks++;
}

static void put_sample(float s)
{
    g_play[g_play_n++] = s;
    if (g_play_n >= BLOCK) {
        write_block(g_play, g_play_n);
        g_play_n = 0;
    }
}

static void flush_partial(void)
{
    if (g_play_n > 0) {
        write_block(g_play, g_play_n);
        g_play_n = 0;
    }
}

static void put_silence(unsigned ms)
{
    unsigned long left = (unsigned long)(RATE / 1000) * ms;
    while (left--) put_sample(0.0f);
}

void cm108_write_samples(const float *s, int n)
{
    if (!g_ready || !s || n <= 0) return;
    for (int i = 0; i < n; i++) put_sample(s[i]);
}

void cm108_tx_dibits(const uint8_t *dibits, int n)
{
    if (!g_ready || !dibits || n <= 0) return;

    for (int i = 0; i < n; i++) {
        g_txc.push(dibits[i]);
        for (int k = 0; k < SAMPLES_PER_SYM; k++) put_sample(g_txc.next());
    }
    g_dibits += (uint32_t)n;
}

// -------------------------------------------------------------------- keying
void cm108_key(bool on)
{
    if (!g_ready) return;

    if (on) {
        if (g_keyed) return;
        // The PTT edge has to arrive while the stream is running, or the fob is
        // starting a DMA transfer at the same moment the radio is keying up.
        // So: 10 ms of silence into the buffer, the edge, then the rest of the
        // pre-key padding on the air, which is time the receiver spends
        // settling its AGC before the preamble arrives.
        put_silence(10);
        flush_partial();
        if (g_hid) cm108_hid_gpio(g_hid, g_pin, 1);
        g_keyed = true;
        if (g_prekey_ms > 10) put_silence(g_prekey_ms - 10);
    } else {
        if (!g_keyed) return;
        // The modulator's filter holds 31 symbols of history; letting those go
        // before the carrier drops is what keeps the last symbol's tail off the
        // air as a click.
        while (g_txc.busy()) put_sample(g_txc.next());
        flush_partial();
        put_silence(g_tail_ms);
        flush_partial();
        if (g_hid) cm108_hid_gpio(g_hid, g_pin, 0);
        g_keyed = false;
        // Capture has been filling (or not) all the way through the
        // transmission: start the receiver's chain from a clean stream.
        if (g_in) cm108_pcm_drop(g_in);
        g_rxc.reset();
    }
}

bool cm108_keyed(void) { return g_keyed; }

// ------------------------------------------------------------------- receive
void cm108_poll(int n)
{
    if (!g_ready || !g_in) return;

    if (n <= 0 || n > MAX_BLOCK) n = BLOCK;

    // A receive loop that stopped reading (because it was transmitting, or the
    // host stalled) leaves the capture stream holding stale audio.  Drop it
    // rather than letting the timing loop chase something minutes old.
    if (cm108_pcm_pending(g_in) > 4 * MAX_BLOCK) cm108_pcm_drop(g_in);

    int got = cm108_pcm_read(g_in, g_cap, n);
    if (got <= 0) return;

    int s = g_rxc.process(g_cap, got, g_sym, MAX_SYM);
    if (s > 0) {
        g_syms += (uint32_t)s;
        m17_rx_symbols(g_sym, s);
    }
}

// ---------------------------------------------------------------- open/close
bool cm108_open(const char *pcm_dev, const char *hid_dev)
{
    char err[128];

    if (g_ready) return true;

    if (pcm_dev == NULL) pcm_dev = getenv("CM108_PCM");
    if (pcm_dev == NULL || *pcm_dev == '\0') pcm_dev = CM108_DEFAULT_PCM;

    if (getenv("CM108_PREKEY_MS")) g_prekey_ms = (unsigned)atoi(getenv("CM108_PREKEY_MS"));
    if (getenv("CM108_TAIL_MS"))   g_tail_ms   = (unsigned)atoi(getenv("CM108_TAIL_MS"));
    if (getenv("CM108_GPIO"))      g_pin       = atoi(getenv("CM108_GPIO"));
    if (g_pin < 1 || g_pin > 8)    g_pin       = CM108_GPIO_PIN;

    err[0] = '\0';
    g_out = cm108_pcm_open(pcm_dev, RATE, 0, err, (int)sizeof(err));
    if (g_out == NULL) {
        snprintf(g_status, sizeof(g_status),
                 "cm108: cannot open %s for playback (%s) - aplay -l, then set CM108_PCM",
                 pcm_dev, err[0] ? err : "no reason given");
        gui_log_event("cm108", "%s", g_status);
        return false;
    }

    // One device normally serves both directions.  CM108_PCM_IN is for the cases
    // where it does not: snd-aloop, or the fob's output with a separate ADC.
    const char *in_dev = getenv("CM108_PCM_IN");
    if (in_dev == NULL || *in_dev == '\0') in_dev = pcm_dev;

    g_in = cm108_pcm_open(in_dev, RATE, 1, err, (int)sizeof(err));
    if (g_in == NULL) {
        snprintf(g_status, sizeof(g_status),
                 "cm108: %s plays but %s will not capture (%s) - is the capture side busy?",
                 pcm_dev, in_dev, err[0] ? err : "no reason given");
        gui_log_event("cm108", "%s", g_status);
        cm108_pcm_close(g_out);
        g_out = NULL;
        return false;
    }

    // PTT is optional in the sense that the audio still works without it, but a
    // fob whose GPIO cannot be reached cannot key anything, so say so loudly.
    g_hid = cm108_hid_open(hid_dev, err, (int)sizeof(err));
    if (g_hid == NULL) {
        gui_log_event("cm108", "cm108: no GPIO for PTT (%s) - is the hidraw node "
                               "readable by this user?", err[0] ? err : "no C-Media device");
    }

    g_txc.init();
    g_rxc.init();
    g_play_n = 0;
    g_syms = g_dibits = g_blocks = 0;
    g_ready = true;

    gui_log_event("cm108", "cm108: %s, PTT gpio%d on %s, %u/%u ms pre-key/tail",
                  cm108_pcm_name(g_out), g_pin,
                  g_hid ? cm108_hid_name(g_hid) : "no HID", g_prekey_ms, g_tail_ms);
    return true;
}

void cm108_close(void)
{
    if (!g_ready) return;
    if (g_keyed) cm108_key(false);
    if (g_hid) { cm108_hid_close(g_hid); g_hid = NULL; }
    if (g_in)  { cm108_pcm_close(g_in);  g_in  = NULL; }
    if (g_out) { cm108_pcm_close(g_out); g_out = NULL; }
    g_ready = false;
}

bool cm108_open_ok(void) { return g_ready; }

const char *cm108_status(void)
{
    if (!g_ready) return g_status;      // the reason it would not open

    snprintf(g_status, sizeof(g_status),
             "cm108: %s  sym %u  dibits %u  blocks %u  u/r %u/%u  %s%s",
             cm108_pcm_name(g_out),
             (unsigned)g_syms, (unsigned)g_dibits, (unsigned)g_blocks,
             cm108_pcm_underruns(), cm108_pcm_overruns(),
             g_keyed ? "PTT " : "",
             g_hid ? "gpio ok" : "no gpio");
    return g_status;
}

uint32_t cm108_symbols_rx(void) { return g_syms; }
uint32_t cm108_dibits_tx(void)  { return g_dibits; }
