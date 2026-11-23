/* SPDX-FileCopyrightText: 2026 Mackie
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Control server of mbox3d: a tiny HTTP server on 127.0.0.1 that serves the
 * mixer UI (ui/index.html) and a JSON API for the device's settings.
 */
#ifndef MBOX3_CTLSERVER_H
#define MBOX3_CTLSERVER_H

#include "mbox3.h"

#define CTL_PORT 47310

/* Start the server thread. Returns 0 on success. */
int ctl_server_start(unsigned short port);

/* The streaming device the API talks to; NULL when there is none. The call
 * waits until requests in flight are done, so the caller may stop the device
 * right after set(NULL). */
void ctl_server_set_device(mbox3 *m, unsigned rate);

#endif
