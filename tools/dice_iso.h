/* SPDX-FileCopyrightText: 2026 Mackie
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Isochronous receive test: allocate an iso channel, point the device's TX
 * stream at it, enable streaming, and report what arrives.
 */
#ifndef DICE_ISO_H
#define DICE_ISO_H

/* Runs for `secs` seconds. Returns 0 on success. The caller must own the
 * device and have set the clock/router already. */
int dice_iso_capture(unsigned secs);

/* Playback test: send a sine tone on the RX stream (host -> device) for `secs`
 * seconds. `latency` is the SYT presentation offset in bus cycles (125 us),
 * `freq` the tone frequency in Hz. Route Avs0 -> outputs beforehand. */
int dice_iso_play(unsigned secs, unsigned latency, unsigned freq);

/* Capture and playback together (loopback check, see dice_iso.c). If
 * clock_src >= 0 the device clock source is switched to it (e.g. 0x08 = ARX1)
 * one second after streaming starts. */
int dice_iso_duplex(unsigned secs, unsigned latency, unsigned freq, int clock_src);

#endif
