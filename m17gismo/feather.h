//
// feather.h
//
// The Feather STM32F405 modem as a radio back end.
//
// The board does the radio work: DAC and ADC at 48 kHz, 4FSK with RRC, the
// discriminator, and symbol timing.  It does NOT frame: it hands up symbols and
// takes down dibits.  So this back end replaces the whole IQ path - there are no
// samples here at all - and meets the rest of the program at two points:
//
//   RX  radio_rx_process() -> feather_poll() -> m17_rx_symbols()   (m17_rx_frame)
//   TX  m17_send_dibit_array() -> feather_tx_dibits() -> TX_DIBITS -> USB
//
// PTT lives here too, because it lives on the board: LINK_TX_CONTROL carries
// PTT, TXEN and the frame-sync hint, and the board will not key without it.
//
#ifndef M17WEB_FEATHER_H
#define M17WEB_FEATHER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FEATHER_DEFAULT_TTY  "/dev/ttyACM0"

// Open the serial port.  path == NULL means FEATHER_DEFAULT_TTY.  Returns false
// and leaves the back end closed if the port cannot be opened; the program keeps
// running (this is a radio that can be unplugged).
bool feather_open(const char *path);
void feather_close(void);
bool feather_open_ok(void);

// Key / unkey.  The board only keys when the host says PTT and TXEN together.
void feather_key(bool on);
bool feather_keyed(void);

// Called from the receive loop.  Reads whatever the board has sent, delivers
// symbols to the framer, and keeps the control heartbeat going.
void feather_poll(void);

// Called from the transmit path with one frame's worth of dibits (192).
void feather_tx_dibits(const uint8_t *dibits, int n);

// One line for the UI: link state, the board's counters, and anything that has
// gone wrong on the host side.
const char *feather_status(void);

// ------------------------------------------------------------------ loopback
// The board's own modem, without the board: dibits are modulated to 48 kHz
// deviation by feather_tx_chain and fed sample by sample into the front end of
// feather_rx_chain, which hands symbols to m17_rx_symbols() exactly as the
// Feather does over the cable.  -r feather-loop selects it.
//
// This is the honest way to exercise the firmware's DSP without hardware: the
// code in the path is the code the STM32F405 runs, and a message that survives
// the trip has been through the RRC, the discriminator and the timing loop.
void feather_loop_open(void);
void feather_loop_tx_dibits(const uint8_t *dibits, int n);
// Called while receiving: drains the modulator's filter tail, which the board
// does on its own because its DAC never stops asking for samples.
void feather_loop_idle(void);
const char *feather_loop_status(void);

// How long since anything valid arrived.  The board times the host out after
// 100 ms, so the host has to notice first.
uint32_t feather_silent_ms(void);

uint32_t feather_symbols_rx(void);
uint32_t feather_frames_tx(void);
uint32_t feather_drops(void);

#ifdef __cplusplus
}
#endif

#endif // M17WEB_FEATHER_H
