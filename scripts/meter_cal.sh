#!/bin/sh
# SPDX-FileCopyrightText: 2026 Mackie
# SPDX-License-Identifier: LGPL-3.0-or-later
# Calibrate the control panel's meters: plays a 1 kHz tone at known levels through the
# playback ring and prints what the device's peak meter reports for DAW 1 (host stream 0).
# Needs the daemon running and build/m3_shm_check (started with sudo: the daemon only accepts root and coreaudiod). No CoreAudio client may be playing.
cd "$(dirname "$0")/.."
for db in 0 -3 -6 -12 -20 -30 -40 -60; do
    sudo ./build/m3_shm_check tone 4 1000 $db >/dev/null 2>&1 &
    sleep 2
    best=0
    for i in 1 2 3 4 5 6 7 8; do
        v=$(curl -s http://127.0.0.1:47310/api/meters | grep -o '\[45088,[0-9]*\]' | head -1 | tr -dc '0-9,' | cut -d, -f2)
        [ -n "$v" ] && [ "$v" -gt "$best" ] && best=$v
        sleep 0.15
    done
    wait
    echo "tone $db dBFS -> peak meter $best (0x$(printf %x $best))"
done
