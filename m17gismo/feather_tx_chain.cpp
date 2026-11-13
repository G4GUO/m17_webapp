/*
 * VENDORED VERBATIM from G4GUO/m17_feather, fw/tx_chain.cpp @ sha256 8fe6f082c047f8c8
 * (only the include guard and the header name differ).
 *
 * This is the modem the STM32F405 runs: dibits to 48 kHz deviation and back.
 * -r feather-loop compiles it into this program so a message can go out and
 * come back through the board's own software with no board attached.  If it
 * needs changing, change it in m17_feather first and re-vendor - the same
 * rule feather_link.c and the DSP follow.
 */
/* tx_chain.cpp - see tx_chain.h */
#include <string.h>

#include "m17defines.h"
#include "feather_tx_chain.h"

namespace m17f {

/* M17_spec.tex Table 2, normalised so the outer symbol is +-1.0 */
static const float SYM[4] = { +1.0f / 3.0f, +1.0f, -1.0f / 3.0f, -1.0f };

void TxChain::init(int taps)
{
    if (taps > MAX_TAPS) taps = MAX_TAPS;
    if (taps < N_OS) taps = N_OS;
    taps_ = taps - (taps % N_OS);
    per_phase_ = taps_ / N_OS;

    float h[MAX_TAPS];
    m17_dsp_build_rrc_filter(h, 0.5f, taps_, N_OS);
    /* sum(h) = N_OS so a constant symbol stream reproduces the symbol value
     * exactly at the sampling instants and there is no ISI there */
    m17_dsp_set_filter_gain(h, (float)N_OS, 1, taps_);

    for (int p = 0; p < N_OS; p++)
        for (int k = 0; k < per_phase_; k++)
            coef_[p * per_phase_ + k] = h[p + k * N_OS];

    reset();
}

void TxChain::reset(void)
{
    memset(hist_, 0, sizeof(hist_));
    head_ = 0;
    phase_ = 0;
    pending_ = pending_pos_ = 0;
}

void TxChain::push(uint8_t dibit)
{
    if (pending_ >= sizeof(pending_buf_)) return;    /* drop, don't corrupt */
    pending_buf_[pending_++] = (uint8_t)(dibit & 3u);
}

void TxChain::advance(void)
{
    float a = 0.0f;
    if (pending_pos_ < pending_) a = SYM[pending_buf_[pending_pos_++]];
    head_ = (head_ + 1) % HIST;
    hist_[head_] = a;
    if (pending_pos_ >= pending_) pending_ = pending_pos_ = 0;
}

float TxChain::next(void)
{
    /* Commit this symbol period's symbol before filtering it: y[n] uses
     * s[m-k] where m = n / N_OS, so the first sample of a symbol period must
     * already see that symbol as the newest entry in the history. */
    if (phase_ == 0) advance();

    const float *c = &coef_[phase_ * per_phase_];
    float sum = 0.0f;
    int idx = head_;
    for (int k = 0; k < per_phase_; k++) {
        sum += c[k] * hist_[idx];
        idx = (idx + HIST - 1) % HIST;
    }

    if (++phase_ >= N_OS) phase_ = 0;
    return sum;
}

bool TxChain::busy(void) const
{
    if (pending_pos_ < pending_) return true;
    for (int i = 0; i < HIST; i++)
        if (hist_[i] != 0.0f) return true;
    return false;
}

size_t TxChain::render(const uint8_t *dibits, size_t n, float *out, size_t cap)
{
    size_t k = 0;
    for (size_t i = 0; i < n; i++) push(dibits[i]);
    /* one extra symbol of tail so the last symbol is fully filtered */
    for (size_t i = 0; i < n * (size_t)N_OS + (size_t)N_OS && k < cap; i++)
        out[k++] = next();
    return k;
}

} // namespace m17f
