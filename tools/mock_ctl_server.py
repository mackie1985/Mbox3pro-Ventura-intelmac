#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Mackie
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Stand-in for the mbox3d control server (UI development without the hardware):
   python3 tools/mock_ctl_server.py [port]   then open http://127.0.0.1:PORT/"""
import json, math, random, sys, time
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import parse_qs
IN, OUT = 18, 16
mix = [0]*(IN*OUT)
for o in range(8): mix[o*IN+o] = 0x4000
for o in range(8, 12): mix[o*IN+(o & 1)] = 0x4000
P = dict(usecase=0, knob=100, phantom=0, hpf=[0,0,0,0], trim=[255,255,200,200,128,128], rtype=6, rvol=90, rdur=120, rfb=60,
         mute=0, mono=0, spkr=0, dim=0)
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 47311
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, code, body, ctype="application/json"):
        b = body if isinstance(body, bytes) else body.encode()
        self.send_response(code); self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def do_GET(self):
        if self.path in ("/", "/index.html"):
            self.send(200, open("ui/index.html", "rb").read(), "text/html; charset=utf-8")
        elif self.path.startswith("/api/state"):
            self.send(200, json.dumps(dict(online=1, rate=48000, locked=1, under=0, late=0, dbc=0, params=P, sat=0, mixer=mix)))
        elif self.path == "/api/meters":
            t = time.time(); out = []
            def lv(ph, base): return int(max(0, min(4095, 4095*10**((base + 14*math.sin(t*ph) - 14)/20) * (0.6+0.4*random.random()))))
            for k in range(8): out.append([((0xb0+k) << 8) | (0x20+k), lv(1.3+k*.4, -18)])
            for k in range(6): out.append([((0x40+k) << 8) | (0xb0+k), lv(0.9+k*.7, -14 if k < 4 else -30)])
            for o in range(8): out.append([((0x20+o) << 8) | (0x40+o), lv(1.1+o*.3, -12)])
            self.send(200, json.dumps(out))
        else: self.send(404, "nf", "text/plain")
    def do_POST(self):
        if not self.headers.get("X-Mbox3"): return self.send(403, "x", "text/plain")
        q = parse_qs(self.rfile.read(int(self.headers.get("Content-Length", 0))).decode())
        if self.path == "/api/mixer":
            for c in q["c"][0].split(";"):
                o, i, v = map(int, c.split(",")); mix[o*IN+i] = v
        elif self.path == "/api/param":
            n, i, v = q["name"][0], int(q.get("i", ["0"])[0]), int(q["v"][0])
            if n in ("hpf", "trim"): P[n][i] = v
            else: P[{"rvol":"rvol","rdur":"rdur","rfb":"rfb","rtype":"rtype","phantom":"phantom"}[n]] = v
        self.send(200, "ok", "text/plain")
HTTPServer(("127.0.0.1", PORT), H).serve_forever()
