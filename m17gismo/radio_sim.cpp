// radio_sim.cpp
//
// Simulated SDR.  Lets the whole program (DSP, M17 framing, reflectors, web UI
// and the audio path) be built and exercised without a Pluto or LimeSDR:
//
//   * transmit samples are pushed into a ring buffer
//   * the receive path decimates that ring by 8 exactly like the Pluto path,
//     adds optional noise and hands the result to the demodulator
//   * receive calls are paced to real time (40 ms per 1920 sample block) so
//     throttling, AFC and PTT timing behave like the real thing
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

#include "m17defines.h"
#include "radio_sim.h"

#define SIM_TXBUF   32768          // complex samples of loopback buffer
#define SIM_NFIL    31
#define RX_RATE     48000.0        // baseband sample rate of the decoder

static scmplx m_ring[SIM_TXBUF];
static int    m_head, m_tail, m_count;
static int16_t m_dec[SIM_NFIL];
static float  m_have_filter;
static double m_noise = 0.002;
static bool   m_loopback = true;
static bool   m_realtime = true;
static bool   m_keyed;
static double m_level = 0.0;
static double m_tx_gain = 1.0, m_rx_gain = 0.7;
static unsigned int m_rng = 0x12345678;

static double rand_uniform(void) {
    // xorshift: cheap, avoids rand()/rand_r portability noise
    m_rng ^= m_rng << 13; m_rng ^= m_rng >> 17; m_rng ^= m_rng << 5;
    return ((double)(m_rng & 0xFFFFFF) / 8388608.0) - 1.0;
}

static scmplx sub_filter(scmplx *in, int16_t *coffs) {
    int32_t real, imag;
    scmplx sum;
    real = in[SIM_NFIL/2].re * coffs[SIM_NFIL/2];
    imag = in[SIM_NFIL/2].im * coffs[SIM_NFIL/2];
    for (int i = 0; i < SIM_NFIL/2; i++) {
        real += coffs[i] * (in[i].re + in[(SIM_NFIL-1)-i].re);
        imag += coffs[i] * (in[i].im + in[(SIM_NFIL-1)-i].im);
    }
    sum.re = real >> 15;
    sum.im = imag >> 15;
    return sum;
}

// Filter history: oldest sample first
static scmplx m_hist[SIM_NFIL];

static void hist_push(scmplx v) {
    memmove(&m_hist[0], &m_hist[1], sizeof(scmplx) * (SIM_NFIL - 1));
    m_hist[SIM_NFIL-1] = v;
}

void sim_open(void) {
    m_head = m_tail = m_count = 0;
    m_have_filter = 0;
    m_level = 0;
    m_keyed = false;
    printf("m17web: simulated radio (no SDR hardware, loopback %s)\n",
           m_loopback ? "on" : "off");
}

void sim_close(void) { }

static void build_filter(void) {
    float filter[SIM_NFIL];
    m17_dsp_build_lpf_filter(filter, 0.125f, SIM_NFIL);
    m17_dsp_set_filter_gain(filter, 0.9, 1, SIM_NFIL);
    m17_dsp_float_to_short(filter, m_dec, SIM_NFIL);
    m_have_filter = 1;
}

void sim_transmit_samples(scmplx *s, int len) {
    if (len <= 0) return;
    int written = 0;
    for (int i = 0; i < len; i++) {
        if (m_count >= SIM_TXBUF) {
            // Overflow: drop the oldest (the receiver is not keeping up)
            m_tail = (m_tail + 1) % SIM_TXBUF;
            m_count--;
        }
        scmplx v;
        v.re = (int16_t)(s[i].re * m_tx_gain);
        v.im = (int16_t)(s[i].im * m_tx_gain);
        m_ring[m_head] = v;
        m_head = (m_head + 1) % SIM_TXBUF;
        m_count++;
        written++;
    }
    (void)written;
}

int sim_receive_samples(scmplx *s, int len) {
    if (!m_have_filter) build_filter();

    // Real time pacing: one 1920 sample block is 40 ms of baseband
    if (m_realtime) {
        long usec = (long)((double)len * 1000000.0 / RX_RATE);
        struct timespec ts;
        ts.tv_sec = usec / 1000000L;
        ts.tv_nsec = (usec % 1000000L) * 1000L;
        nanosleep(&ts, NULL);
    }

    double acc = 0;
    for (int i = 0; i < len; i++) {
        scmplx v;
        if (m_loopback) {
            // Shift 8 new transmit samples into the filter window, then
            // decimate exactly like the Pluto receive path does.
            for (int k = 0; k < 8; k++) {
                scmplx in;
                if (m_count > 0) {
                    in = m_ring[m_tail];
                    m_tail = (m_tail + 1) % SIM_TXBUF;
                    m_count--;
                } else {
                    in.re = 0;
                    in.im = 0;
                }
                hist_push(in);
            }
            v = sub_filter(m_hist, m_dec);
        } else {
            v.re = 0;
            v.im = 0;
        }
        // Channel noise
        if (m_noise > 0.0) {
            v.re += (int16_t)(rand_uniform() * m_noise * 8192.0);
            v.im += (int16_t)(rand_uniform() * m_noise * 8192.0);
        }
        v.re = (int16_t)(v.re * m_rx_gain);
        v.im = (int16_t)(v.im * m_rx_gain);
        s[i] = v;
        acc += ((double)v.re * v.re + (double)v.im * v.im);
    }
    if (len > 0) {
        double rms = sqrt(acc / (2.0 * len)) / 8192.0;
        m_level = rms > 1.0 ? 1.0 : rms;
    }
    return len;
}

double sim_get_level(void) { return m_level; }
void sim_set_loopback(bool on) { m_loopback = on; }
void sim_set_noise(double level) { m_noise = level; }
void sim_set_realtime(bool on) { m_realtime = on; }
bool sim_get_loopback(void) { return m_loopback; }
double sim_get_noise(void) { return m_noise; }
bool sim_get_realtime(void) { return m_realtime; }
bool sim_is_keyed(void) { return m_keyed; }
void sim_set_tx_gain(double g) { m_tx_gain = g; }
void sim_set_rx_gain(double g) { m_rx_gain = g; }
double sim_get_tx_gain(void) { return m_tx_gain; }
double sim_get_rx_gain(void) { return m_rx_gain; }
