# m17web — M17 digital voice over SDR, with a web interface

Receive and transmit **M17** digital voice with a PlutoSDR, a LimeSDR, or no
radio at all, and control the whole station from a web browser on any machine on
your network.  This is a fork of [G4GUO/m17_core](https://github.com/G4GUO/m17_core)
with the terminal interface replaced by a browser one; the M17 protocol, DSP and
networking are upstream's.

This is the one tree for the station: the web interface, the SMS mailbox, the
FARGAN neural receiver, AX.25 packet over M17, and the standalone KISS bridge.
The `m17_webapp_fargan` line and the separate `m17-ax25` bridge repository were
merged into it, so both remain public as history rather than as places to work.

    Host (Windows / Linux)                     Raspberry Pi 4 + SDR
    +------------------------+   WebSocket   +---------------------------+
    | browser UI             |<------------->| m17web                    |
    |  . PTT + meters        |  JSON state   |  . HTTP + WS server       |
    |  . frequencies/gains   |  PCM audio    |  . M17 modem (unchanged)  |
    |  . reflector control   |               |  . Pluto / Lime / SIM     |
    |  . console + log       |               |  . Codec2 3200 voice      |
    +------------------------+               +---------------------------+

## What you get

* **AX.25 packet over M17.** A KISS host — BPQ,
  or `kissattach` through `kissnetd` — connects to TCP port 8001 and the station
  transmits its frames as M17 packets, handing back whatever the receiver
  decodes. Off unless `data on` is in `config.txt`; see
  [docs/ax25.md](docs/ax25.md).
* **A browser station.** Push-to-talk, RX/TX frequencies and gains, signal and
  power meters, reflector selection, a live log and every console command — all
  on one page, sized to fit a screen.
* **Audio where you want it.** The browser's own microphone and speaker, the
  Pi's sound card, a Bluetooth speaker and headset, an Android phone, or silence
  — chosen separately for the speaker and the microphone in one Audio
  configuration box, with the gain and volume applied on the Pi so the meters
  show what is really going out.
* **From a phone.** The companion Android app (`/shared/AndroidStudio/M17`,
  also on this repository's `tools/android_proto_check.py` side of the wire) is
  a wireless speaker, microphone and PTT for the station: choose `phone` for
  either direction and only that client gets the audio, so the page on the desk
  stays a control surface.  The app finds the station by itself if the station
  advertises `_m17web._tcp` — see `tools/avahi/m17web.service`.
* **No hardware needed to try it.** The simulated radio runs the whole UI, the
  voice path and the logging on any Linux box.
* **Optional extras:** the FARGAN neural receiver (16 kHz voice from the same
  3200 bps bitstream), GPS position in the M17 link setup frame, recording of
  what the receiver decodes, and hardware PTT on the Pi's GPIO header.

## Layout

    m17gismo/     the station itself: M17 modem, DSP, web server, the browser
                  page in m17gismo/www/, and the AX.25/KISS packet support
    m17bridge/    the standalone KISS bridge - its own program, built from the
                  same framing sources as the station
    docs/         user manual (PDF and the reportlab source), TECHNICAL.md,
                  ax25.md, sms.md, the M17 notes
    tools/        the avahi advertisement, the Android protocol check, and
                  station-sync.sh
    examples/

## Keeping a station in step with this repo

The stations are built and run from a **clone**, never from a hand-edited copy -
that is what caused the divergence this repo was consolidated to end.  On each
station:

```sh
git clone https://github.com/G4GUO/m17_webapp.git ~/m17web-src
~/m17web-src/tools/station-sync.sh           # build, install, restart, verify
BIN_DEST=~/m17web-build/m17web ~/m17web-src/tools/station-sync.sh   # unit starts a different path
```

`station-sync.sh` fetches, refuses to run over local edits (`--force` to discard
them), resets to `origin/master`, builds with `SIM=1 CODEC2_BACKEND=mod`,
installs the binary where the systemd unit starts it and `www/` where the service
serves it, restarts, then proves the served page is byte-identical to the repo and
the panel answers on 8080.  `--check` does the verification alone.

Only two things on a station are allowed to be local: `www/`'s live copies (which
the script overwrites from the repo, deliberately) and `M17Hosts.txt` in the
service directory.

## Checking a station still works

After any change, the question is not "did the sync finish" but "is this radio
still a working station".  `tools/regress.py` answers that.  It runs on the
station, touches nothing, transmits nothing, and exits non-zero if any check
fails:

```sh
python3 ~/m17web-src/tools/regress.py            # ~15 s: deployment, service, panel, radio
python3 ~/m17web-src/tools/regress.py --deep     # + the sim/fake-reflector tests, ~90 s
python3 ~/m17web-src/tools/regress.py --json /tmp/regress.json
tools/regress-both.sh --deep                     # both bench stations, summarised
```

It checks, in order:

| check | what a failure means |
|---|---|
| `repo` | the clone is dirty or behind `origin/master` - the station is not the repo |
| `service` | the unit is not running |
| `binary` | the running binary is older than the sources - it was built from older code |
| `www/served-matches` | a served asset differs from the clone (a half-finished deploy) |
| `page/markup` | an anchor the UI needs has been renamed or removed |
| `page/assets` | `index.html` asks for an asset version that does not serve |
| `panel/websocket` | the state channel is dead |
| `radio/route`, `radio/frequency` | the SDR is not the expected one, or has no tuning |
| `radio/receiver` | zero signal - no samples are arriving from the radio |
| `radio/counters` | `gerr`/drops/overruns moved while idle |
| `command/help` | the command path is broken |
| `gpio`, `audio`, `gps` | reported, warned about, but not fatal |
| `deep/*` | the software path is broken: no sim instance, no reflector link, or a link that never times out |

Warnings do not fail the run.  A station can pass every check with a dead GPS,
which is exactly the sort of thing you want told about but not woken up for.

## Requirements

* Linux with gcc/g++, make.  Built and tested on Raspberry Pi OS (Pi 4B).
* [Codec2](https://github.com/drowe67/codec2) installed from source (the build
  needs its headers; `CODEC2_BACKEND=mod` still uses them).
* Optional, for real radios and local audio: `libiio-dev` (Pluto),
  `liblimesuite-dev` (LimeSDR), `libasound2-dev` (sound card),
  `libpulse-dev` (PulseAudio), `libgpiod-dev` (hardware PTT).
* Optional, for Bluetooth audio: `bluez-alsa-utils` and
  `libasound2-plugin-bluez`.

## Build

```sh
sudo apt install cmake libasound2-dev libiio-dev limesuite liblimesuite-dev libgpiod-dev

# Codec2 from source
git clone https://github.com/drowe67/codec2.git && cd codec2
mkdir build && cd build && cmake -DCMAKE_BUILD_TYPE=Release .. && make -j4
sudo make install && sudo ldconfig
cd ../..

git clone https://github.com/G4GUO/m17_webapp.git
cd m17_webapp/m17gismo
make LIME=1 SIM=1 CODEC2_BACKEND=mod
```

That produces `m17web`.  The flag set is not optional on Debian: a bare `make`
leaves out the simulated radio and the vendored codec (which the sources
reference unconditionally, so the link fails), and it cannot see Debian's Lime
headers.  `make sim` builds a browser UI plus simulator with no radio and no
sound card, which is the quickest way to see what the program does.

## Run

```sh
./m17web -r sim   -w 8080        # simulated radio - no hardware
./m17web -r pluto -w 8080        # PlutoSDR / Pluto+
./m17web -r lime  -w 8080        # LimeSDR
./m17web -r cm108 -w 8080        # CM108/CM119 sound fob + the analogue board
```

Then open `http://<pi-address>:8080/`.  Settings live in `config.txt` in the
working directory (callsign, mode, frequencies, audio devices, Bluetooth
choices) and are read at startup; everything can also be changed while running,
from the page or the console.

| option | meaning |
|---|---|
| `-r sim\|pluto\|lime\|feather\|feather-loop\|cm108` | radio backend |
| `-w PORT` | web UI port (default 8080) |
| `-p IP` | Pluto address (default `pluto.local`) |
| `-a browser\|pulse\|both\|none\|file:IN:OUT` | audio backend (the whole pair; the interface sets the two routes instead) |

Browser audio needs one click to start (browser autoplay rules), and a browser
will only give a web page the microphone on a secure origin — `localhost` or
HTTPS.  The Pi's sound card or a Bluetooth headset avoid the problem entirely
and are what most stations use.

## Install as a service

```ini
# /etc/systemd/system/m17web.service
[Unit]
Description=m17web - M17 digital voice over SDR (web interface)
After=network-online.target sound.target bluetooth.target

[Service]
WorkingDirectory=/home/pi/m17web
ExecStart=/home/pi/m17web/m17web -r pluto -w 8080
Restart=always

[Install]
WantedBy=multi-user.target
```

```sh
sudo systemctl daemon-reload && sudo systemctl enable --now m17web
```

Run it from a directory of its own, with its `config.txt`: the program rewrites
that file with the settings you change from the page.

## Documentation

* **[User manual (PDF)](docs/m17web-user-manual.pdf)** — written for the
  operator: setting up, making a contact, the audio options, every control and
  command, troubleshooting.  Generated from `docs/user_manual.py`.
* **[docs/TECHNICAL.md](docs/TECHNICAL.md)** — design notes, measurements and
  the history: why the audio back end is ordered the way it is, the transmit
  timing budget, the Bluetooth traps, the GPS payload layout, the Codec2
  comparison, and what changed relative to upstream.

## Licence and credits

0BSD.

* M17 implementation: **G4GUO** ([m17_core](https://github.com/G4GUO/m17_core)).
* FARGAN neural decoder: **Wojciech Kaczmarski SP5WWP**, M17 Project
  ([Codec2-mod-neural](https://github.com/M17-Project/Codec2-mod-neural)).
* Codec2: **David Rowe VK5DGR** and contributors.
