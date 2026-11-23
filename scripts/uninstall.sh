#!/bin/sh
# SPDX-FileCopyrightText: 2026 Mackie
# SPDX-License-Identifier: LGPL-3.0-or-later
# Remove the plug-in and the daemon.
LABEL=com.mbox3pro.driver.daemon
sudo rm -rf /Library/Audio/Plug-Ins/HAL/Mbox3.driver
sudo launchctl bootout system/$LABEL 2>/dev/null
sudo rm -f /Library/LaunchDaemons/$LABEL.plist /usr/local/libexec/mbox3d
sudo rm -rf /usr/local/share/mbox3pro
sudo killall coreaudiod 2>/dev/null
echo "removed (power-cycle the Mbox 3 Pro to get its standalone routing back)"
