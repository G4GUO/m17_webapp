# The Feather modem, on this branch

`m17web` normally owns the whole signal chain: it takes IQ from an SDR, finds the
discriminator output, tracks symbol timing, frames, and decodes. The Feather is an
STM32F405 board that does everything up to the symbols, so with one fitted this
program keeps only the last two steps: framing and decoding. That is the split the
board was built for - **symbol tracking on the board, frame tracking on the host** -
and it is why the board can be a $20 microcontroller instead of a DSP.

This branch adds that back end. It is **not merged to master**: nothing on the
board has run on silicon yet (no USB enumeration, no DAC, no ADC), so the far end
of this link is still unproven. Everything on *this* side is tested - see below -
but the branch stays open until a real board answers.

## Where it joins

Two seams, both one line in the existing code:

```
  RX   m17_tx_rx.cpp   radio_rx_process(m_rsamples, len)
                         -> an SDR:  m17_dsp_rx()  (discriminator, timing)
                         -> a Feather: feather_poll() -> m17_rx_symbols()
  TX   m17_tx_routines.cpp   m17_send_dibit_array()
                         -> an SDR:  m17_mod_dibits()   (4FSK, RRC, samples out)
                         -> a Feather: feather_tx_dibits()  (dibits out)
```

`m17_send_dibit_array()` matters because every frame this program builds - carrier,
preamble, link setup, stream, BERT - is assembled into `m_tx_d[]` and ends there.
So it is the whole transmit path, not one of several.

PTT rides on the link: `LINK_TX_CONTROL` carries PTT, TXEN and the frame-sync hint,
and the board will not key unless the host asks for PTT and TXEN together - the
policy the firmware enforces and tests.

## Files

| file | what it is |
|---|---|
| `feather.cpp` | the back end: serial port, link parser, heartbeat, keying |
| `feather_link.c`, `feather_link.h` | the wire codec, vendored verbatim from `G4GUO/m17_feather` (`link/link.c`, sha256 `302c5b74…`) |
| `tests/feather_test.cpp` | the back end over a real pty, with a fake board on the far end |
| `tests/feather_fake_board.py` | a stand-in board, so the program can be run without hardware |

The codec lives on the other side of the cable too, and the firmware's own tests
are what prove it; if it needs changing, change it in `m17_feather` first and
re-vendor. The same rule the DSP follows with `m17_core`.

## Running it

```sh
make sim                              # no SDR libraries needed
FEATHER_TTY=/dev/ttyACM0 ./m17web -r feather
```

`FEATHER_TTY` defaults to `/dev/ttyACM0`, which is what the board appears as
(ST `0483:5740`). Without a board the program starts, says why the port would not
open, and runs on with no radio rather than dying.

Without hardware, give it a fake board:

```sh
./tests/feather_fake_board.py --noise    # prints a pty path; quiet band
FEATHER_TTY=<that path> ./m17web -r feather
./tests/feather_fake_board.py --echo     # loopback: dibits come back as symbols
```

`--echo` maps dibits to ideal symbol amplitudes, so the program should decode what
it transmits - an ideal channel, useful for checking the chain end to end.

## What is tested, and what is not

`make feathertest` (no hardware, no SDR libraries) puts the real link codec over a
real pty with a fake board that implements the firmware's rules:

* nothing is keyed until the host sets PTT and TXEN;
* a frame's 192 dibits arrive on the wire in the format the firmware parses;
* the echoed symbols reach `m17_rx_symbols()` in order, at the right amplitude,
  to within the packing quantisation - which is what an off-by-one in the dibit
  packing or the symbol scale would break;
* the receiver streams continuously (PTT gates the transmitter, not the ADC), and
  stops carrying data on unkey;
* the counters and the status line agree with the traffic.

16 checks. Running the real program against the fake board for eight seconds
proves it drains the link at the full 4800 symbols/s (200 blocks, where a stalled
reader would wedge after 20) and stays up.

Not tested, because it needs the board: USB enumeration, the DAC and ADC, PTT
polarity, and therefore whether any of it makes a signal.

## Known risks for the first bench session

* **FIFO depth versus jitter.** The board's dibit FIFO is two frames (80 ms) and
  it unkeys after one frame of starvation. The host sends a frame as soon as the
  program builds one, so the FIFO sits near empty and any scheduling hiccup over
  ~40 ms shows up as a gap. If that happens, deepen `SUP_DIBIT_FIFO` in the
  firmware rather than papering over it here.
* **No watchdog on the board.** A firmware hang with the radio keyed is the
  failure that matters. `board_init()` drives PTT low first, but an IWDG is not
  in yet.
* **HSE value and PTT polarity** are both build options in the firmware and both
  unconfirmed.
