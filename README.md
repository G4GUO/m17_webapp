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

Not done yet, in priority order:

1. **Voice fidelity** - a clean simulator should reproduce the reference Codec2
   decode almost exactly; decoded audio currently correlates about 0.45
   (envelope) with the original speech after ~1.1 s of pipeline latency.  That
   points at a lost or repeated subframe in the M17 stream path (or my test
   harness's pacing); compare sent vs decoded payloads frame by frame.
2. **Real hardware** - the Pluto and Lime paths compile but have not been
   exercised since this fork (no SDR was attached during development).
3. The web UI has been driven headlessly (curl + `wstest.py`) but not clicked
   through in a real browser.
4. Reflector gateway mode (`mode gate`) has not been tested against a live
   reflector.
5. Upstream has no licence file; this fork inherits that (all rights reserved by
   default) - worth clarifying with the author before publishing.
