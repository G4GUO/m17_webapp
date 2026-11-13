//
// cm108.h
//
// A C-Media CM108/CM119 sound fob as the radio back end: the cheap way to put
// real converters on the analogue board while the Feather firmware is still
// being proven.
//
// The fob is an ordinary USB sound card, so the DSP stays here - the same
// feather_tx_chain/feather_rx_chain the board runs, vendored from m17_feather.
// The only thing the fob adds is 16-bit converters at 48 kHz (better than the
// STM32F405's 12-bit ones) and a GPIO pin for PTT, reached through the chip's
// HID interface.
//
//   TX  m17_mod_dibits() -> cm108_tx_dibits() -> TxChain -> 48 kHz -> fob out
//   RX  fob in -> 48 kHz -> RxChain -> symbols -> m17_rx_symbols()
//   PTT hidraw vendor report, GPIO3 (LQFP pin 13), keyed around the audio
//
// PTT and audio are separate transports here: a HID report has no ordering
// relationship with an isochronous stream.  So keying is padded - silence into
// the stream, then the PTT edge, then silence on the air - and unkeying waits
// for the modulator's filter tail to go out.  The Feather does not need any of
// this because there PTT and dibits share one endpoint; see docs/CM108.md.
//
#ifndef M17WEB_CM108_H
#define M17WEB_CM108_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Defaults, all overridable from the environment: CM108_PCM (playback),
// CM108_PCM_IN (capture, defaults to the same device), CM108_HID,
// CM108_PREKEY_MS, CM108_TAIL_MS, CM108_GPIO.
#define CM108_DEFAULT_PCM   "hw:1,0"
#define CM108_GPIO_PIN      3       // the tack-solderable one, LQFP pin 13

// dev == NULL means CM108_DEFAULT_PCM (or $CM108_PCM); hid == NULL means find a
// C-Media hidraw node.  Returns false and leaves the back end closed if either
// cannot be opened - a fob is a thing that gets unplugged, so the program keeps
// running and says so.
bool cm108_open(const char *pcm_dev, const char *hid_dev);
void cm108_close(void);
bool cm108_open_ok(void);

// Key / unkey, with the padding described above.  Called from radio_transmit()
// and radio_receive(), so it follows the program's own keying discipline.
void cm108_key(bool on);
bool cm108_keyed(void);

// One frame of dibits (192) from the transmit path.
void cm108_tx_dibits(const uint8_t *dibits, int n);

// Baseband straight to the fob, for radio_silence() and anything else that
// wants to send samples rather than symbols.
void cm108_write_samples(const float *s, int n);

// Called from the receive loop with the number of samples the loop expected:
// reads that much audio, runs the receive chain and hands symbols to the framer.
void cm108_poll(int n);

const char *cm108_status(void);

uint32_t cm108_symbols_rx(void);
uint32_t cm108_dibits_tx(void);

#ifdef __cplusplus
}
#endif

#endif // M17WEB_CM108_H
