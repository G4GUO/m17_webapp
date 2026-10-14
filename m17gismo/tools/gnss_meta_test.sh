#!/bin/sh
# Does "gps meta on|off" really control whether our position leaves the station?
#
# Starts an instance on a simulated radio, which loops its own transmission back
# to itself, then reads the position it hears announced in the META field it just
# sent.  Each phase gets a FRESH instance and so a fresh receiver: a receiver that
# has already been told a position keeps showing it, so reusing one would report
# a stale value and could hide a switch that does not work.
#
# Each phase runs in its own directory, so the instance keeps its own config.txt
# and cannot disturb a running service.
#
#   tools/gnss_meta_test.sh                          # the receiver it finds
#   M17WEB_GPS=/dev/... tools/gnss_meta_test.sh      # or a particular one
#
# Exit status 0 means the switch works: position sent when on, nothing sent when
# off, and the receiver itself keeps running in both cases.

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
BIN="$HERE/../m17web"
PORT=${PORT:-8086}
WAIT_TX=${WAIT_TX:-7}
DIR=${TMPDIR:-/tmp}/gnss_meta_test.$$

[ -x "$BIN" ] || { echo "no binary at $BIN - run make first"; exit 2; }
mkdir -p "$DIR" || exit 2
OURS=""
cleanup() { [ -n "$OURS" ] && kill "$OURS" 2>/dev/null; sleep 1; rm -rf "$DIR"; }
trap cleanup EXIT INT TERM

# Find an instance by its command line, never by name alone, so a running service
# is never mistaken for the test instance or killed.
find_ours() {
    OURS=""
    for p in $(pgrep -x m17web); do
        grep -qa "$PORT" /proc/$p/cmdline 2>/dev/null && OURS=$p
    done
}

ask() { python3 "$HERE/wstest.py" --port "$PORT" --cmd "$1" --duration 1 >/dev/null 2>&1; sleep "$2"; }

run_phase() {
    MODE=$1
    PHRASE="$DIR/$MODE"
    rm -rf "$PHRASE"; mkdir -p "$PHRASE" || return 2
    ( cd "$PHRASE" && setsid "$BIN" -r sim -a none -w "$PORT" \
        > run.log 2>&1 < /dev/null & ) || return 2
    sleep 8
    find_ours
    [ -n "$OURS" ] || { echo "  meta=$MODE FAIL: the test instance did not start"; tail -5 "$PHRASE/run.log"; return 2; }
    ask "sa G4GUO" 2
    ask "gps meta $MODE" 1
    ask "td" "$WAIT_TX"
    python3 "$HERE/wstest.py" --port "$PORT" --steps 1 --duration 4 \
        --state-out "$DIR/$MODE.json" >/dev/null 2>&1
    kill "$OURS" 2>/dev/null; sleep 3; OURS=""
    return 0
}

RC=0
for MODE in on off; do
    run_phase "$MODE" || RC=1
    python3 - "$DIR/$MODE.json" "$MODE" <<'PYTHON' || RC=1
import json, sys
try:
    s = json.load(open(sys.argv[1]))
except Exception as e:
    print("  meta=%-3s FAIL: no usable state from the station (%s)" % (sys.argv[2], e)); sys.exit(1)
g = s.get("gps") or {}
heard = s.get("heard") or {}
position = heard.get("grid") or ""
if sys.argv[2] == "on":
    ok = bool(position) and g.get("tx") is True
    print("  meta=on   announced: %-5s  received back: %-7s  fix: %-5s -> %s"
          % (g.get("tx"), position or "no", g.get("valid"), "PASS" if ok else "FAIL"))
else:
    ok = (not position) and g.get("valid") is True
    print("  meta=off  announced: %-5s  received back: %-7s  fix: %-5s -> %s"
          % (g.get("tx"), position or "no", g.get("valid"),
             "PASS (receiver still runs, nothing sent)" if ok else "FAIL"))
sys.exit(0 if ok else 1)
PYTHON
done
[ "$RC" -eq 0 ] && echo "gnss meta switch: OK" || echo "gnss meta switch: FAILED"
exit "$RC"
