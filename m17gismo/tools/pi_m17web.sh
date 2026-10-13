#!/bin/bash
# tools/pi_m17web.sh - run m17web on the Raspberry Pi (start/stop/status/log/cmd).
#
#   bash tools/pi_m17web.sh start [args...]   start with a test config, log to /tmp/m17web.log
#   bash tools/pi_m17web.sh stop
#   bash tools/pi_m17web.sh status
#   bash tools/pi_m17web.sh log [n]
#   bash tools/pi_m17web.sh cmd "rf 146400000"
#
# Defaults to the simulator (-r sim) and the web UI on port 8080, running from
# ~/m17test where it finds config.txt / M17Hosts.txt.
set -uo pipefail

BIN=${BIN:-/mnt/hf/shared/projects/m17_sdr_web/m17gismo/m17web}
SRCDIR=${SRCDIR:-/mnt/hf/shared/projects/m17_sdr_web/m17gismo}
RUNDIR=${RUNDIR:-$HOME/m17test}
LOG=${LOG:-/tmp/m17web.log}
PIDF=${PIDF:-/tmp/m17web.pid}
PORT=${PORT:-8080}
MODE=${MODE:-radio}

prepare() {
    mkdir -p "$RUNDIR"
    cd "$RUNDIR" || exit 1
    [ -f M17Hosts.txt ] || cp "$SRCDIR/M17Hosts.txt" . 2>/dev/null
    if [ ! -f config.txt ] || [ "${RESET_CONFIG:-0}" = "1" ]; then
        cat > config.txt <<EOF
# m17web test configuration
mode $MODE
tf 146400000
rf 146400000
tg 1.0
rg 0.6
fc 1.0
afc on
sa G4GUO
ba
EOF
    fi
}

case "${1:-status}" in
start)
    shift || true
    prepare
    if pgrep -F "$PIDF" >/dev/null 2>&1; then
        echo "already running (pid $(cat "$PIDF"))"
        exit 0
    fi
    # Make sure nothing else is holding the port (a previous run that ignored
    # SIGTERM would leave the listener behind and the new one would fail to bind)
    if (command -v ss >/dev/null && ss -ltn 2>/dev/null | grep -q ":$PORT "); then
        echo "port $PORT already in use - stopping the old instance first"
        pkill -x "$(basename "$BIN")" 2>/dev/null
        for i in $(seq 1 20); do
            ss -ltn 2>/dev/null | grep -q ":$PORT " || break
            sleep 0.25
        done
        if ss -ltn 2>/dev/null | grep -q ":$PORT "; then
            echo "port $PORT still busy - giving up" >&2
            exit 1
        fi
    fi
    nohup "$BIN" -r sim -w "$PORT" "$@" >"$LOG" 2>&1 &
    echo $! > "$PIDF"
    sleep 2
    if pgrep -F "$PIDF" >/dev/null 2>&1; then
        if grep -q "failed to start the web server" "$LOG"; then
            echo "process started but the web server failed to bind port $PORT:"
            cat "$LOG"
            exit 1
        fi
        echo "started pid $(cat "$PIDF") on port $PORT (log $LOG)"
    else
        echo "FAILED to start; log follows:"
        cat "$LOG"
        exit 1
    fi
    ;;
stop)
    if [ -f "$PIDF" ]; then
        pid=$(cat "$PIDF")
        if kill "$pid" 2>/dev/null; then
            # wait for a clean exit, then insist
            for i in $(seq 1 20); do
                kill -0 "$pid" 2>/dev/null || break
                sleep 0.25
            done
            if kill -0 "$pid" 2>/dev/null; then
                echo "did not exit, sending SIGKILL"
                kill -9 "$pid" 2>/dev/null
                sleep 0.5
            fi
            echo "stopped pid $pid"
        else
            echo "pid $pid was not running"
        fi
    else
        pkill -x "$(basename "$BIN")" && echo "stopped (by name)" || echo "not running"
    fi
    rm -f "$PIDF"
    ;;
status)
    if [ -f "$PIDF" ] && pgrep -F "$PIDF" >/dev/null 2>&1; then
        echo "running: pid $(cat "$PIDF")"
    else
        pkill -0 -f "$BIN" 2>/dev/null && echo "running (no pid file)" || echo "not running"
    fi
    code=$(curl -s -o /dev/null -w "%{http_code}" "http://localhost:$PORT/" 2>/dev/null)
    echo "http://localhost:$PORT/ -> HTTP ${code:-no response}"
    ;;
log)
    tail -n "${2:-40}" "$LOG"
    ;;
cmd)
    shift
    python3 "$SRCDIR/tools/wstest.py" --port "$PORT" --cmd "$*" --steps 2 --quiet
    ;;
*)
    echo "usage: $0 start|stop|status|log [n]|cmd \"command\""
    ;;
esac
