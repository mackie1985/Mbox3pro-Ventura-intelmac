#!/bin/sh
# SPDX-FileCopyrightText: 2026 Mackie
# SPDX-License-Identifier: LGPL-3.0-or-later
# Install the daemon and/or the Audio Server plug-in.
#   scripts/install.sh daemon    only the LaunchDaemon (test it with build/m3_shm_check)
#   scripts/install.sh driver    only the plug-in
#   scripts/install.sh           both
set -e
cd "$(dirname "$0")/.."
what="${1:-all}"
LABEL=com.mbox3pro.driver.daemon

if [ "$what" = all ] || [ "$what" = daemon ]; then
    [ -x build/mbox3d ] || { echo "build first: make daemon"; exit 1; }
    sudo install -d /usr/local/libexec
    sudo install -m 755 build/mbox3d /usr/local/libexec/mbox3d
    sudo install -d /usr/local/share/mbox3pro/ui
    sudo install -m 644 ui/index.html /usr/local/share/mbox3pro/ui/index.html
    sudo cp scripts/$LABEL.plist /Library/LaunchDaemons/$LABEL.plist
    sudo chown root:wheel /Library/LaunchDaemons/$LABEL.plist
    sudo chmod 644 /Library/LaunchDaemons/$LABEL.plist
    sudo launchctl bootout system/$LABEL 2>/dev/null || true
    # bootout is asynchronous: wait until the old instance is really gone
    for i in 1 2 3 4 5 6 7 8 9 10; do
        launchctl print system/$LABEL >/dev/null 2>&1 || break
        sleep 1
    done
    ok=0
    for i in 1 2 3 4 5; do
        if sudo launchctl bootstrap system /Library/LaunchDaemons/$LABEL.plist; then ok=1; break; fi
        sleep 2
    done
    if [ $ok = 0 ]; then
        echo "bootstrap kept failing; trying the legacy load"
        sudo launchctl load -w /Library/LaunchDaemons/$LABEL.plist
    fi
    echo "daemon installed; log: /var/log/mbox3d.log"
    echo "control panel: http://127.0.0.1:47310/"
fi
if [ "$what" = all ] || [ "$what" = driver ]; then
    [ -d build/Mbox3.driver ] || { echo "build first: make driver"; exit 1; }
    sudo rm -rf /Library/Audio/Plug-Ins/HAL/Mbox3.driver
    sudo cp -R build/Mbox3.driver /Library/Audio/Plug-Ins/HAL/
    sudo chown -R root:wheel /Library/Audio/Plug-Ins/HAL/Mbox3.driver
    sudo killall coreaudiod || true
    echo "plug-in installed; coreaudiod restarted"
fi
