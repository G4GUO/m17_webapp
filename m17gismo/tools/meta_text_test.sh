#!/bin/sh
# Does the META text really leave the station in the META field, and does it
# really stay off when it is cleared?
#
# Starts an instance on a simulated radio, which loops its own transmission back
# to itself, then reads back the text it heard announced in the META field it
# just sent.  The position is left announcing, because the text has to win the
# field while it is set: set switches the text on, clear switches it off and
# hands the field back to the position.  Only one payload fits in META at a time.
#
# Three phases, each on a FRESH instance and so a fresh receiver:
#   set     the text is set from the command line, and must be heard back
#   seeded  the text is only in config.txt, and must be heard back after a start
#   off     nothing is set, and nothing may be heard
#
#   tools/meta_text_test.sh
#
# Exit status 0 means the text goes out when set, comes back from config.txt, and
# nothing goes out when cleared.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
BIN="$HERE/../m17web"
PORT=${PORT:-8087}
WAIT_TX=${WAIT_TX:-9}
DIR=${TMPDIR:-/tmp}/meta_text_test.$$
PHRASE="The quick brown fox jumps over the lazy dog"

[ -x "$BIN" ] || { echo "no binary at $BIN - run make first"; exit 2; }
mkdir -p "$DIR" || exit 2
OURS=""
cleanup() { [ -n "$OURS" ] && kill "$OURS" 2>/dev/null; sleep 1; rm -rf "$DIR"; }
trap cleanup EXIT INT TERM

find_ours() {
    OURS=""
    for p in $(pgrep -x m17web); do
        grep -qa "$PORT" /proc/$p/cmdline 2>/dev/null && OURS=$p
    done
}

ask() { python3 "$HERE/wstest.py" --port "$PORT" --cmd "$1" --duration 1 >/dev/null 2>&1; sleep "$2"; }

run_phase() {
    MODE=$1
    WORK="$DIR/$MODE"
    rm -rf "$WORK"; mkdir -p "$WORK" || return 2
    # "seeded" starts with the text already in config.txt and never sets it from
    # the command line: that is the path a station reboot takes.
    if [ "$MODE" = "seeded" ]; then
        printf 'meta text %s\n' "$PHRASE" > "$WORK/config.txt"
    fi
    ( cd "$WORK" && setsid "$BIN" -r sim -a none -w "$PORT" \
        > run.log 2>&1 < /dev/null & ) || return 2
    sleep 8
    find_ours
    [ -n "$OURS" ] || { echo "  text=$MODE FAIL: the test instance did not start"; tail -5 "$WORK/run.log"; return 2; }
    ask "sa G4GUO" 2
    case "$MODE" in
        set)    ask "meta text $PHRASE" 1 ;;
        seeded) : ;;                      # config.txt already said it
        off)    ask "meta text off" 1 ;;
    esac
    ask "td" "$WAIT_TX"
    python3 "$HERE/wstest.py" --port "$PORT" --steps 1 --duration 4 \
        --state-out "$DIR/$MODE.json" >/dev/null 2>&1
    kill "$OURS" 2>/dev/null; sleep 3; OURS=""
    return 0
}

RC=0
for MODE in set seeded off; do
    run_phase "$MODE" || RC=1
    python3 - "$DIR/$MODE.json" "$MODE" "$PHRASE" "$DIR/$MODE" <<'PYTHON' || RC=1
import json, os, sys
path, mode, phrase, work = sys.argv[1:5]
try:
    s = json.load(open(path))
except Exception as e:
    print("  text=%-6s FAIL: no usable state from the station (%s)" % (mode, e)); sys.exit(1)
m = s.get("mtext") or {}
heard, src, sent = m.get("text") or "", m.get("src") or "", m.get("send") or ""
if mode == "off":
    ok = (not heard) and (not sent)
    print("  text=off    set: %-5s heard back: %-8s -> %s"
          % (bool(sent), repr(heard), "PASS (receiver still runs, nothing sent)" if ok else "FAIL"))
    sys.exit(0 if ok else 1)
# 43 characters is four blocks, so this also proves the round robin and the
# space padding of the last block.
ok = (heard == phrase) and (src == "G4GUO") and (sent == phrase)
# The console has to show it too, not just the page: that is what an operator
# watching the terminal sees.
try:
    log = open(os.path.join(work, "run.log"), errors="replace").read()
except OSError:
    log = ""
logged = ("META text from G4GUO: " + phrase) in log
if mode == "set":
    # Setting it from the command line has to be remembered for the next start.
    try:
        cfg = open(os.path.join(work, "config.txt"), errors="replace").read()
    except OSError:
        cfg = ""
    if ("meta text " + phrase) not in cfg:
        ok = False
        print("  text=set    FAIL: config.txt does not carry the line")
print("  text=%-6s heard back from %-5s: %s  console: %s -> %s"
      % (mode, src or "nobody", repr(heard), "yes" if logged else "no",
         "PASS" if ok and logged else "FAIL"))
sys.exit(0 if (ok and logged) else 1)
PYTHON
done
[ "$RC" -eq 0 ] && echo "META text: OK" || echo "META text: FAILED"
exit "$RC"
