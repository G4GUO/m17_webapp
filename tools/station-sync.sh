#!/bin/sh
# Put a station on the definitive repo, build it, install it, restart it.
#
#   tools/station-sync.sh [--force] [--no-build] [--check]
#
# The rule this exists to enforce: a station's source is a *clone* of
# G4GUO/m17_webapp, never a hand-edited copy.  Everything that used to drift -
# the web page, the audio path, the reflector state, the AX.25 port - is in that
# repo now, so a station either matches it exactly or it does not build.
#
# What it does, in order:
#   1. clone the repo if this station has no clone yet
#   2. refuse to throw away local edits, unless --force
#   3. reset the clone to origin/master
#   4. build with the flags this hardware needs
#   5. install the binary where the systemd unit starts it, and the web page
#      where the service serves it from
#   6. restart, then verify the served page and the source both report the same
#      commit, and that the www files are byte-identical to the repo
#
# Environment (defaults suit both m17web stations):
#   REPO_URL     https://github.com/G4GUO/m17_webapp.git
#   SRC          ~/m17web-src            the clone
#   SERVICE_DIR  ~/m17web                the unit's WorkingDirectory (www/, M17Hosts.txt)
#   BIN_DEST     unset = build in place  where ExecStart points
#   BUILD_FLAGS  "SIM=1 CODEC2_BACKEND=mod"
#   UNIT         m17web

set -eu

FORCE=0
BUILD=1
CHECK_ONLY=0
for a in "$@"; do
    case "$a" in
        --force)     FORCE=1 ;;
        --no-build)  BUILD=0 ;;
        --check)     CHECK_ONLY=1 ;;
        *) echo "unknown option: $a" >&2; exit 2 ;;
    esac
done

REPO_URL=${REPO_URL:-https://github.com/G4GUO/m17_webapp.git}
SRC=${SRC:-$HOME/m17web-src}
SERVICE_DIR=${SERVICE_DIR:-$HOME/m17web}
BIN_DEST=${BIN_DEST:-}
BUILD_FLAGS=${BUILD_FLAGS:-SIM=1 CODEC2_BACKEND=mod}
UNIT=${UNIT:-m17web}

say() { printf '%s\n' "$*"; }
die() { printf '%s\n' "station-sync: $*" >&2; exit 1; }

# ---------------------------------------------------------------- the clone
if [ "$CHECK_ONLY" -eq 0 ]; then
    if [ ! -d "$SRC/.git" ]; then
        say "== no clone at $SRC, cloning $REPO_URL"
        mkdir -p "$(dirname "$SRC")"
        git clone -q "$REPO_URL" "$SRC"
    fi

    cd "$SRC"
    git remote set-url origin "$REPO_URL"
    git fetch -q origin --prune

    # Local edits mean somebody worked on this box by hand.  That is exactly how
    # the two stations diverged, so it stops here unless asked to overwrite.
    if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
        if [ "$FORCE" -eq 1 ]; then
            say "== discarding local edits (--force):"
            git status --short --untracked-files=no | sed 's/^/     /'
            git checkout -q -- .
        else
            say "== local edits on this station, refusing to overwrite:"
            git status --short --untracked-files=no | sed 's/^/     /'
            die "commit them into the repo, or re-run with --force to lose them"
        fi
    fi

    BRANCH=$(git rev-parse --abbrev-ref HEAD)
    if [ "$BRANCH" != "master" ]; then
        say "== was on $BRANCH, switching to master"
        git checkout -q master
    fi
    git reset -q --hard origin/master
    say "== source at $(git log --oneline -1)"
fi

cd "$SRC"
COMMIT=$(git rev-parse --short HEAD)

# ------------------------------------------------------------------ the build
if [ "$BUILD" -eq 1 ] && [ "$CHECK_ONLY" -eq 0 ]; then
    say "== building: make -j$(nproc) $BUILD_FLAGS"
    ( cd "$SRC/m17gismo" && make -j"$(nproc)" $BUILD_FLAGS ) >"$SRC/build.log" 2>&1 \
        || { tail -20 "$SRC/build.log"; die "build failed, see $SRC/build.log"; }
    [ -x "$SRC/m17gismo/m17web" ] || die "build produced no m17web"
    say "== built $(stat -c%s "$SRC/m17gismo/m17web") bytes"
fi

# --------------------------------------------------------------- the install
if [ "$CHECK_ONLY" -eq 0 ]; then
    if [ -n "$BIN_DEST" ]; then
        say "== installing the binary at $BIN_DEST"
        install -m 755 "$SRC/m17gismo/m17web" "$BIN_DEST"
    fi
    if [ -d "$SERVICE_DIR/www" ]; then
        say "== installing the web page into $SERVICE_DIR/www"
        cp -f "$SRC"/m17gismo/www/* "$SERVICE_DIR/www/"
    fi
    say "== restarting $UNIT"
    sudo systemctl restart "$UNIT"
fi

# ----------------------------------------------------------------- the checks
# This script lives in the tree it just reset, so the copy running now may be the
# previous version - `sh` reads a script as it goes, and the reset rewrote the
# file underneath it (9 Nov: an old single-shot HTTP check failed a restart that
# was fine).  Hand over to the version that is now checked out.
if [ "$CHECK_ONLY" -eq 0 ]; then
    SELF=$(readlink -f "$0" 2>/dev/null || printf '%s' "$0")
    NEW="$SRC/tools/station-sync.sh"
    if [ -f "$NEW" ] && ! cmp -s "$SELF" "$NEW"; then
        say "== the checked-out script is newer than the one running, re-running it"
        exec sh "$NEW" --check
    fi
fi

FAIL=0
if [ -d "$SERVICE_DIR/www" ]; then
    for f in "$SRC"/m17gismo/www/*; do
        b=$(basename "$f")
        [ -f "$SERVICE_DIR/www/$b" ] || { say "   MISSING  $b"; FAIL=1; continue; }
        cmp -s "$f" "$SERVICE_DIR/www/$b" || { say "   DIFFERS  $b"; FAIL=1; }
    done
    [ "$FAIL" -eq 0 ] && say "== served www matches the repo, file for file"
fi
[ "$FAIL" -eq 0 ] || die "the served page does not match the repo"

if [ -n "$BIN_DEST" ]; then
    cmp -s "$SRC/m17gismo/m17web" "$BIN_DEST" || die "$BIN_DEST is not the binary just built"
    say "== installed binary is the one just built"
fi

if command -v curl >/dev/null 2>&1; then
    # The unit takes a few seconds to open the radio and the web port, so give it
    # time instead of declaring failure on the first connection refused.
    i=0
    code=000
    while [ "$i" -lt 45 ]; do
        code=$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:8080/ || true)
        [ "$code" = "200" ] && break
        i=$((i + 1)); sleep 1
    done
    say "== http://127.0.0.1:8080/ -> $code (after ${i}s)"
    [ "$code" = "200" ] || die "the panel did not come back after the restart"
fi

say "== $(hostname) is on $COMMIT, in step with the repo"
