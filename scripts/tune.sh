#!/bin/sh
# SPDX-FileCopyrightText: 2026 Mackie
# SPDX-License-Identifier: LGPL-3.0-or-later
# usage: scripts/tune.sh [PREFILL QUEUE [AHEAD]]   (no arguments = back to the defaults)
#   PREFILL, QUEUE: frames (defaults 768, 512)
#   AHEAD: send look-ahead in slots of 125 us (default 224; more survives longer
#          stalls of the library's IO thread)
# Output latency is about PREFILL + AHEAD * 5.5 + 50 frames.
set -e
if [ $# -eq 0 ]; then
    sudo rm -f /var/db/mbox3d.conf
    echo "defaults restored"
elif [ $# -eq 2 ] || [ $# -eq 3 ]; then
    A=${3:-224}
    printf 'prefill %s\nqueue %s\nahead %s\n' "$1" "$2" "$A" | sudo tee /var/db/mbox3d.conf >/dev/null
    L=$(( $1 + A * 11 / 2 + 50 ))
    echo "prefill $1, queue $2, ahead $A slots (expected output latency about $L frames, $(( L * 1000 / 44100 )) ms)"
else
    echo "usage: $0 [PREFILL QUEUE [AHEAD]]" >&2; exit 2
fi
sudo launchctl kickstart -k system/com.mbox3pro.driver.daemon
