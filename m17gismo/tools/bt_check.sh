#!/bin/sh
# bt_check.sh - rebuild, restart, and show how the Bluetooth audio path is doing.
#
# Run it on the Pi from the m17gismo directory:
#     sh tools/bt_check.sh [MAC] [speaker|headset]
#
# It is deliberately read-only apart from the rebuild/restart: it asks the
# station to route to the device given (default: the Anker speaker on this
# station, which does A2DP playback; for HFP pass a headset's address) and then
# reports which bluealsa directions came up and where the audio ended up.
#
# Why it exists: the interesting failures here are ordering ones - bluealsa
# creates a Bluetooth PCM only once the profile is up, and a headset's SCO link
# appears when the first of its two streams opens - so the useful evidence is
# which PCMs started and which device the app settled on, not just "it failed".
MAC=${1:-F4:4E:FD:5D:F9:A1}
ROLE=${2:-headset}

echo "=== build ==="
make LIME=1 SIM=1 CODEC2_BACKEND=mod 2>&1 | grep -E "^[a-z_/.]*:[0-9]+:[0-9]+: error" | head -6
echo "BUILD_DONE ($(stat -c%s m17web) bytes)"

echo
echo "=== restart (stop, wait for the card, start) ==="
sudo systemctl stop m17web
for i in $(seq 1 20); do
    sudo fuser /dev/snd/pcmC3D0p /dev/snd/pcmC3D0c >/dev/null 2>&1 || break
    sleep 1
done
sudo systemctl start m17web
sleep 12
printf '  service: %s\n' "$(systemctl is-active m17web)"

echo
echo "=== asking the station to use $MAC as a $ROLE ==="
timeout 60 python3 tools/wstest.py --port 8080 --cmd "bt use $MAC $ROLE" --steps 3 --quiet 2>&1 | grep -E "^\s*\[resp" | tail -3
sleep 14

echo
echo "=== bluealsa directions that came up (source = the headset's own mic) ==="
journalctl -u m17web --since "-2 min" --no-pager 2>/dev/null |
    grep -oE "hfpag/(sink|source): Starting|a2dpsrc/sink: Starting" | sort | uniq -c

echo
echo "=== what the station settled on ==="
timeout 40 python3 tools/wstest.py --port 8080 --cmd st --steps 2 --quiet --state-out /tmp/bt_check.json >/dev/null 2>&1
tr "," "\n" < /tmp/bt_check.json |
    grep -iE '"mode"|"device"|mic_open|spk_open|mic_level|spk_level' | head -8

printf '  USB card holders (empty = every direction is on Bluetooth): '
sudo fuser /dev/snd/pcmC3D0p /dev/snd/pcmC3D0c 2>/dev/null || echo none

echo
echo "=== fallbacks in the last 2 minutes ==="
journalctl -u m17web --since "-2 min" --no-pager 2>/dev/null |
    grep -E "is not available, using" | tail -4 || true
