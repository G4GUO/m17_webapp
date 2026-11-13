/*
 * VENDORED VERBATIM from G4GUO/m17_feather, fw/rx_chain.cpp @ sha256 fd83f5aaefda8d90
 * (only the include guard and the header name differ).
 *
 * This is the modem the STM32F405 runs: dibits to 48 kHz deviation and back.
 * -r feather-loop compiles it into this program so a message can go out and
 * come back through the board's own software with no board attached.  If it
 * needs changing, change it in m17_feather first and re-vendor - the same
 * rule feather_link.c and the DSP follow.
 */
/* rx_chain.cpp - see rx_chain.h */
#include <string.h>

#include "m17defines.h"
#include "feather_rx_chain.h"

namespace m17f {

void RxChain::init(int lpf_taps, float lpf_bw)
{
    if (lpf_taps > MAX_TAPS) lpf_taps = MAX_TAPS;
    taps_ = lpf_taps;

    m17_dsp_build_lpf_filter(coef_, lpf_bw, taps_);
    m17_dsp_apply_window(coef_, taps_);
    m17_dsp_set_filter_gain(coef_, 1.0f, 1, taps_);

    reset();
}

void RxChain::reset(void)
{
    memset(ring_, 0, sizeof(ring_));
    ring_pos_ = 0;
    decim_ = 0;
    dc_y_ = dc_x_ = 0.0f;
    stage_n_ = 0;
    m17_rx_sync_init();
}

/* Hand the accumulated 9600 Hz samples to the timing loop. m17_rx_sync_samples()
 * resets its own output index per call, so symbols come back contiguous. */
int RxChain::drain(float *sym, int cap)
{
    if (stage_n_ < 2) return 0;

    /* len must be even: the loop toggles an internal 2-sample clock across calls,
     * and an odd length would pair half a symbol with the next call's first
     * sample. Leaving a sample staged keeps that alignment.
     */
    int len = stage_n_ & ~1;
    if (len == 0) return 0;

    /* the timing loop can emit one more symbol than len/2 on a phase step */
    float tmp[STAGE / 2 + 8];
    int ns = m17_rx_sync_samples(stage_, tmp, len);
    if (ns > cap) ns = cap;
    if (ns > 0) memcpy(sym, tmp, sizeof(float) * (size_t)ns);

    if (len < stage_n_)
        memmove(stage_, stage_ + len, sizeof(float) * (size_t)(stage_n_ - len));
    stage_n_ -= len;

    return ns;
}

int RxChain::process(const float *in, int n, float *sym, int cap)
{
    int out = 0;

    for (int i = 0; i < n; i++) {
        /* DC block first: a discriminator offset of 0.3 of full scale is
         * harmless here (rig result), but it must go before the decimator or it
         * shows up as a bias on the symbol values. */
        float x = in[i];
        dc_y_ = 0.999f * dc_y_ + x - dc_x_;
        dc_x_ = x;
        float v = dc_y_;

        ring_[ring_pos_] = v;
        ring_pos_ = (ring_pos_ + 1) % taps_;

        if (++decim_ >= 5) {
            decim_ = 0;
            /* FIR over the ring: newest sample is at ring_pos_ - 1 */
            float sum = 0.0f;
            int idx = (ring_pos_ + taps_ - 1) % taps_;
            for (int k = 0; k < taps_; k++) {
                sum += coef_[k] * ring_[idx];
                idx = (idx + taps_ - 1) % taps_;
            }
            stage_[stage_n_++] = sum;
            if (stage_n_ >= STAGE) {
                int ns = drain(sym + out, cap - out);
                out += ns;
                if (out >= cap) break;
            }
        }
    }

    if (out < cap) out += drain(sym + out, cap - out);
    return out;
}

} // namespace m17f
