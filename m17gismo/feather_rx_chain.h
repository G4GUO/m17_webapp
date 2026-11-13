/*
 * VENDORED VERBATIM from G4GUO/m17_feather, fw/rx_chain.h @ sha256 194789f64848657a
 * (only the include guard and the header name differ).
 *
 * This is the modem the STM32F405 runs: dibits to 48 kHz deviation and back.
 * -r feather-loop compiles it into this program so a message can go out and
 * come back through the board's own software with no board attached.  If it
 * needs changing, change it in m17_feather first and re-vendor - the same
 * rule feather_link.c and the DSP follow.
 */
/* rx_chain.h - 48 kHz real baseband in, symbols out. Symbol tracking only.
 *
 * This is the whole of what the Feather does on receive: DC block, anti-alias
 * LPF with /5 decimation to 9.6 kS/s, then the 40-phase RRC matched filter and
 * timing loop from dsp/m17_rx_sync.cpp. No framing, no demap - the host owns
 * those, because the host is what finds the sync burst and therefore what knows
 * the gain reference (see docs/LINK.md 3).
 *
 * The timing loop asks m17_rx_lock() whether it is locked so it can pick its
 * adjustment threshold (10 unlocked / 80 locked). On the Feather that comes from
 * the host's RX_LOCK hint; fw/rx_lock.cpp provides it, and it is deliberately not
 * linked into the rig, which has the real framer.
 */
#ifndef M17F_RX_CHAIN_H
#define M17F_RX_CHAIN_H

#include <stddef.h>

namespace m17f {

class RxChain {
public:
    /* lpf_bw is normalised to the sample rate: 0.128 = 6.1 kHz at 48 kHz. The
     * anti-alias corner must sit well above the 3.6 kHz RRC edge - 4.4 kHz cost
     * ~10% SER in the rig (docs/FINDINGS.md 4). */
    void init(int lpf_taps = 241, float lpf_bw = 0.128f);
    void reset(void);

    /* Process n samples of real baseband. Writes up to cap symbols, returns the
     * count. Symbols arrive at 4800/s, i.e. roughly n/10, but the timing loop may
     * insert or drop one at a phase step, so always trust the return value. */
    int process(const float *in, int n, float *sym, int cap);

private:
    static const int MAX_TAPS = 512;
    static const int STAGE    = 240;    /* 9600 Hz samples buffered per timing call */

    float coef_[MAX_TAPS];
    int   taps_ = 0;
    float ring_[MAX_TAPS];
    int   ring_pos_ = 0;
    int   decim_ = 0;

    /* DC blocker state: y = a*y + x - x_prev */
    float dc_y_ = 0.0f, dc_x_ = 0.0f;

    float stage_[STAGE];
    int   stage_n_ = 0;

    int   drain(float *sym, int cap);
};

} // namespace m17f

#endif
