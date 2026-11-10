#!/bin/sh
# Does the position really keep the META field while there is a fix, and does the
# text take over the moment the position is switched off?
#
# A pty plays the part of the GPS (tools/gps_feed.py), so this can run anywhere,
# with no receiver: the station sees a genuine fix at IO90ST.  Two transmissions
# are keyed on a simulated radio that loops back to itself:
#
#   1. a fix is present and a text is set -> the position goes, the text does not
#   2. the position is switched off        -> the text goes
#
# `td` is a duplex PTT, so each transmission has to be ended with `rx` before the
# next one is keyed - otherwise the second `td` is a no-op on a transmission that
# is already running, with the payload kind it latched when it started.
#
#   tools/meta_precedence_test.sh
#
# Exit status 0 means the position wins while it can, and the text goes when it
# cannot.  The blank case is covered by tools/meta_text_test.sh.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
BIN="$HERE/../m17web"
PORT=${PORT:-8089}
DIR=${TMPDIR:-/tmp}/meta_prec.$$
PHRASE="Bench precedence"

[ -x "$BIN" ] || { echo "no binary at $BIN - run make first"; exit 2; }
mkdir -p "$DIR" || exit 2
OURS=""
FEED=""
cleanup() {
    [ -n "$OURS" ] && kill "$OURS" 2>/dev/null
    [ -n "$FEED" ] && kill "$FEED" 2>/dev/null
    sleep 1
    rm -rf "$DIR"
}
trap cleanup EXIT INT TERM

find_ours() {
    OURS=""
    for p in $(pgrep -x m17web); do
        grep -qa "$PORT" /proc/$p/cmdline 2>/dev/null && OURS=$p
    done
}

python3 "$HERE/gps_feed.py" > "$DIR/gpsdev.txt" 2>&1 &
FEED=$!
sleep 1
DEV=$(head -1 "$DIR/gpsdev.txt")
[ -e "$DEV" ] || { echo "no gps pty (see $DIR/gpsdev.txt)"; exit 2; }
echo "  gps pty: $DEV"

( cd "$DIR" && setsid "$BIN" -r sim -a none -w "$PORT" > run.log 2>&1 < /dev/null & ) || exit 2
sleep 8
find_ours
[ -n "$OURS" ] || { echo "  precedence FAIL: the test instance did not start"; tail -5 "$DIR/run.log"; exit 2; }

say() { python3 "$HERE/wstest.py" --port "$PORT" --cmd "$1" --duration 1 >/dev/null 2>&1; }
state() { python3 "$HERE/wstest.py" --port "$PORT" --steps 1 --duration 3 --state-out "$1" >/dev/null 2>&1; }

say "sa G4GUO"
say "gps $DEV 38400"
sleep 6
state "$DIR/s1.json"                 # a fix, before anything is transmitted
say "meta text $PHRASE"
sleep 1
say "td"
sleep 9
state "$DIR/s2.json"                 # fix present: the position must have gone
say "rx"
sleep 2
say "gps meta off"
sleep 1
say "td"
sleep 9
state "$DIR/s3.json"                 # announcing off: the text must go
say "rx"
kill "$FEED" 2>/dev/null; FEED=""

python3 - "$DIR" "$PHRASE" <<'PY'
import json, sys
d, phrase = sys.argv[1], sys.argv[2]

def get(name):
    try:
        return json.load(open("%s/%s" % (d, name)))
    except Exception:
        return {}

s1, s2, s3 = get("s1.json"), get("s2.json"), get("s3.json")
g1 = s1.get("gps") or {}
fix_ok = g1.get("valid") is True
print("  fix:        valid=%s sats=%s locator=%s" % (g1.get("valid"), g1.get("sats"), g1.get("grid")))
g2, m2 = s2.get("gps") or {}, s2.get("mtext") or {}
g3, m3 = s3.get("gps") or {}, s3.get("mtext") or {}
pos_ok = (g2.get("tx") is True) and (m2.get("text") or "") == ""
txt_ok = (g3.get("tx") is False) and (m3.get("text") or "") == phrase
print("  fix+text:   position in META=%-5s text heard=%-18r -> %s"
      % (g2.get("tx"), m2.get("text"), "PASS" if pos_ok else "FAIL"))
print("  text only:  position in META=%-5s text heard=%-18r -> %s"
      % (g3.get("tx"), m3.get("text"), "PASS" if txt_ok else "FAIL"))
if not fix_ok:
    print("  fix:        FAIL - the fake receiver never produced a usable fix")
sys.exit(0 if (fix_ok and pos_ok and txt_ok) else 1)
PY
RC=$?
[ "$RC" -eq 0 ] && echo "META precedence: OK" || echo "META precedence: FAILED"
exit "$RC"
