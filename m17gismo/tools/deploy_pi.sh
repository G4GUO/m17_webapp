#!/bin/bash
# tools/deploy_pi.sh - sync m17web sources to the Raspberry Pi and build.
#
# Run from anywhere on the Hermes host:   bash tools/deploy_pi.sh [make-target]
# Default make target is "sim" (web UI + simulated radio, no SDR hardware).
#
# The canonical sources live in /mnt/hf/shared/projects/m17_sdr_web (visible to
# Windows as Z:\shared\projects\m17_sdr_web).  The Pi has the same share at
# /mnt/hf, but building on local disk is quicker and avoids NFS permission
# surprises, so the sources are copied to ~/m17web first.
set -euo pipefail

PI=${PI:-charles@10.168.1.33}
SRC=${SRC:-/mnt/hf/shared/projects/m17_sdr_web/m17gismo}
DEST=${DEST:-m17web}
TARGET=${1:-sim}
PW_FILE=${PW_FILE:-$HOME/.pi_pw}

SSH_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
 -o PreferredAuthentications=password -o PubkeyAuthentication=no -o LogLevel=ERROR"

remote() { sshpass -f "$PW_FILE" ssh $SSH_OPTS "$PI" "$@"; }

echo "== syncing $SRC -> $PI:~/$DEST"
sshpass -f "$PW_FILE" ssh $SSH_OPTS "$PI" "mkdir -p ~/$DEST"
sshpass -f "$PW_FILE" rsync -az --delete \
    --exclude '*.o' --exclude 'm17web' --exclude 'www_assets.h' --exclude '.git' \
    -e "ssh $SSH_OPTS" "$SRC/" "$PI:~/$DEST/" 2>/dev/null || {
        echo "-- rsync unavailable or failed, falling back to tar over ssh"
        tar czf - -C "$SRC" --exclude='*.o' --exclude=m17web --exclude=www_assets.h . \
          | sshpass -f "$PW_FILE" ssh $SSH_OPTS "$PI" "mkdir -p ~/$DEST && tar xzf - -C ~/$DEST"
    }

echo "== building ($TARGET)"
remote "cd ~/$DEST && make clean >/dev/null 2>&1; make $TARGET 2>&1 | tail -25; ls -l m17web 2>/dev/null"
