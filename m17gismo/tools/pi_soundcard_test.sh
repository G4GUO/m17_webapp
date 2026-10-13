#!/bin/bash
# tools/pi_soundcard_test.sh
#
# Verifies the Raspberry Pi sound card audio back end, without anyone having to
# speak into a microphone:
#
#   1. loads a PulseAudio null sink and makes its monitor the default source,
#      so m17web's microphone input becomes a file we can play into it
#   2. bounces the app's audio backend (none -> pulse) so it re-opens its
#      streams against that source - this also exercises the web UI hot swap
#   3. keys the transmitter through the web UI (tools/wstest.py)
#   4. plays a raw 8 kHz test vector into the virtual source
#   5. records what m17web sends to the real sound card from its monitor source
#
# The app must already be running on the Pi sound card backend, e.g.
#   RADIO=pluto MODE=loop bash tools/pi_m17web.sh start -a pulse
#
# Then compare the recording with the codec reference decode:
#   python3 tools/audio_verify.py /tmp/c2_mod.raw /tmp/pi_soundcard_capture.raw
#
# Notes: recording needs "parec" (pacat is the playback half of the same
# binary), and the monitor of an idle sink delivers nothing until the sink is
# woken up, which step 0 handles.
#
set -uo pipefail

RAW=${1:-/home/charles/codec2/raw/hts1a.raw}
HERE=$(cd "$(dirname "$0")" && pwd)
PORT=${PORT:-8080}
CAP=${CAP:-/tmp/pi_soundcard_capture.raw}
SECS=${SECS:-4}

export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
T() { timeout "$1" "${@:2}"; }          # every external call is bounded

command -v pactl >/dev/null || { echo "pactl not found - install pulseaudio-utils"; exit 1; }
T 10 pactl info >/dev/null 2>&1 || { echo "no PulseAudio server reachable"; exit 1; }

SINK=$(T 10 pactl get-default-sink)
ORIG_SRC=$(T 10 pactl get-default-source)
echo "default sink : $SINK"
echo "default src  : $ORIG_SRC"

# 0. wake the sink, and drop any null sinks left over from earlier runs
T 10 pactl suspend-sink "$SINK" 0 >/dev/null 2>&1
for idx in $(T 10 pactl list short modules | awk '/module-null-sink/{print $1}'); do
    T 10 pactl unload-module "$idx" >/dev/null 2>&1
done
T 10 pactl load-module module-null-sink sink_name=m17mic \
    sink_properties=device.description=m17mic >/dev/null
T 10 pactl set-default-source m17mic.monitor
echo "mic source   : $(T 10 pactl get-default-source)  (fed from $RAW)"

# 1. re-open the app's audio streams so it picks up the new default source
python3 "$HERE/wstest.py" --port "$PORT" --cmd "audio none"  --steps 1 --duration 0.3 >/dev/null 2>&1
python3 "$HERE/wstest.py" --port "$PORT" --cmd "audio pulse" --steps 1 --duration 0.3 >/dev/null 2>&1
sleep 0.5

# 2. key the transmitter (the state persists after the client exits)
python3 "$HERE/wstest.py" --port "$PORT" --cmd "tx" --steps 1 --duration 0.3 >/dev/null 2>&1
sleep 0.5

echo "pulse streams belonging to m17web:"
T 10 pactl list short sink-inputs
T 10 pactl list short source-outputs

# 3. record the card while injecting the test vector
echo "recording $SINK.monitor -> $CAP"
T $((SECS+4)) parec --raw --rate=8000 --channels=1 --format=s16le \
    --device="$SINK.monitor" > "$CAP" 2>/tmp/pi_soundcard_capture.err &
CAPPID=$!
sleep 0.7
echo "playing $RAW into m17mic"
T $((SECS+2)) pacat --raw --rate=8000 --channels=1 --format=s16le \
    --device=m17mic < "$RAW" 2>/dev/null
sleep 1
kill "$CAPPID" 2>/dev/null
wait "$CAPPID" 2>/dev/null
python3 "$HERE/wstest.py" --port "$PORT" --cmd "rx" --steps 1 --duration 0.3 >/dev/null 2>&1
T 10 pactl set-default-source "$ORIG_SRC"

echo "captured $(stat -c%s "$CAP" 2>/dev/null || echo 0) bytes into $CAP"
err=$(cat /tmp/pi_soundcard_capture.err 2>/dev/null)
[ -n "$err" ] && echo "recorder said: $err"
python3 - "$CAP" <<'PY'
import array, math, sys
p = sys.argv[1]
try:
    d = open(p, "rb").read()
except OSError:
    sys.exit(0)
if len(d) >= 2:
    a = array.array("h"); a.frombytes(d[:len(d) & ~1])
    n = min(40000, len(a))
    rms = math.sqrt(sum(v * v for v in a[:n]) / max(1, n))
    print(f"  {len(a)} samples captured, RMS {rms:.0f} (0 would mean silence)")
PY
