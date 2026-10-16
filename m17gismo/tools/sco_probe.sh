#!/bin/sh
# sco_probe.sh - does this Bluetooth device do HFP (headset) at all?
#
#     sh tools/sco_probe.sh [MAC]
#
# BlueALSA's HFP-AG support creates the SCO link when one of its two PCMs is
# opened, so this opens them directly with aplay/arecord - below the station,
# with no re-open logic in the way.  If this works, the device and bluealsa are
# fine and anything wrong afterwards is ordering inside the application.
MAC=${1:-F4:4E:FD:5D:F9:A1}
DEV="bluealsa:DEV=$MAC,PROFILE=sco"

echo "=== is the device there? ==="
bluetoothctl info "$MAC" 2>/dev/null | grep -E "Name:|Connected:|Trusted:|UUID: (Handsfree|Headset|Audio Sink)"
echo
echo "=== what bluealsa is offering ==="
bluealsa-cli list-pcms 2>/dev/null
echo
echo "=== a tone out through the headset profile (its speaker) ==="
if [ -f /tmp/tone8k.wav ]; then
    aplay -D "$DEV" /tmp/tone8k.wav >/tmp/sco_out.log 2>&1
    echo "  aplay exit: $?"
    tail -2 /tmp/sco_out.log | sed "s/^/    /"
else
    echo "  (/tmp/tone8k.wav is missing - skipped)"
fi
echo
echo "=== three seconds in from its microphone ==="
timeout 20 arecord -D "$DEV" -d 3 -t wav /tmp/sco_in.wav >/tmp/sco_in.log 2>&1
echo "  arecord exit: $?"
tail -2 /tmp/sco_in.log | sed "s/^/    /"
if [ -s /tmp/sco_in.wav ]; then
    python3 - <<'EOF'
import wave, struct
try:
    w = wave.open("/tmp/sco_in.wav", "rb")
    n = w.getnframes()
    peak = 0
    data = w.readframes(min(n, 8000 * 3))
    for i in range(0, len(data) - 1, 2):
        v = struct.unpack_from("<h", data, i)[0]
        if abs(v) > peak:
            peak = abs(v)
    print("    captured %d frames at %d Hz, peak sample %d (0 = silence)"
          % (n, w.getframerate(), peak))
except Exception as e:
    print("    could not read the capture:", e)
EOF
else
    echo "    no capture file was written"
fi
echo
echo "=== bluealsa's own log for the HFP PCMs ==="
journalctl -u bluealsa --since "-3 min" --no-pager 2>/dev/null | grep -iE "hfpag|sco|hf" | tail -6
