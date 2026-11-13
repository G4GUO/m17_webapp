/*
 * VENDORED VERBATIM from G4GUO/m17_feather, fw/tx_chain.h @ sha256 fe89575264c500fa
 * (only the include guard and the header name differ).
 *
 * This is the modem the STM32F405 runs: dibits to 48 kHz deviation and back.
 * -r feather-loop compiles it into this program so a message can go out and
 * come back through the board's own software with no board attached.  If it
 * needs changing, change it in m17_feather first and re-vendor - the same
 * rule feather_link.c and the DSP follow.
 */
/* tx_chain.h - dibits in, 48 kHz real deviation out. Polyphase RRC interpolator.
 *
 * Streaming, so it runs on the Feather off the DAC DMA interrupt: one call to
 * next() per output sample, 31 multiply-accumulates each, which is 1.5 M MAC/s
 * at 48 kHz on a 168 MHz M4F - about 1% of the CPU.
 *
 * No IQ, no NCO: the sample value IS the deviation, outer symbol +-1.0 and inner
 * +-0.333. The radio's FM modulator makes the carrier; the DAC just has to
 * produce the waveform. Idle output is 0.0 = mid-scale = zero deviation, which
 * is what the board sends when there is no host (see fw/supervisor.h).
 */
#ifndef M17F_TX_CHAIN_H
#define M17F_TX_CHAIN_H

#include <stddef.h>
#include <stdint.h>

namespace m17f {

class TxChain {
public:
    /* taps at 10 samples/symbol; 310 = 31 symbols, matching m17_core's TX_FN.
     * Must be a whole number of symbols (taps % N_OS == 0) for the polyphase
     * decomposition. */
    void init(int taps = 310);
    void reset(void);

    /* Queue one symbol. Dibit -> deviation is M17_spec.tex Table 2:
     * 00 -> +1/3, 01 -> +1, 10 -> -1/3, 11 -> -1. */
    void push(uint8_t dibit);

    /* Next 48 kHz sample in deviation units. 0.0 when nothing is queued and the
     * filter has run dry. */
    float next(void);

    /* True while queued symbols or filter tail remain - the supervisor uses this
     * to decide whether a frame has actually played out. */
    bool busy(void) const;

    unsigned queued(void) const { return pending_; }

    /* Convenience for hosts and tests: render n dibits plus the filter tail. */
    size_t render(const uint8_t *dibits, size_t n, float *out, size_t cap);

private:
    static const int MAX_TAPS = 512;
    static const int HIST     = 64;     /* must exceed taps / N_OS */

    float    coef_[512];        /* polyphase coefficients, phases x taps_per_phase */
    int      taps_ = 0;
    int      per_phase_ = 0;
    int      phase_ = 0;

    float    hist_[HIST];       /* most recent symbol amplitudes, hist_[head_] newest */
    int      head_ = 0;

    uint8_t  pending_buf_[4 * 192];
    unsigned pending_ = 0, pending_pos_ = 0;

    void advance(void);         /* pull the next symbol into the history */
};

} // namespace m17f

#endif
