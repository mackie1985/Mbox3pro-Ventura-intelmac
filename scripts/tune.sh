#!/bin/sh
# SPDX-FileCopyrightText: 2026 Mackie
# SPDX-License-Identifier: LGPL-3.0-or-later
# usage: scripts/tune.sh [PREFILL QUEUE [AHEAD [SLACK]]]   (no arguments = back to the defaults)
#   PREFILL, QUEUE: frames (defaults 384, 320)
#   AHEAD: send look-ahead in slots of 125 us (default 64; more survives longer
#          stalls of the library's IO thread)
# The older, more cautious values are 768 512 224 (about 1500 frames more output latency).
#   SLACK: extra frames the output write cursor keeps ahead of the daemon (default 128; less = lower latency,
#          too little = crackles)
# Output latency is about PREFILL + AHEAD * 5.5 + 50 frames.
set -e
if [ $# -eq 0 ]; then
    sudo rm -f /var/db/mbox3d.conf
    echo "defaults restored"
elif [ $# -ge 2 ] && [ $# -le 4 ]; then
    A=${3:-64}
    S=${4:-128}
    printf 'prefill %s\nqueue %s\nahead %s\nslack %s\n' "$1" "$2" "$A" "$S" | sudo tee /var/db/mbox3d.conf >/dev/null
    L=$(( $1 + A * 11 / 2 + 50 ))
    echo "prefill $1, queue $2, ahead $A slots (expected output latency about $L frames, $(( L * 1000 / 44100 )) ms)"
else
    echo "usage: $0 [PREFILL QUEUE [AHEAD [SLACK]]]" >&2; exit 2
fi
sudo launchctl kickstart -k system/com.mbox3pro.driver.daemon
