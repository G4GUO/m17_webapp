# m17_sdr_web / m17web

Web-UI fork of [G4GUO/m17_sdr](https://github.com/G4GUO/m17_sdr): the same M17
SDR implementation, but the ncurses terminal interface has been replaced by a
browser interface.  The M17 protocol, DSP, framing and network code is
upstream's own; everything front-end related is new.

    Host (Windows / Linux)                     Raspberry Pi 4 + SDR
    +------------------------+   WebSocket   +---------------------------+
    | browser UI             |<------------->| m17web                    |
    |  . PTT + meters        |  JSON state   |  . HTTP + WS server       |
    |  . frequencies/gains   |  PCM audio    |  . M17 modem (unchanged)  |
    |  . reflector control   |               |  . Pluto / Lime / SIM     |
    |  . console + log       |               |  . Codec2 3200 voice      |
    +------------------------+               +---------------------------+

## What changed relative to upstream

| Area | Upstream | Here |
|------|----------|------|
| User interface | ncurses (`gui.cpp`, stdin command loop in `main.cpp`) | `webui.cpp` + `webserver.c` + `www/` single page app |
| Console UI | only option | still available: `make console` (`ui_console.cpp`, same API) |
| Voice audio | PulseAudio on the Pi | browser mic/speaker (WebAudio) by default; `pulse`, `both`, `file`, `none` selectable at run time |
| Radio | Pluto / LimeSDR | plus a **simulated radio** (`radio_sim.cpp`) so everything runs with no hardware |
| UI boundary | `gui_*()` calls scattered through the code | same names, now declared in `ui.h` and implemented by either front end |
| Binary | `m17gismo` | `m17web` |

Nothing in `m17_*.cpp`, `pluto.cpp`, `lime.cpp`, `gps.cpp`, `buffers.cpp` was
rewritten for the web UI.  Small additive changes only: a reflector-module
getter, gain/name/type getters, a `radio_keyed()` fix (upstream always called
the Lime function, so a Pluto station could never read its own PTT input), and
compile guards so back ends are selected at build time.

## Building

Needs gcc/g++, make, **Codec2** built from source, and whichever of libiio
(Pluto), LimeSuite (LimeSDR), libpulse-simple (local audio) you have:

    sudo apt install libiio-dev limesuite liblimesuite-dev libpulse-dev

    cd m17_sdr_web/m17gismo
    make                # everything that is available
    make sim            # no SDR hardware, no PulseAudio: web UI + simulator
    make console        # legacy ncurses interface

Build options: `PULSE=0 PLUTO=0 LIME=0 SIM=1 CONSOLE=1 WEB=0 EMBED=0 DEBUG=1`,
`CODEC2_PREFIX=/usr/local`.

Codec2 itself (no cmake on Raspberry Pi OS by default):

    sudo apt install cmake
    git clone https://github.com/drowe67/codec2.git && cd codec2
    mkdir build && cd build && cmake -DCMAKE_BUILD_TYPE=Release .. && make -j4
    sudo make install && sudo ldconfig

## Codec2 back ends: stock libcodec2 or the modified codec

Two codecs can be built in, selected with `CODEC2_BACKEND`:

    make                        # CODEC2_BACKEND=lib (default): external libcodec2
    make CODEC2_BACKEND=mod     # vendored M17-Project/Codec2-mod, no external codec

    make bitexact-check INPUT=~/codec2/raw/hts1a.raw    # compare the two

`codec2mod/` contains [M17-Project/Codec2-mod](https://github.com/M17-Project/Codec2-mod)
vendored at commit `7c334283` (see `codec2mod/VENDORED.txt`).  That fork is a
minimal extraction of **only** the 3200 bps mode - which is exactly what M17
uses - with static, caller-supplied codec state, no runtime `malloc`, and a
smaller footprint.  It ships no build system, so the makefile compiles its
sources directly, and `codec2mod/codec2_shim.c` adapts its static-state API
(`codec2_encoder_t` / `codec2_encode()`) to the upstream API the rest of the
project expects (`codec2_create(CODEC2_MODE_3200)` / `codec2_encode()`), so no
other file needed changing.  With `CODEC2_BACKEND=mod` the binary has **no
libcodec2 dependency at all** (verify with `ldd`).

Measured on the Pi 4B, same 150 frames of `hts1a.raw`, stock vs modified:

| | stock libcodec2 | modified Codec2 | ratio |
|---|---|---|---|
| encode | 0.145 ms/frame | 0.070 ms/frame | **2.07x faster** |
| decode | 0.104 ms/frame | 0.088 ms/frame | 1.18x faster |

Bitstream compatibility, which is what matters on air: **149 of 150 frames were
byte-for-byte identical**; one frame differed by 2 bits.  The fork claims
bit-exact encoder output, and on this platform that holds for 149/150 frames
here - worth knowing before relying on it for forensic comparison, but 2 bits
in one frame is irrelevant to interop.  Decoded audio was not byte-identical
(as the fork documents) but correlated 0.9999 on energy envelopes.

Both codecs were also run through the full modem path (simulator, duplex,
`file:` audio in and out): identical behaviour, `gerr=0`, decoded audio
returned in both cases.

## Running

    ./m17web -r sim -w 8080                     # simulated radio, web UI port 8080
    ./m17web -r pluto -w 8080                   # Pluto / Pluto+
    ./m17web -r lime -w 8080                    # LimeSDR
    ./m17web -r sim -a file:in.raw:out.raw      # headless voice test

    -p IP      Pluto address (default pluto.local)
    -w PORT    web UI port (default 8080)
    -www DIR   web assets (default ./www; the bundled copy is embedded in the binary)
    -a BACKEND browser (default) | pulse | both | none | file:IN.raw:OUT.raw

Open `http://<pi-address>:8080/`.  `config.txt` in the working directory is run
at startup exactly as before (callsign, mode, frequencies, ...).

Browser audio needs one click ("enable browser audio") because of autoplay
rules, and needs a secure context (localhost, https, or an explicit browser
exception) for the microphone; Firefox allows plain http, Chrome usually does
not.  The PulseAudio back end or the file back end can be used instead.

## Web interface

* **PTT** - hold the button or the space bar; break / carrier / duplex buttons.
  Live RX/TX/DX indicator, signal and power meters, "in frame" badge.
* **Station** - callsign, destination, gateway, broadcast, listen-to-all.
* **Radio** - RX/TX frequency, gains, frequency correction, AFC; simulator
  controls (loopback, real time, noise) appear when running `-r sim`.
* **Reflector** - pick a reflector (list read from `M17Hosts.txt`) plus module,
  connect / disconnect, connection status.
* **Audio** - enable browser audio, choose the back end, volume, mute, and
  mic/speaker level meters.
* **Console** - every MMI command from the terminal version works here, with a
  scrolling log; `h` for help, `w` to clear.

Wire protocol (WebSocket at `/ws`): JSON text frames for state (`{"t":"state",...}`),
log lines, command responses, help text and the reflector list; binary frames
for audio, tagged `0x01` + s16le PCM (8 kHz mono) for the microphone and `0x02`
+ PCM for decoded receive audio.

## Transmit samples used to be dropped: the rates and the fix

The transmit path is fed by frames, so its rates and its timing budget both
matter.  On the Pluto the whole chain is an exact integer division of the
AD9361's clock - there is no fractional resampling anywhere:

| stage | rate | ratio to the next |
|---|---|---|
| AD9361 sampling_frequency | 3 072 000 S/s | /8 in the AD9361 |
| Pluto stream (`cf-ad9361-lpc`) | 384 000 S/s | /48 |
| voice audio (codec2 3200) | 8 000 Hz | x6 (ALSA plug, card does 48 kHz) |
| M17 symbols | 4 800 sym/s | x80 = 384 000 (`radio_get_oversample()`) |
| modulator block `N_SAMPLES` | 1 920 samples | x8 = 15 360 = one 40 ms sub frame |

384000/8000 = 48, 48000/8000 = 6, 384000/48000 = 8, 4800 x 80 = 384000, and
1920 x 8 = 15360 samples = exactly one 40 ms M17 sub frame = 2 codec frames.
The ratio being integral means chunking can be aligned to whole audio frames and
cannot drift - it does not mean the two crystals are locked, which is why the
capture and transmit buffers still exist.

What actually went wrong was the **timing budget**.  `make` with `DIAG=1` prints
the transmit loop's per-sub-frame cost (`diag-tx:`), and it read:

```
audio(incl pace)=6us  encode=20228us  send+radio=26485us  total=47525us  (40000us = real time)
```

46.7 ms of work for 40 ms of stream is 17% slow, so the DAC starved (repeated
samples - the far end hears dropped transmit audio) and the microphone ring
overflowed (samples dropped before they were ever encoded).  The two costs:
codec2 3200 encoding is ~10 ms per 20 ms frame on a Pi 4B (half the budget on
its own), and pushing one 1920 sample block per `iio_buffer_push` cost ~26 ms
per sub frame in USB overhead - 3 ms a push, eight pushes a frame.

Fixes, all measured afterwards:

* `pluto_tx_samples()` accumulates and pushes **whole 40 ms sub frames**
  (`TX_CHUNK_SAMPLES` 15360) instead of eight 5 ms blocks: 26.5 ms -> 6.5 ms.
* `audio_alsa_input()` no longer reads the card from the transmit loop.  A
  thread drains the card continuously into a 2 s ring, so a slow moment in the
  loop cannot overflow the card's ring and silently discard microphone audio.
  Overruns and lost samples are counted now (`audio_tx_overruns()`,
  `audio_tx_dropped()`), because they used to be invisible.
* `audio_input()` paces on a **running deadline** rather than sleeping what is
  left of the current block, so the encode/modulate/push time is charged to the
  40 ms frame instead of being added on top of it.
* `m17_tx_lead_in()` sends a few **blank codec frames** (encoded silence, not
  zero bytes - the first payload logs as `C0006A439CE42108`) before the voice,
  so the receiver's acquisition and the microphone's start-up fall in silence
  instead of in speech.  `lead N` (or `M17WEB_TX_LEAD`) sets it, 2 frames = 40 ms,
  default 4.
* `audio_input_flush()` at key-down drops what the microphone captured while the
  radio was starting up (~5 s on this Pluto), which otherwise made the whole
  over lag the microphone by the depth of the buffer.

After that: transmit loop 39 990-40 010 us per 40 000 us frame, audio
consumption 8010 samples/s against the card's 7986, `tx_drops=0`,
`tx_overruns=0`, ring depth 0 (live audio, no lag).  The pending measurements
are `tx_drops` and `tx_overruns` in the state JSON, shown in the UI's Audio
panel next to the blank-frame setting.

## Audio: which device the voice comes from and goes to

The web UI has an **audio device** selector (the "Audio" panel) plus the
equivalent commands, and the choice is applied live - the radio loop re-opens
its streams within one transmit/receive cycle, no restart needed:

| UI option | command | what it does |
|---|---|---|
| Raspberry Pi sound card (direct) | `audio alsa [device]` | the card through **ALSA**, no daemon in the path |
| browser microphone / speaker | `audio browser` | WebAudio in the page, 8 kHz over the WebSocket |
| Raspberry Pi sound card (PulseAudio) | `audio pulse` | the same card through PulseAudio |
| browser + Pi sound card | `audio both` | mic from the browser (falling back to the card), audio out to both |
| mute | `audio none` | silence in, discard out |
| (testing) | `-a file:in.raw:out.raw` | raw 8 kHz files instead of a device |

`make` builds the ALSA back end when `libasound2-dev` is present (`ALSA=1`,
override with `ALSA=0`) and the PulseAudio one when `libpulse-dev` is
(`PULSE=1`).

**Use the direct ALSA option on a Pi.** Two reasons, both measured on this one:

* Debian routes ALSA's `default` PCM *through* PulseAudio, so "plain ALSA" is
  still PulseAudio unless you name a real device.
* With PulseAudio installed, the app's streams attached to the USB card
  (`pactl` showed `sink-input ... s16le 1ch 8000Hz`, `Corked: no`) while the
  sink stayed `SUSPENDED` with the ALSA device `closed` and a 190 s buffer, i.e.
  PulseAudio accepted writes but never ran the card - even for a direct
  `pacat`.  `aplay -D plughw:3,0` worked, which is what pointed at ALSA.

The direct back end picks its device itself: the configured device first
(`audio alsa plughw:3,0`, or `M17WEB_ALSA_DEVICE`), otherwise the first USB/PnP
card it finds as `plughw:<card>,0` (rate conversion included, so 8 kHz mono
just works), otherwise `sysdefault`.

Verified on the Pi: with `-a alsa` the state reports
`audio=alsa alsa={'compiled': True, 'ready': True, 'device': 'plughw:3,0'}`,
the card's ALSA capture PCM shows `state: RUNNING owner_pid <m17web>`, CPU stays
around 4%, and switching `alsa -> browser -> alsa` in the UI is picked up live.
The playback PCM occasionally reports `XRUN` in `mode loop` (audio loopback is
the one mode that reads and writes the same card at once, so the writer falls a
fraction of a period behind); the back end re-prepares and continues, and the
real transmit/receive modes do not read and write simultaneously.

## Simulated radio (no hardware needed)

`-r sim` loops transmit samples back into the receiver through the same 8x
decimating filter the Pluto path uses, adds adjustable noise and paces the
receive path to real time.  It exercises the whole chain - modulation, sync,
Golay/LICH decode, framing, Codec2 - with `gerr=0` on a clean channel, so the
web UI, audio path and protocol code can be developed without an SDR attached.

Commands: `sim loop on|off`, `sim realtime on|off`, `sim noise 0.0-0.5`.

## Testing tools (`tools/`)

| Tool | Purpose |
|------|---------|
| `wstest.py` | headless WebSocket client (stdlib only): commands, hold PTT, stream a `.raw` file into the mic, save decoded audio |
| `audio_verify.py` | decoded audio against a reference `c2enc`/`c2dec` decode: envelope correlation (Codec2 is a vocoder, sample correlation is not meaningful) |
| `audio_compare.py` | chunked, drift tolerant comparison of two raw files |
| `pi_m17web.sh` | start/stop/status/log on the Pi with a generated test `config.txt` |
| `deploy_pi.sh` | rsync the sources to the Pi and build there |
| `embed_www.py` | generate `www_assets.h` from `www/` |

Example session (on the Pi):

    cd m17gismo && make sim
    MODE=radio bash tools/pi_m17web.sh start
    python3 tools/wstest.py --cmd "sa G4GUO" --cmd "td" --ptt \
        --wav-in ~/codec2/raw/hts1a.raw --wav-out /tmp/dec.raw --duration 6
    python3 tools/audio_verify.py ~/codec2/raw/hts1a.raw /tmp/dec.raw

## Status

Verified on a Raspberry Pi 4 (Debian 13, Codec2 built from source):

* builds clean as `make sim` (web UI + simulator, no SDR libraries)
* HTTP serves the app; WebSocket handshake, state, commands, help, reflector
  list and log replay all work headlessly (`tools/wstest.py`)
* audio flows both ways over the WebSocket (mic frames in, decoded frames out)
  and through the file back end
* duplex through the simulator decodes its own transmission with `gerr=0`
* clean SIGTERM/SIGINT shutdown, including from a browser `q` command
* both codec back ends work in the app (mode loop and through the modem)
* the audio device selector works live for browser, direct-ALSA and PulseAudio
  back ends, including the Pi's USB sound card (see the audio section above)
* the transmit path runs in real time on the Pluto with the Pi's sound card:
  no dropped samples, no card overruns, blank codec frames before the voice
  (see the transmit section above)

Also deployed against real hardware: a PlutoSDR (ADALM-PLUTO, fw v0.33,
reachable at 192.168.2.1 over the USB RNDIS interface ```eth1```).  The web UI
runs on the Pi with ```-r pluto``` and ```iio_attr``` confirms the radio is being
driven: RX and TX LO both 146 400 000 Hz, RX sample rate 384 kS/s (3.072 MS/s
with the x8 decimation the code expects), rf_bandwidth 200 kHz, live RSSI in
the UI.  PulseAudio and the simulator are still compiled in, so
```-r sim```/```-a pulse``` remain available as fallbacks.

Not done yet, in priority order:

1. **Voice fidelity** - a clean simulator should reproduce the reference Codec2
   decode almost exactly; decoded audio currently correlates about 0.45
   (envelope) with the original speech after ~1.1 s of pipeline latency.  That
   points at a lost or repeated subframe in the M17 stream path (or my test
   harness's pacing); compare sent vs decoded payloads frame by frame.
2. **Hardware PTT** - ```rpi_gpio.cpp``` uses the legacy sysfs GPIO interface,
   which this kernel rejects outright (```echo 10 > /sys/class/gpio/export``` gives
   EINVAL even as root), so the physical PTT input/output pins do not work;
   PTT from the browser does.  Fixing it means rewriting ```rpi_gpio.cpp``` on
   libgpiod.  (The export/unexport swap found upstream was still wrong and is
   fixed.)
3. **Partly persistent** - the Pluto interface now has a NetworkManager profile
   (```nmcli con show pluto```: ```eth1```, static ```192.168.2.2/24```,
   autoconnect), which was needed because the Pluto re-enumerates on USB and the
   hand-added address disappeared with it.  Nothing yet starts m17web at boot;
   a systemd unit is the remaining step.  The program no longer dies when the
   Pluto is missing - it reports why and falls back to the simulator so the web
   UI stays usable.
4. **LimeSDR** - still untested (none attached).
5. The web UI has been driven headlessly (curl + `wstest.py`) but not clicked
   through in a real browser.
6. Reflector gateway mode (`mode gate`) has not been tested against a live
   reflector.
7. Upstream has no licence file; this fork inherits that (all rights reserved by
   default) - worth clarifying with the author before publishing.