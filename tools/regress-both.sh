#!/bin/sh
# Run the regression suite on both bench stations and summarise.
#
# The addresses are the bench's; a different fleet only needs STATIONS changed.
# Each station runs its own copy of tools/regress.py from its clone, so the
# tests always match the code that station is actually running.
#
#   tools/regress-both.sh            # the fast checks, ~15 s per station
#   tools/regress-both.sh --deep     # every check, ~90 s per station
#
# SSH uses a key if there is one, otherwise the bench password in ~/.pi_pw.
set -u

STATIONS="${M17_STATIONS:-charles@10.168.1.5:m17-pi5 charles@10.168.1.6:m17-pi4}"

SSH="ssh -o ConnectTimeout=8"
if ! ssh -o BatchMode=yes -o ConnectTimeout=5 charles@10.168.1.5 true 2>/dev/null; then
    if command -v sshpass >/dev/null 2>&1 && [ -f "$HOME/.pi_pw" ]; then
        SSHPASS=$(cat "$HOME/.pi_pw"); export SSHPASS
        SSH="sshpass -e ssh -o StrictHostKeyChecking=no -o ConnectTimeout=8"
    fi
fi

fail=0
for entry in $STATIONS; do
    target=${entry%%:*}
    name=${entry##*:}
    echo "################## $name ($target)"
    # shellcheck disable=SC2086
    if $SSH "$target" "python3 ~/m17web-src/tools/regress.py --repo ~/m17web-src $*"; then
        :
    else
        fail=1
    fi
    echo
done

if [ "$fail" -eq 0 ]; then
    echo "== both stations pass"
else
    echo "== at least one station FAILED - see above"
fi
exit "$fail"
