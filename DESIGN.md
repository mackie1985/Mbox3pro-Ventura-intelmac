# Avid Mbox 3 Pro — driver design for macOS 13 (Ventura) / Intel

Goal: use the Mbox 3 Pro (FireWire) as a CoreAudio device on an Intel Mac running Ventura, including control of the
mixer, routing and preamps.

> This document is the engineering log of the project. The early sections are the original plan and the register
> findings; later sections record what was measured on real hardware and what was changed because of it. Where an early
> section and a later one disagree, the later one wins. The Japanese original is kept as `DESIGN.ja.md`.

## Background (research results)
- Ventura removed Apple's FireWire Core Audio driver (AppleFWAudio). The FireWire bus itself (IOFireWireFamily) is
  still present in Ventura (it disappears in macOS 26).
- The Mbox 3 Pro is a TCAT DICE FireWire device, supported by Linux snd-dice. The control protocol has been reverse
  engineered (firewire-dice-protocols, LGPL-3.0+).
- Using IOFireWireLib from user space most likely avoids loading an unsigned kext or relaxing system security.

## Architecture (three layers, original plan)
```
 CoreAudio (coreaudiod)
   └─ Audio Server Plug-in (.driver)      … exposes device/streams, low-latency ring buffer
        ├─ iso engine                      … isoch RX/TX through IOFireWireLib (AM824/IEC61883-6)
        └─ DICE/Mbox3 control              … asynchronous read/write, notifications
 control app (menu bar or CLI)             … mixer / routing / preamp UI
```
- `src/mbox3_params.[ch]` : serialisation of the application section (implemented and tested)
- `src/fw_transport.h`    : boundary of the OS-dependent part (the IOFireWireLib implementation was not started here)

The architecture that was finally built differs: the plug-in only talks to a LaunchDaemon (`mbox3d`) through XPC and
shared memory, and the daemon owns the device (see the later sections).

## Known internals of the Mbox 3 Pro
- Router: 34 inputs x 42 outputs, up to 128 entries. Mixer: 18 inputs x 16 outputs. One reverb DSP.
- Inputs: analog 1-6, SPDIF 1/2, reverb return, 8 stream channels (PC -> device).
- Outputs: analog 1-6, headphones 1/2 and 3/4, SPDIF, 8 stream channels (device -> PC), control room.
- Application section (implemented): use case, master knob, mute/mono/SPKR LEDs, dim, phantom power, HPF,
  output trim (wire value = 255 - trim), reverb (type/volume/duration/feedback).

## Facts confirmed on hardware (Mbox 3 Pro, FireWire 400, Intel Mac / output of dice_probe)
- The DICE address space is 0xffff_e000_0000. The offsets/sizes in the section table are in units of 4 bytes (multiply by 4).
  global=0x28, tx=0x190, rx=0x3c8, ext_sync=0x830. Version 0x01000400.
- The TCAT extension sections start at DICE space +0x00200000. The table has 9 entries of (offset,size) in units of 4 bytes:
  caps, command, mixer, peak, router, stream_format, current_config, standalone, application.
- Strings (nickname, channel names, clock names) have their byte order reversed in every group of 4 bytes.
- Clock source names: index 0 = SPDIF, 7 = Word Clock, 12 = Internal (matches the low byte of the clock select register).
- caps (+0x00 router, +0x04 mixer, +0x08 general):
  - router 0x00800001 -> exposed, writable, up to 128 entries
  - mixer 0x10120221 -> exposed, writable, 18 inputs x 16 outputs
  - general 0x0002022f -> dynamic stream format, peak meters present (the meaning of the other bits is unknown)
- The mixer section can only be read in 4-byte units (block reads are rejected with an "address error").
  The coefficients are laid out as "18 inputs per output" (index = out*18 + in), 0x4000 presumed to be 0 dB.
  Non-zero at that time: out6<-in0, out7<-in1 (S/PDIF output).
- Router entry = (src << 8) | dst. Each byte = block ID << 4 | channel.
  src: AES=0 ADAT=1 Mixer=2 Ins0=4 Ins1=5 ARM=6 Avs0=0xb Avs1=0xc Mute=0xf
  dst: AES=0 ADAT=1 MixerTx0=2 MixerTx1=3 Ins0=4 Ins1=5 ARM=6 Avs0=0xb Avs1=0xc
- current_config: per low/mid/high rate, a [router table @0x0000/0x2000/0x4000] and a [stream configuration @0x1000/0x3000/0x5000].
  Stream configuration = [number of tx][number of rx] + 0x10c bytes each ([pcm][midi][name 256B][ac3 caps]).
  All rate classes: tx 8 ch (Mic-Line 1-4, Aux L/R, SPDIF L/R) / rx 8 ch (Line 1-6, SPDIF L/R), 2 MIDI each.
- The live router section is empty (0 entries). The entries of the peak section carry the router format in the low
  16 bits (the level is in the high bits).
- Default routing (current_config): 6 input channels + SPDIF -> 8 stream outputs / 8 stream channels + inputs + reverb
  return -> 18 mixer inputs / mixer outputs 0-5 -> analog outputs, 6-7 -> SPDIF, 8-11 -> headphones / stream 1-2 -> reverb.
- +0x04 of the application section may change from run to run (0x80 was observed). Purpose unknown.
- Writing: a 4-byte block write to the application section (reverb volume) was changed, read back and restored successfully.
- TX entry = [iso ch (0xffffffff = unused)][pcm][midi][speed] + name. RX entry = [iso ch][seq start][pcm][midi] + name.
- ext_sync = [clock source 0x0c][lock 1][rate 1 (44.1k)][ADAT user data].
- The peak section appears to reflect the routing that is running (standalone operation). The live router section is empty.
- Not yet confirmed: the procedure (opcode) of the command section for writing and activating the router.
- Mixer: the layout is out*18+in (confirmed on hardware). Coefficients are linear amplitude, 0x4000 = 0 dB.
  In standalone mode the firmware manages the mixer itself and rewrites the coefficients every time the master knob is
  moved (use case AD/DA: out0<-in6, out1<-in7 = S/PDIF input -> analog output, the coefficient follows the knob value.
  Knob 30 -> 0x0080, 226 -> 0x2e6e: 0.2 dB per step, 240 presumed to be 0 dB). Cells added by the host disappear when
  the knob is turned.
- A plain block write to the owner register (global +0x00, 8 bytes) is rejected with an "address error". It seems to
  have to be taken with CompareSwap (lock). The node ID of this Mac is 0xffc1 (IOFireWireLocalNode in the IORegistry).
- The value of a lock (CompareSwap64) must be passed as a buffer holding the bytes exactly as they travel on the bus
  (passing it as a number makes the comparison fail with 0xe00002cc).
- The maximum of the master knob is 255, and the coefficient then saturates at 0x4000 (0 dB).
- Command section: +0x00 opcode, +0x04 return value. Opcode = 0x80000000 (execute) | rate flag (low 0x10000 / mid 0x20000 /
  high 0x40000) | operation (1 = load router, 2 = load stream configuration, 3 = both, 4 = load from flash, 5 = save to flash).
  Poll every 50 ms until the execute bit drops, then read the return value. The flash operations (4, 5) are not implemented.
- Once the host is the owner, the router table the device had stored (40 entries in current_config) looks like 0 entries.
  LoadRouter (low rate) then completes with return value 0, but the live router stays at 0 entries.
- The owner registration is kept within one process (observed for 5 s) and is dropped when the process exits.
- Unresolved: the discrepancy between the clock select (which says 48k) and the status (44.1k). stream_format is all 0 when unset.

## Phase plan (original)
1. **Probe tool (next)**: a CLI that reads the config ROM and the DICE global section. The first real test of talking to the device.
2. **Control**: read/write of the TCAT extension sections (router/mixer) and notification handling. A control app can be built from here.
3. **Streaming**: isoch RX/TX, clock synchronisation, sample-rate switching (44.1-96 kHz).
4. **Audio Server Plug-in**: exposure to CoreAudio, drift correction, verification in a DAW.

## Open items and risks (original)
- The register layout of the TCAT extension sections (router/mixer coefficients) was not yet imported. It has to be
  implemented by reading `tcat::extension` / `tcd22xx_spec` of firewire-dice-protocols.
- The base of the DICE address space (said to be 0xffffe0000000) and the section table must be confirmed on hardware.
- Latency/jitter of user-space isoch: the isoch ports of IOFireWireLib are old, and whether low latency (buffers of
  64-128 frames) can be reached has to be measured on hardware.
- Differences between controllers behind a Thunderbolt -> FireWire adapter (behaviour can change with the connection
  topology even on Intel Macs).
- The encoding of `OrangeBlink` is identical to Red in the reference implementation, which looks like a typo. To be checked on hardware.
- The machine this was written on is Linux, so the macOS-specific parts (IOKit/CoreAudio) cannot be built or run there.

## License
The protocol knowledge is based on analysis work released under LGPL-3.0-or-later, so this project is also LGPL-3.0-or-later.

## Hardware notes (behaviour with the host as owner, successful iso reception)
- When the host becomes the owner, the firmware drops its own (standalone) routing and the router becomes empty.
  In addition, **the output stage stays stopped until streaming starts** (mixer outputs and direct paths are both silent,
  no peaks either). Releasing ownership does not restore it until the device is power-cycled.
- Conditions for sound: own -> clock setting (internal 44.1k) -> router write + LoadRouter -> stream_format write +
  LoadStreamConfig -> allocate an iso channel -> write the iso channel into the tx section -> enable=1. Enabling without
  assigning a channel stops the input as well.
- The firmware's router in standalone mode (22 entries; the paths show up in the peak section):
  Ins0:0-5->MixerTx0:0-5, AES:0-1->MixerTx0:6-7, Mixer:0-5->Ins0:0-5, Mixer:6-7->AES:0-1, Mixer:0/1->Ins1:0-5 (headphones).
  Mixer inputs 0-5 = analog inputs 1-6, 6-7 = S/PDIF inputs. The default table for the host (40 entries in
  current_config) is a different wiring that assumes the PC streams.
- Peak section: in the same order as the router table, [level 16 bit | path 16 bit]. Beyond the table it is stale memory (ignore it).
- The stream_format section is empty while owned. The stream configuration the device keeps (current_config
  0x1000/0x3000/0x5000) has to be read before taking ownership and copied over (it reads as 0 afterwards). One stream
  each for tx and rx (8 ch + MIDI 2).
- Iso reception with IOFireWireLib: CreateIsochChannel (IRM present, 512 B, S400) + RemoteIsochPort (talker; the device-side
  handler writes the iso ch of tx and the enable) + LocalIsochPort (NuDCL receive ring of 512 packets) +
  SetTalker/AddListener/AllocateChannel/Start works.
  Received packet: 4 B iso header + 2 CIP quadlets + 8 blocks x 9 quadlets (PCM 8 + MIDI 1). SYT interval 8 at 44.1k.
  About 69 % data packets, about 31 % empty packets (SYT=0xffff). Samples = 24 bit (AM824 MBLA, label 0x40).
- Not yet confirmed: mixer behaviour while streaming, playback (host -> device, rx section), clock drift correction, sample-rate switching.
- Confirmed: while streaming (iso allocated + enable=1) the router written by the host (26 entries) and the mixer
  coefficients take effect, and the output through the mixer (Mixer:0/1 -> headphones/analog outputs) is audible.
  Without writing the iso channel into the tx section and enable there is silence.
- Note: the session command of dice_ctl takes up to 95 tokens per line (it used to be 15, which cut long router specifications short).
- Next: playback (host -> device), the rx stream. The device is the master of the internal clock, so the host sends rx
  packets in step with the arrival of the device's tx packets, and SYT is derived from the SYT of the device's tx
  (to avoid drift). After that, move to the Audio Server Plug-in.

## Playback (host -> device) stream, hardware verification
- `dice_ctl stream play SECS [LATENCY] [FREQ]` plays a 440 Hz test tone; checked on the device peak (Avs0:0 -> Ins0:0) and on headphones.
- Transmit: a NuDCL send ring of 640 slots (= one lap of the 44.1 kHz packet pattern: 441 data + 199 no-data). One packet per cycle.
  Data = CIP (8 B) + 8 blocks x 9 quadlets (PCM 8 + MIDI 1) = 296 B, no-data = CIP only (SYT=0xffff).
  CIP: q0 = SID (own node) << 24 | DBS 9 << 16 | DBC, q1 = 0x90 | FDF 0x01 (44.1k) | SYT. DBC increases by 8 per data packet.
- Timestamps: SetDCLTimeStampPtr alone does not make the DCL update. Set `kNuDCLDynamic | kNuDCLUpdateBeforeCallback` and
  register the previous 16 slots as the update list of the callback DCL placed every 16 slots. The value is in bus cycle
  timer format (seconds [31:25], cycles [24:12], offset [11:0]); the cycle count is (v >> 12) & 0x1fff. The kernel writes 0
  when it builds the program, so put 0xffffffff back after starting.
- Update on every lap: each time a slot is sent, SYT is recomputed with that send cycle + 640 as the send cycle of the
  next lap and written. SYT = (4 bits of the presentation time in 16-cycle periods, phase within 3072 ticks).
  Presentation time = send cycle + LATENCY + (ideal phase - slot position).
- LATENCY (in 16-cycle periods) the device accepts: 2-10 are accepted (peak 0x02ff, sound), 0, 1 and 11-15 are rejected (peak 0).
  The default is 6, the middle of the window. Larger values work as long as the remainder after dividing by 16 is inside
  the window (24 and 200 are accepted; 32, 48, 64, 96 and 128 are rejected).
- 120 s of continuous playback did not drop out and the peak stayed (clock drift is no problem over a short time; long
  runs and buffer headroom were not verified).
- Only the first lap fills the start cycle with an estimate (approximate), so the first ~80 ms can be invalid.

## Duplex test
- `dice_ctl stream duplex SECS [LATENCY] [FREQ]`: runs tx (recording) and rx (playback) at the same time. dice_iso.c is split
  into cap_begin/cap_end (receive) and play_begin/play_service/play_end (playback), shared by capture / play / duplex.
- Loopback: with Avs0:0 -> Avs0:0 (0xb0b0) in the router, the sound the host sends is turned around inside the device and
  comes back on tx. The receive ring is checked every second (missing data block counter values, discontinuities as a sine
  wave). One discontinuity per lap of the ring at the turn-around is normal.

## Clock synchronisation (ARX1): results

- Running duplex on the internal crystal (0x0c), at any latency of 2-10 cycles, produced 10-18 duplicated samples (crackling) every few seconds. The cause is that the host's transmission (based on the bus cycle timer) and the device's consumption (based on the device's crystal) are different clocks.
- Writing the clock select `(1<<8)|0x08` (44.1 kHz, ARX1) one second after the stream starts made the device synchronise to the SYT of the playback stream.
  - Right after the switch the status is 0x100 (unlocked) and returns to 0x101 (locked) within about a second.
  - One DBC discontinuity appears at the moment of the switch (expected).
  - Afterwards 0 glitches for 10 seconds. No crackling is audible. late slots 0, longest service gap 4.0 ms.
- Decision: add "switch to ARX1 -> wait for the status lock bit" at the end of the stream start procedure. The device loses the lock when the host stops sending, so on stop switch back to the internal clock (0x0c) first.
- Not verified: use with an external clock (S/PDIF, Word Clock), stability over a long time (minutes), rates other than 44.1 kHz.

### 300 s of ARX1 duplex, and the seam correction in the analysis

- 300 s reported "glitches in 61/300 snapshots", but by ear there was no dropout apart from the one at the start.
- The cause was a false detection on the analysis side. A snapshot reads the 512-packet ring while it is being written, so there is one seam between old and new data at the DMA write position. With 353 data packets in the ring DBC is shifted by 8 and the seam is detected; with 352 DBC is continuous and the seam was counted as a jump of the sine wave. This matches that only the seconds with "352 data packets, dbc breaks 0" showed glitches.
- Fix: for snapshots with dbc break 0, subtract one glitch (the seam).
- The dropout right after the start comes from the lock transient (about 1 s) when switching the clock to ARX1. A procedure that locks the PLL before the stream starts is to be considered (below).

### Phase 3 complete (44.1 kHz, full duplex, ARX1 sync)

- 120 s: 0/120 glitches after the seam correction, late slots 0, longest service gap 6.6 ms. By ear no dropout except at the start.
- Established start procedure: ownership -> internal clock -> router -> stream_format -> allocate iso channel -> write iso channel -> enable -> switch to ARX1 after about 1 s -> wait for lock.
- Not handled: removing the dropout at the start (lock transient), external clock.

## libmbox3 (as a library)

The procedure verified on hardware was collected into one C library in `lib/` (`lib/mbox3.h` is the public API).

- Layout: `lib/dice_io.[ch]` (register I/O, moved from tools), `lib/mbox3.[ch]` (engine), `tools/mbox3_demo.c` (test tool), `dice_ctl` / `dice_iso` (for investigation, as before).
- Life cycle: `mbox3_open` (snapshot of stream_format -> take ownership) -> `mbox3_start` (internal clock -> router -> stream_format -> iso channel -> enable -> switch to ARX1 after about 1 s -> wait for lock) -> audio in/out -> `mbox3_stop` (back to the internal clock first) -> `mbox3_close` (release ownership).
- IO thread: owned by the library; every 2 ms it takes in the receive ring and refills the send slots. During warm-up (until the lock) it sends silence and discards the input. The dropout at the start is expected to become inaudible (to be confirmed on hardware).
- Audio format: 44.1 kHz, 8 ch interleaved, each sample the low 24 bits of an int32 (sign-extended). ch0-5 = analog 1-6, ch6-7 = S/PDIF. MIDI is not supported.
- Hand-over: lock-free rings for playback and capture (16384 frames each). Playback takes in the data to be sent about 80 ms ahead, so the output latency is 80 ms or more for now. There is room to shorten it by making the send ring a short period (for example 64 slots) (the pattern position is computed as the global slot number mod 640).
- The default router is the 40 entries the device itself held in standalone mode (host -> mixer -> outputs, inputs -> host). The default mixer is out n <- in n at 0 dB (n = 0-7). Neither has yet been confirmed on hardware through the library.
- Limits: one instance per process, the first device found, 44.1 kHz only. The API for application settings (phantom, HPF, ...) is not ported yet (it is in `dice_ctl`).

### libmbox3 on hardware (44.1 kHz)

- 20 s loopback: 0 glitches, underrun/overrun/dbc/late all 0, 954912 frames in and 954944 frames out (the clock sync works), longest IO thread pause 13.0 ms. No dropout at the start is audible (the silence during warm-up worked).
- Default routing: sound comes out of outputs 1/2. The headphones were silent (in the default router the headphone path Ins1:0-3 comes from mixer outputs 8-11, but nothing was fed into them). Fixed by adding out 8,10 <- in 0 and out 9,11 <- in 1 to the default mixer (to be confirmed).
- The one-time dbc disturbance at the moment of switching to ARX1 does not affect the sound, so the statistics are reset once audio starts to flow.

## Phase 4: structure of the Audio Server plug-in

In the coreaudiod sandbox the only external access a plug-in can use is the mach services listed in `AudioServerPlugIn_MachServices` of Info.plist (Apple QA1811). Direct FireWire access cannot be relied on, so a two-part structure was chosen.

- `mbox3d` (daemon/mbox3d.c): a LaunchDaemon (root). It owns and streams the device with libmbox3 and keeps the capture and playback rings in shared memory (driver/proto.h). It hands over the shared memory through the XPC mach service `com.mbox3pro.driver.daemon`. When there is no device, or the stream has stopped (no audio for 1.5 s), it reopens every 2 seconds.
- `Mbox3.driver` (driver/Mbox3Driver.c): the plug-in inside coreaudiod. It receives the shared memory over XPC and only converts between CoreAudio and the rings (Float32 <-> 24 bit). Without the daemon the device still works, silently.
- Time base: CoreAudio sample time = capture frame number. The daemon publishes "the time of frame 0, `base_ns`" (smoothed with an EMA) and the plug-in builds the zero timestamp (period of 2048 frames) from it.
- Capture: frame T is `cap[T & mask]` (valid while T < cap_w). Input safety offset 512 frames.
- Playback: the plug-in writes frame T into `play[T & mask]`. The daemon takes frames in order at the device's clock (`play_pos`) and sets what it has taken back to 0 (so old sound is not played). The library's playback ring always keeps 1536 frames of silence (prefill), so it takes about 5200 frames (about 118 ms) from writing to hearing. Output safety offset 512 frames.
- Limits (v1): 44.1 kHz only, fixed 8 ch (analog 1-6 + S/PDIF), no MIDI, plugging and unplugging the device is handled only by "reopen when the audio stops". While the daemon runs, the device stays host-owned (to return to standalone operation, stop the daemon and power-cycle the device).
- XPC peer: the daemon accepts only root and the `_coreaudiod` user (`xpc_connection_get_euid`); other peers are cancelled and logged (first five). The code signature of the peer is not checked: the plug-in runs inside coreaudiod, whose signing identifier differs between macOS versions (a separate driver-service helper from macOS 14.4) and may be re-signed by OCLP root patches, so an `anchor apple` requirement could lock the plug-in out. Anyone able to run code as `_coreaudiod` or root can still get the shared memory.

### Crackling in playback through CoreAudio (first hardware test)

- The daemon alone (tone of m3_shm_check) was fine. Playback through CoreAudio produced sound, but with crackling and dropouts.
- Suspicion: the daemon takes frames in before CoreAudio has written them, and frames written late are discarded on the plug-in side. The write headroom is only about 420 frames (about 10 ms): the safety offset of 512 frames minus clock offset and IO jitter.
- Fix 1 (diagnostics): add io_cycles / late_write_frames / write_margin_min / late_read_frames / read_margin_min, measured by the plug-in, to the shared memory and show them in m3_shm_check.
- Fix 2: delay the daemon's take-in time by M3_PUMP_DELAY = 1024 frames (frame T is taken when the device clock is at T + 1024). Of the 1536 prefill frames, 512 are left for the daemon's scheduling and 1024 go to CoreAudio's headroom. The output latency does not change.

### Second hardware test (heavy noise) and the fix

- Numbers from watch: a large stall before the start (under 4371 = about 35000 frames of silence, late 672, dbc 191). The playback take-in position stayed about 25400 frames behind the capture position. CoreAudio's late writes were 0 and the write margin always sufficient. During monitoring, 25 underrun packets in 30 s.
- Judgement: the problem is not CoreAudio's writing but that the daemon and the IO thread sometimes stop (at normal priority, under the load of music playback a 1 ms sleep becomes tens of ms). A stop causes underruns (silent packets), and the playback time base stays shifted against the take-in position.
- Fix: (1) real-time priority (time-constraint, mbox3_make_thread_realtime) for the library's IO thread and the daemon's take-in thread. (2) take-in delay M3_PUMP_DELAY 512, playback queue target 1024 frames (about 23 ms), output safety offset 1024. (3) after an underrun the daemon puts the time base back to real time with silence (V = consumed frames + 8 x underruns), so the lag of the take-in position (play_pos) does not accumulate. (4) add io_jumps / io_jump_max to the diagnostics (how many times, and by how much at most, a CoreAudio output cycle did not start where the previous one ended).

### Third hardware test (crackling remains) and the fix

- The daemon-side stalls were fixed (under 0, late 0, dbc 0, the playback stop problem also improved).
- Diagnostics (io_jumps): about 70 % of the output cycles did not start where the previous cycle ended (normally a few frames, at most 16-20, sometimes -500). The mSampleTime CoreAudio hands over changes by a few frames every cycle because of jitter of the clock we return, and writing at that time every cycle duplicates or drops one sample at the boundary (about 86 crackles per second).
- Fix: the plug-in remembers "the next frame to write (read)" and treats it as contiguous, ignoring jitter while CoreAudio's time is within ±128 frames (M3_JUMP_TOLERANCE). Only beyond that does it realign the position (for both output and input). The diagnostics are split into absorbed jitter (io_wobble) and real jumps (io_jumps / in_jumps). The smoothing of the daemon's clock was slowed from about 0.3 s to about 8 s (initially a cumulative average).

### Fourth hardware test and fix

- Even with a tolerance of 128 frames, about 90 % of the output cycles were off the cursor, and jumps over 128 frames occurred about 5 times per second (166 times in 30 s). The crackling matched the number of these resyncs. The jitter of CoreAudio's cycle start time is several ms; the cause was not identified (jitter of the HAL wake-up, or jitter of our clock).
- Fix: widen the tolerance to 2048 frames (about 46 ms) and write (read) contiguously. Add the mean offset from the cursor and the number of times it exceeded 128 frames to the diagnostics to measure the jitter.
- Next candidate: build the zero timestamp from a long-term regression (slope and intercept, updated every second) of the daemon's measurement, so that it is linear in time. This removes the jitter that enters the HAL's DLL.

### Fifth hardware test and fix

- With a tolerance of 2048, the crackling decreased a lot and occasional dropouts remained. The write margin (write margin min) fell by about 300 frames per second and reached 0 about every 7 s, causing a resync (a jump of 2052 frames).
- Presumed cause: the CoreAudio time base is built as 44100 Hz on the host's uptime clock, but the device runs synchronised to the bus cycle timer. The speed difference between the two clocks (about 0.7 %) accumulated.
- Fix: every 250 ms the daemon collects (capture_frames, capture_host_ns) for up to 60 s, computes ns/frame by least squares every second and publishes it in shared memory with a sequence lock. The plug-in builds the zero timestamp with this slope. m3_shm_check shows the measured rate.

### 6. 15 minutes of continuous playback, and shorter output latency

- With the regression time base, `real jumps` 0 and `under/late/dbc` 0 for 15 minutes. The measured speed difference between host and device was +0.003 % (the earlier estimate of 0.7 % was wrong; the fix is believed to have worked by removing the offset caused by the start-up average).
- Most of the previous output latency (about 5200 frames) was the lead of the send ring (640 slots = 80 ms = 3528 frames). Slots were filled 80 ms before they were sent.
- Change: the slot header (DBC and SYT) is rewritten on completion as before. The audio content is put in when the slot is 64 slots (about 8 ms) ahead of the completion report. Completion reports come every 16 slots and the IO thread runs every 2 ms, so 64 slots is a guide for a safe lower limit. If it is not in time and the slot is sent anyway, silence is sent and counted as an underrun.
- Estimate: output latency = prefill 1536 + about 350 + SYT/converter = about 2000 frames. M3_OUT_LATENCY was set to 2048. The measurement is `mbox3_demo latency` (round trip measured with the device's built-in loopback).

### 7. Hardware result of the 64-slot lead, and tuning

- Looped-back tone, 20 s: glitches 0, under/late/dbc 0. Audio is continuous with the new send method.
- CoreAudio playback: under increased twice, by 11 and 12. This matches the number of data packets in one group of 16-slot completion reports (about 11). It reads as the IO thread being late for a moment and one group being sent as silence (at the start or stop of playback). Before, the 80 ms lead hid the delay.
- Fix: TX_AHEAD from 64 to 128 slots (about 16 ms). M3_OUT_LATENCY is an estimate of 2400 frames.
- `mbox3_demo latency` reported "the impulse does not come back". The tone round trip is fine, so this is a defect of the measurement. Diagnostic output (frames written, frames taken in, maximum of ch0) was added.

### 8. Slope jitter right after the start

- Hardware with a lead of 128 slots: under/late/dbc 0 for 60 s. The write margin fell from 1804 to 972 in about 60 s. The regression right after the start has few points and the slope jitters (the host time is accurate to about 2 ms, so about 700 ppm for 3 s of data and about 170 ppm for 12 s).
- Fix: until 20 s of points (80 points) have been collected, the slope is not published and the plug-in uses the time base at the nominal rate (base_ns). The regressed slope is limited to ±150 ppm around the nominal value.
- The measured difference to the host clock is about +30 ppm (44101.3 Hz).
- The latency measurement failed because the daemon had just been stopped and the device was still held. Wait 3 s after the stop.

### 9. Time base right after the start, and an IO thread stall

- During the wait for the slope (20 s), with the nominal-rate time base the write margin fell by about 300 frames per second, reached 0 in about 6 s, and late writes of 104 fr and a resync occurred. After switching to the regression (from about 8 s) the margin was stable at 1900-2000. The nominal-rate time base (base_ns) most likely has some other error (cause not identified).
- Fix: the slope is nearly constant (+30 ppm) for a given machine, so the last measured ns/frame is saved in /var/db/mbox3d.rate (after a 60 s window has accumulated, every 60 s). At the next start it is used as the initial slope and the time base is published from 2 s (8 points). The intercept is the mean of the points. After 20 s of points the regressed slope takes over.
- Around 36 s, under increased by 55 (about 80 slots = about 10 ms of silence). A stall that even a lead of 128 slots cannot absorb; it reads as the IO thread stopping for 25 ms or more. To find the cause, the longest IO thread stall (max_gap_us) was added to shared memory and shown in m3_shm_check.
- The latency measurement failed with "clock did not lock" (a known problem that happens when reconnecting without a power cycle after the previous playback).

### 10. Check of the saved slope

- First run (no saved slope): under/late/dbc 0 for 90 s, late writes 0, real jumps 0. Write margin 1400-2000. Longest IO thread stall 7.1 ms.
- Second run (after restarting the daemon, using the saved slope): the time base was valid from 2 s after the start, and under 0 and late writes 0 for 40 s. The margin only decreased slowly, from 1536 to 1344. Longest IO thread stall 5.2 ms. The earlier stall of 25 ms or more did not reproduce.
- Noted: when the daemon is restarted, the plug-in's cursor stays on the old time base and one real jump (-4803660 frames) happened. The cursor is now rebuilt when the seed changes.

### 11. Defect of the latency measurement

- `latency` after a power cycle: the maximum of received ch0 stayed 0 and the impulse did not come back (383616 frames written, 384112 frames taken in, impulse at 4410, waiting 1). The tone loopback had been fine before, so it is either the device state or a defect of the measurement code.
- The measurement code was changed from a one-sample impulse to a 32-frame rectangular burst. A loss when the write was accepted only partially was also fixed.
- To narrow it down, run `tone 10 440 loopback` before `latency`.

### 12. Measured latency, and tuning the prefill
- `mbox3_demo latency 1536`: round trip 2222 frames (50.4 ms), the same value in all 10 runs (no spread). Subtracting the prefill of 1536 leaves 686 frames, which matches the estimate for a send lead of 128 slots (about 705 frames). The loopback does not include converter delay, so the output latency is this plus DA (about 35 frames), about 2270 frames. M3_OUT_LATENCY was set to 2272.
- The round-trip delay is prefill + send lead + receive side. The prefill is silence placed at the head of the pipeline.
- The prefill and the amount the pump keeps in the library (queue) can be changed in /var/db/mbox3d.conf (`scripts/tune.sh PREFILL QUEUE`). The time the pump takes frame T of shared memory is when the device clock is at T + (prefill - queue). This difference trades off against the time CoreAudio has to write (write margin).
- In all measurements so far the write margin min is 1400-2000, so there is plenty to spare. CoreAudio writes frames about 1400 frames ahead.

### 13. Results of tuning the prefill

| prefill / queue | output latency | 60 s | longest IO thread stall |
|---|---|---|---|
| 1024 / 768 | about 39 ms | under 0 | 15.4 ms |
| 768 / 512 | about 34 ms | under 0 | 6.2 ms |
| 512 / 384 | about 28 ms | under 0 until 59 s, 381 in the last second | 11.1 ms |

- The last under of 381 at 512/384 (about 69 ms of audio) happened while the longest IO thread stall stayed at 11.1 ms, so it reads as a stall on the daemon's pump side. To tell them apart, the maximum interval of the pump loop is published in shared memory (max_pump_gap_us).
- The default became 768 / 512 (M3_PREFILL_FRAMES 768, M3_PUMP_DELAY 256) and M3_OUT_LATENCY 1504.
- A real jump of 1 appeared at the start of every setting. The identifier of the time base (seed) was counted from 1 again at every daemon restart and so had the same value as last time, so rebuilding the plug-in's cursor did not take effect. It was changed to a value derived from the start time.

### 14. Five-minute check, and an IO thread stall

- 768 / 512 for 5 minutes: late writes 0, real jumps 0 (the seed fix worked), dbc 0. under rose from 0 to 41 between 148 and 149 s and did not rise elsewhere.
- At that moment the longest stall of the library's IO thread changed from 9.5 ms to 20.4 ms. The longest interval of the daemon's pump loop was 1.5 ms. So the cause of the gap was not the pump but the IO thread stall. The headroom is about 12 ms: the send lead of 128 slots (16 ms) minus the completion report interval and so on. The difference to the 20.4 ms stall is about 8 ms, which matches under 41 (328 frames).
- The cause of the IO thread stall is not identified (competition with other real-time threads or kernel work on a 2-core machine is suspected). How long a stall can be survived is set by the send lead in slots, so it was made configurable (mbox3_config.tx_ahead_slots, `ahead` in /var/db/mbox3d.conf, `scripts/tune.sh PREFILL QUEUE AHEAD`). One more slot adds about 5.5 frames of output latency.

### 15. Five minutes with a send lead of 224 slots

- ahead 224, prefill 768, queue 512 for 5 minutes: late writes 0, real jumps 0, dbc 0. under rose from 0 to 18 at 299 s (about 3 ms of audio; 41 at ahead 128). Longest IO thread stall 17.3 ms, pump 2.9 ms.
- The time under appeared was right after the plug-in's cycles hardly advanced for a second (a pause-and-resume operation). The IO thread may be prone to stalling at the moment of resuming.
- The default became ahead 224 (output latency about 2030 frames = about 46 ms). M3_OUT_LATENCY is 2032.

### 16. Sample rate 48 kHz (library)

- The places fixed at 44.1 kHz were identified: the packet pattern (slot_is_data, data_before, number of data packets per lap), the ideal time of SYT, the rate number of the clock select register (bits 8..15; 1 = 44.1 kHz, 2 = 48 kHz), the rate number checked for the lock.
- The 48 kHz pattern is `slot * 48000 / 64000`, i.e. 3 data packets in 4 slots. One lap of 640 slots has 480 data packets (441 at 44.1 kHz). The number of data packets per lap is `rate / 100`.
- mbox3_config.sample_rate accepts 44100 or 48000. The router and stream configuration commands stay in the same low-rate mode as 44.1 kHz (RATE_FLAG_LOW).
- `rate=48000` was added to mbox3_demo. The daemon and the plug-in are still fixed at 44.1 kHz (notification of rate changes in the plug-in and restarting the daemon are the next step).

### 17. 48 kHz on hardware

- `tone 10 440 loopback rate=48000`: glitches 0, under/late/dbc 0, locked 1, about 52000 frames received per second (one second of the demo is about 1.083 s, which fits 48000). The following 44.1 kHz tone also had glitches 0, so nothing was broken.
- `latency 768 rate=48000`: ch0 stayed 0 until the end, under 200, and it failed. The same path works for the tone, so the routing works. A condition where the prefill of 768 is smaller than the send lead (224 slots = about 1230 frames) is suspected. Diagnostics (maximum of all channels, frames taken in, ...) were added and the queue target was raised to 2048.

## 18. Result of the latency test and a known quirk

- After changing to a gated 1 kHz burst (441 frames), measurements came out with the same value in all 10 runs.
  44.1 kHz: round trip 1979 frames (44.9 ms), 1211 frames (27.5 ms) left after removing the prefill of 768.
  48 kHz: round trip 2096 frames (43.7 ms), 1328 frames (27.7 ms) left. The remaining delay is constant in time.
- Only the first `latency` right after a power cycle did not get the loopback (LOOP route) working and ch0/ch1 stayed at 0.
  From the second run on it succeeds. No effect on the normal daemon path. Treated as a quirk of the LOOP route only (to be reproduced).

## 19. Sample-rate switching (44.1 / 48 kHz)

- When the plug-in receives a change of `kAudioDevicePropertyNominalSampleRate` it issues `RequestDeviceConfigurationChange`
  and, in `PerformChange`, switches `gRate` and writes it to `req_rate` in the shm.
- The daemon looks at `req_rate` in the loop of `run_session`; if it differs from the current one it closes the session and
  restarts `mbox3_start` at the new rate (about 3 s). When the start succeeds it writes `cur_rate`.
- The plug-in produces sound only when `cur_rate == gRate` and the state is STREAMING (silent during the switch).
- The prior rate for the timeline is `/var/db/mbox3d.rate` for 44100, a separate file such as `.48000` for the others.
- Output latency `M3_OUT_LATENCY_FOR(rate)` = prefill + rate x 1264 / 44100 (2142 frames at 48 kHz).
- When coreaudiod is restarted the plug-in starts at 44.1 kHz and the daemon follows it.
- The shm protocol is version 2 (`req_rate` and `cur_rate` added). Always replace the plug-in and the daemon together.

## 20. 48 kHz on hardware

- Switching 44.1 <-> 48 kHz four times from Audio MIDI Setup, the daemon restarted at the new rate every time (about 3 s).
- 48 kHz: timeline 48001.5 Hz (difference to the host +0.003 %, same as 44.1 kHz), under/late/dbc/late writes 0,
  write margin min 1428 frames. No oddness by ear (confirmed by the user).
- A quirk in the statistics: at 48 kHz the start of the writes is off by about 158 frames every cycle (>128 every cycle). At 44.1 kHz about 6 frames.
  Within the tolerance of 2048 and not audible. The cause has not been investigated (may be the relation between CoreAudio's cycle length and the timeline).
- The plug-in's statistics are not reset to 0 by a rate switch (`real jumps 1` is the one at the switch).

## 21. Control panel (browser UI)

- The daemon `mbox3d` has a small HTTP server on 127.0.0.1:47310 (`daemon/ctlserver.[ch]`).
  The UI is `ui/index.html` (installed in `/usr/local/share/mbox3pro/ui/`).
- The device is held by the daemon, so settings are read and written inside the daemon through `libmbox3`
  (`mbox3_params_get/set`, `mbox3_mixer_read_all`, `mbox3_mixer_set`, `mbox3_read_peaks`). Register access is serialised by `m->reg`.
- API: `GET /api/state` (state, parameters, mixer 16x18), `GET /api/meters` (array of [route, level]),
  `POST /api/param` (name=phantom|hpf|trim|rvol|rdur|rfb|rtype, i, v), `POST /api/mixer` (c=out,in,value;...).
- Safety: listens on 127.0.0.1 only. The Host header is checked (against DNS rebinding). POST requires the `X-Mbox3` header
  (a page of another site cannot send it without a CORS preflight). 48 V shows a confirmation dialog.
- UI structure at that time: per destination tab (Out 1-2 / 3-4 / 5-6 / S/PDIF / Phones A / B), strips for DAW 1-8, inputs 1-6, S/PDIF and reverb return.
  Fader (dB), pan, mute, HPF, input meter, master meter. Output trim and reverb at the bottom right.
  Pan is expressed as the ratio of two cells (L and R). Level = the larger one. (The tabs were removed later; see section 28.)
- Not verified: 0 dB = 0x4000, meter full scale = 0x7fff (upper 16 bits of the peak), the correspondence between destination tabs and mixer out numbers.
- For development there is `tools/mock_ctl_server.py` (runs the UI without hardware).

## 22. Calibration of the peak meter (hardware)

`scripts/meter_cal.sh` (plays a 1 kHz tone on host stream ch0 and reads the peak of router src 0xb0):

| tone | meter |
|---|---|
| 0 dBFS | 4095 (0xfff, saturated) |
| -3 dBFS | 4095 (the previous peak was still held; the hold decay takes a few seconds) |
| -6 dBFS | 2052 |
| -12 dBFS | 1028 |
| -20 dBFS | 409 |
| -30 dBFS | 129 |
| -40 dBFS | 40 |
| -60 dBFS | 4 |

A 12-bit linear value, equal to amplitude / 2048 (24 bit). The UI shows 4096 as 0 dBFS.

## 23. Pro Tools: input works but there is no output (analysis of a hardware log)

- Symptom: silent output after starting Pro Tools. In `m3_shm_check`, `play_pos` was 43,904 frames (about 1 s) behind
  capture at 114,785,520 frames (about 43 minutes of running). The plug-in's `write margin min` was 44,9xx frames (more than the ring of 32,768).
  The plug-in discards writes outside the ring, so the output was silent.
- The same log had `longest pause: library IO thread 143 ms, daemon pump loop 83 ms`, `late 1920`, `under 7879`, `dbc 167`.
  So there was a long stall (143 ms = about 1.8 laps).
- Cause: if the send ring (640 slots = 80 ms) cannot be serviced for more than one lap, the slots are sent again with their old content.
  Only the latest completion notice per slot remains, so the skipped laps were not counted in `tx_done`,
  and the device time (V = playback_frames + 8 x underruns) fell behind real time.
- Fix 1 (library): derive the number of skipped laps from the difference of the completion cycles and add it to `tx_done`.
  The slots in the difference to `tx_fill` are then counted in `underruns` (they have already gone out) and V catches up with real time.
- Fix 2 (daemon): capture and playback run on the same device clock, so the difference (capture_frames - V) should be constant.
  Taking the difference 2.5 s after the start as the reference, when it widens by more than 1024 frames V is aligned to the capture side
  (the frames in between are replaced by silence and align with the plug-in's write position again). `playback timeline fell behind` is logged.
- Unresolved: the cause of the 143 ms stall itself (CPU contention caused by Pro Tools' load?). If it reproduces, follow it with `longest pause`.

## 24. Made the timeline fit robust (under Pro Tools load)

- Hardware log while using Pro Tools: the timeline was 44096.77 Hz (difference to the host -0.007 %, normally +0.003 %), with exactly the same value every second.
  `late reads` of almost all frames every second and `read margin min` of -2.3k frames. The input was read ahead and zeros were mixed in, causing pops.
- Presumed cause: the least-squares fit was dragged out of range by host times delayed by stalls (up to 143 ms),
  and in that case it was built not to publish anything, so the anchor stayed old. The saved prior rate may also have been contaminated.
- Fix: the slope is the median of the slopes between points half a window apart (Theil-Sen) and the intercept the median of the residuals. Publishing continues with the prior rate even when out of range.
  On synthetic data (8 % outliers, 50-140 ms late) the error is 3.9 ppm against 51 ppm for least squares.
- Diagnostics: `play_peak` (peak of the playback audio handed to the device, every 250 ms) was added to the shared memory and is shown by `m3_shm_check`.
  It separates whether sound is actually coming from the CoreAudio side.

### 24.1 Hardware result (Pro Tools)

- After the fix, sound came out of Pro Tools (confirmed by the user). `playback peak` moved from 0 to 0x5a8345 as playback started.
- The timeline returned to 44101.33-44101.37 Hz (+0.003 %). `late writes 0`, `real jumps 0`, `input jumps 0`, and `read margin min` positive after -644 right after the start.
- Something to watch: `longest pause` is 243 ms for the daemon's pump and 27 ms for the library's IO thread (over the first 10 s).
  Pro Tools' buffer is small (about 390 output cycles per second, about 113 frames) and the mean `off-cursor` is about 200 frames (within the tolerance of 2048).

## 25. Output dropped again after about 8 minutes: filling the holes of lost capture

- Log after about 8 minutes of playback from Pro Tools (cap 22.4M frames): `play_pos` was 1320 frames "ahead" of `cap`,
  `write margin min` negative, `late writes` of almost every frame every second, `playback peak 0`, `dbc 67`, `late 0`.
  The send side (`late 0`) was fine and the receive side had lost packets 67 times. `cap` fell behind real time by the lost amount,
  and CoreAudio's timeline (the capture index) fell behind. The output write position was overtaken by `play_pos`.
- Fix 1 (library): derive the number of lost frames from the jump of the data block counter (which counts frames; less than 128)
  and insert silence. `capture_dbc_errors` is still counted as before.
- Fix 2 (daemon): compare `(capture + inserted) - V` with a reference value c0. If capture falls behind by 1024 frames or more,
  insert silence into the capture ring in shared memory to catch up (log: `capture fell behind`).
  Conversely, if V falls behind, align V to the capture as before (log: `playback timeline fell behind`).
  The fit uses `capture_frames + cap_ins`, which includes the inserted amount.

## 26. Changing the header layout requires a version bump (a record of a failure)

Diagnostic fields (`plug_out_peak` and others) were added in the middle of the shared-memory header, but only the plug-in was replaced,
so the field positions of the old daemon and the new plug-in were misaligned. The position of `cur_rate` did not match, the plug-in
decided "the daemon is not yet running at this rate" and produced no sound, so Pro Tools and YouTube were both silent (`cycles` did not increase).
When the header changes, always replace the daemon and the plug-in together. `M3_VERSION` was raised to 3 and old combinations are rejected at connection time.

### 26.1 Hardware result after the replacement (Pro Tools, H/W buffer 256)

- Sound comes out and there is no distortion (confirmed by the user). Line `CoreAudio output`: the peak from the app and the daemon's `playback peak` agree,
  256 frames per cycle (at most 512), `dropped as too far ahead 0`, `late writes 0`, `real jumps 0`, `input jumps 0`,
  `write margin min` 864-1348, `read margin min` about 500 after -148 right after the start.
- The mean `off-cursor` is about 250 frames (at most 668), inside the tolerance of 2048.
- A large part of the earlier "distortion and silence" is explained by the combination of an old daemon and a new plug-in (header misalignment).
  Re-checking over a long time (10 minutes or more) remains.

## 27. Distortion and dropouts at small H/W buffers (64-128) — conclusion

### 27.1 Symptoms

With the Pro Tools H/W buffer at 128, distortion and pops appeared with a period of about 30 s, and after about 10 minutes the output sometimes went silent.
It did not happen at 256. CPU usage was 2 %, so load is not the cause.

### 27.2 Causes (three stacked)

1. **The write position is overtaken by the read position.** The plug-in's write cursor is contiguous, but CoreAudio's cycle is about 0.1 % (about 47 frames per second) slower than the device.
   The cursor keeps falling behind CoreAudio's time, and the headroom (about 1000 frames) to the daemon's read position (`play_pos`) runs out in about 25 s. From then on the start of each cycle is written late.
   This is the "period of about 30 s". On the daemon side (playback time V and capture time) the 10-second averages agreed at 44100 ± 15 fr/s during this, so there is no drift there.
2. **Re-placing the cursor alone is not enough.** A re-placement to `play_pos + 1024` when the headroom becomes negative (`M3_NOM_LEAD`) was added, but a re-placement about every 25 s is a click.
3. **Correcting with gaps produces a click train.** Advancing the cursor by about 97 frames to follow CoreAudio's time made a silent gap every 2 s and the sound was "distorted all the time".

### 27.3 Final fix (plug-in `driver/Mbox3Driver.c`)

- Watch the difference `CoreAudio time - cursor` with a slow moving average (1/64), taking its value at the moment the cursor was placed as the reference.
- In proportion to the difference from the reference, change the length of the output block by -2 to +2 frames per cycle (stretched and shrunk with Catmull-Rom cubic interpolation).
  The steady-state difference is about 35 frames. Only a difference over 512 frames moves the cursor at once.
- The interpolation error (1 kHz sine wave) is at most 4.5e-5 (about -87 dB).
- If the headroom is negative for 3 cycles in a row, or exceeds 16384, the cursor is re-placed at `play_pos + 1024` (a safety valve). Re-placements are counted in `real jumps`.

### 27.4 Hardware results

- At buffers 64 / 128 / 256: no distortion and no noise in any.
- 111 measurements in total (about 1.1 s each): `late writes` 0, `write margin min` 877-1532, `real jumps` 0, `under` does not increase.
- Stretching happens about 60 times per second, by about 1 frame each time.

### 27.5 Hypotheses that were wrong (record)

- Raising `prefill` increases the headroom: wrong. The write deadline is set by `prefill - queue`, so raising both by the same amount does not change the headroom (it only adds delay).
- Playback time V and capture time drift apart by 1000 ppm: wrong (the speed log agrees).
- OS wake-up delay (CPU load): wrong. The wake-up interval is regular at about 3.5 ms.
- The run of `capture fell behind by ~1056 frames` and `no audio ... restarting` in the hardware log are from before the restart (the old state). They did not reproduce with the new daemon.
- If you forget to restart `coreaudiod` right after installing, the old plug-in and the new daemon end up running together. After `install.sh all`, always restart Pro Tools.

### 27.6 Diagnostic output (kept)

- Daemon: `rates over ... s: capture .. fr/s, playback .. fr/s, lag-c0, queue` every 10 minutes. A warning with time and counters when a deviation is detected.
- `m3_shm_check watch`: write margin, late writes, wake-up interval (`CoreAudio wake-up`), count and total amount of stretching, number of re-placements.

### 27.7 Still unexplained

- Why CoreAudio's cycle is about 0.1 % slow (the ZeroTimeStamp period is 2048 frames, the time is a fit aligned to the device's capture).
- The wake-up delay of 160-200 ms that appears about every 6.7 s while nothing is playing. During playback it stays within 3.5-11 ms.
- The stalls that appear once at the start: IO thread 11-46 ms, daemon pump 82-125 ms.
- The input side (`late reads`, `input jumps`) has no processing equivalent to the output side's stretching.
- The maximum of `real jumps` (`io_jump_max`) shows a value of about -1.9e9 only right after the daemon restarts (a display-only problem).

## 28. Automatic restore of the last mixer state

A preset feature was built, but "return to the last used state" was enough, so it was removed (the UI button, the API and the storage are all gone).

- Storage: `/var/db/mbox3d-last.state` (text, `mbox3-last 1`). Written by the daemon (root). Not removed by an uninstall.
- Contents: all cells of the mixer, 16 outputs x 18 inputs, and the reverb (type, level, duration, feedback). **48 V phantom power is neither saved nor restored**
  (a restore never switches it on). HPF and output trim were removed from the UI and are not handled (they stay at whatever the device holds).
- Saving: only when something changed, the daemon's saver thread checks every second and writes (to a temporary file, then `rename`; if it cannot write it retries next time).
- Restoring: at every start of streaming (including sample-rate changes and reconnects), inside `ctl_server_set_device`, the mixer is read,
  only the cells that differ from the file are written, and the reverb is restored. At start the library rewrites the diagonal of Out 1-8 and the
  headphone cells to 0 dB (`write_default_mixer`), but the restore happens after that, so the saved values win.
  If the file is missing or damaged (too few cells, out of range) nothing is done, and only a damaged file is logged.
- Log: `mbox3d: restored the last mixer state (N cells changed)`
- UI: the preset button and dialog were removed. The High-pass filter and Output trim cards and the HP buttons on the microphone strips were also hidden.
- If old preset files remain, `sudo rm -rf /var/db/mbox3d-presets` removes them.
- Tests: a C test with a simulated device (change -> save -> restore in another session, phantom unchanged, a damaged file ignored) and a UI check in Chromium.

### 28.1 Structure of the mixer screen (tabs removed)

- The destination tabs were removed. From the left: Output (output volume of Out 1-8 = cell (o,o)), Phones (A L/R, B L/R = cells (8,0)(9,1)(10,0)(11,1)),
  Input, S/PDIF, Reverb, Monitor meter (Out 1/2).
- The Output and Phones faders operate the diagonal cells of the default mixer (Out n <- DAW n at 0 dB) directly. There is no pan.
- The faders and pan of Input, S/PDIF and Reverb write the same value to three pairs at once: Out 1-2 and Phones A and B (one common monitor mix).
  The display is derived from the cells of Out 1-2.
- An earlier edit deleted `st.push(el)` and the faders and pan stopped working (the array used for drawing was empty). Fixed in this version.

### 28.2 Stereo link and resetting the outputs

- A `LINK` button for each of 6 pairs: Out 1-2 / 3-4 / 5-6 / 7-8, Phones A L/R, B L/R. Linked by default (moving one writes the same value to the other).
  Mute works on the pair too. When the link is switched on, the value of the left strip is used. The setting is in the browser's localStorage (this browser only).
  The button is shown once per pair, centred under the two strips.
- "Reset outputs": sets the diagonal of Out 1-8 and the headphone cells to 0 dB, and the other cells of DAW 1-8 for the same outputs to 0.
  Off-diagonal cells left by the pan control of the old UI (for example DAW 1 mixed into Out 2) are invisible in the new UI, so this operation clears them.
- S/PDIF: the input side is "S/PDIF In" next to Input (inputs 14/15), the output side is Out 7-8 ("S/PDIF Out").

### 28.3 Two-row screen

- The upper row is the input side (In 1-6, S/PDIF In, Rev), the lower row the output side (Out 1-6, S/PDIF Out = Out 7-8, Phones, Monitor meter).
  Microphone/line inputs are all labelled In 1-6. The reverb settings are a small panel right of the Rev faders in the upper row.
- Even after switching the sample rate, the mixer and the reverb are restored from the saved values (confirmed on hardware).

### 28.4 Language of the UI

- The UI is in English. A browser whose language starts with `ja` gets the Japanese texts (`I18N` in `ui/index.html`).

### 28.5 Reverb ON/OFF switch

- A button below the reverb settings. OFF writes 0 to the Rev L/R return cells (inputs 16/17) of the monitor outputs (Out 1-2, Phones A/B); ON restores the levels
  remembered in the browser (default -6 dB). The state is derived from the mixer cells, so it follows the Rev faders and the restored state.
  On the first visit of a browser (no `m3rvinit` in localStorage) the reverb is switched off. The device-side reverb parameters are not touched.

## 29. Silent output with pops after a restart cycle (case record)

- Symptom: while the mixer, trims, DIM and saved state were all fine (checked through `/api/state` and `/api/meters`), the sound from CoreAudio was silent with pops
  and the daemon's `playback peak` was mostly 0 although the app's peak was constant. `under 4049`, `over 16488` and a `start failed: clock did not lock` line were in the log.
- Direct playback through libmbox3 (`mbox3_demo tone`, daemon stopped) was clean, so the device, the cable and the output stage were fine.
- After reinstalling the daemon cleanly, `playback peak` followed the app's peak every second and the sound was clean again.
- Conclusion: the daemon/device had entered a half-started state (repeated failed starts and sample-rate switches are the suspects); it was not caused by the UI or the state restore.
  If it happens again, capture `m3_shm_check watch 5` and `grep -E "start failed|rate change" /var/log/mbox3d.log` immediately; a failed start in the log just before
  would point to re-initialisation after a failed start as the thing to add.
- Practical notes: unpacking the archive again deletes `build/`, so run `make daemon driver shmcheck` again before `scripts/install.sh all`. To stop the daemon for a direct test use
  `sudo launchctl bootout system/com.mbox3pro.driver.daemon` (killing the process only makes launchd start it again), and reinstall afterwards.

### 29.1 Cause found: the restore ran in the pump thread before streaming was serviced

- Every start that was silent (with pops) had the log line `restored the last mixer state (N cells changed)`; the start without a state file was clean,
  and `mbox3_demo` (no restore) was clean. `playback peak` was mostly 0 although the app's peak was constant, and `over` ran up to ~17000 at the start.
- `ctl_server_set_device()` runs in the pump thread between `mbox3_start()` and `run_session()`. The restore did a second mixer read, several register
  writes and a read-modify-write of the application section there, so the first service of the streams was delayed and the playback timeline started misaligned.
- Fix: `ctl_server_set_device()` only reads the mixer and arms a 2-second delay; the saver thread performs the restore afterwards (under the control lock),
  so the pump thread is never blocked by it. Verified with the simulated-device test (state restored about 2 s after the start).

### 29.2 Self-healing and the IO-thread stall trace

- Evidence (log of a run after opening the Sound pane): `playback timeline fell behind by ~1100 frames` repeating every 80 ms; after a restart `longest pause: library IO thread 302 ms`, `late 784`, `under 3431`,
  `playback peak` 0 while the app's peak was constant. A start that goes wrong leaves the counters high from the first seconds (good starts: `under` about 2000, `late` 0) and the sound stays silent / pops until the next restart.
- The daemon now restarts the stream by itself when it sees this: at 6 s after the start `under` or `over` above 3000 or `late` above 300 ("bad start"), or four "timeline fell behind" events within 10 s.
  At most 4 restarts in a row (the count is cleared after a session of 2 minutes). Log: `bad start ...; restarting the stream (N of 4)`.
- The library's periodic lock-status read no longer waits for the register lock (`trylock`), and an IO thread iteration that takes more than 60 ms logs where the time went
  (`IO thread stall: run loop .. ms, receive .. ms, send .. ms, status/other .. ms`). If stalls keep appearing, this line says which part is slow.

## 30. 88.2 / 96 kHz (mid rate class)

Status: **verified on hardware** (MacBook Air 2017, macOS 13): 44.1 / 48 / 88.2 / 96 kHz play cleanly through the daemon and the demo tool.

- DICE has three rate classes. 88.2 / 96 kHz is the "mid" class: clock rate index 3 / 4 (low class: 1 = 44.1, 2 = 48), command flag `0x00020000` (low: `0x00010000`),
  router table at current_config + 0x2000, stream config at + 0x3000. The device reports the same 8 + 8 channels in every class (section 3).
- Packet format: AM824 with 8 PCM + 1 MIDI quadlet per block as before, but **16 blocks per packet** instead of 8 (SYT interval 16). A packet then lasts
  16 / 96000 = 8 / 48000 s, so the data/no-data cadence of the 640-slot send ring is identical to the low class (441 data slots at 88.2, 480 at 96 per lap);
  the data block counter advances by 16. The library takes this from `bpp()` (8 or 16). The CIP FDF field carries the rate code (3 / 4) in the mid class; the low class is unchanged (1).
- Router: the library reads the device's own mid-class router table when a 88.2 / 96 kHz session is prepared (44.1 / 48 kHz sessions never touch it); it is identical to the default table on this unit. If it was not captured, or LoadRouter fails with it,
  the 44.1 / 48 kHz default table is used. The log says which: `using the device's own N kHz router`, `retrying with the default router`.
- Stream config: the mid-class snapshot is used, with the low-class one as fallback.
- Buffers: the packet buffers are sized for 16 blocks (send stride 592, receive payload 1024); the channel reservations stay at 304 / 512 bytes for the low class.
- Frame-count constants scale with the rate class (`M3_RATE_MULT`): prefill, pump delay / queue, safety offsets, jump tolerance, nominal lead, timeline resync thresholds double at 88.2 / 96 kHz
  so that the margins last as long in time. The daemon accepts 44100, 48000, 88200 and 96000 from the plug-in; the plug-in advertises all four.
- Problems found on the way (all of them also affected, or could affect, the low rates):
  - **Heap layout dependent `AllocateChannel` failure** (`0xe00002be`, kIOReturnNoResources). The isochronous DMA rings shared pages with other heap data; whether the call
    succeeded depended on the allocation pattern (an unrelated code change broke 44.1 kHz; ASan builds worked). The receive ring and send buffer are now page-aligned and a whole number of pages (`iso_alloc`).
  - **Underrun time accounting.** The device playback timeline is `playback_frames + blocks_per_packet * playback_underruns`, i.e. a silent packet is 8 frames at <= 48 kHz and 16 at 88.2 / 96 kHz
    (`8 * M3_RATE_MULT`).
  - **Startup gap larger than the library ring.** Until the pump starts (~0.35 s after clock lock) the device sends silence, ~2100 packets = ~17k frames, more than the 16384-frame library ring.
    The catch-up zero-fill then never finished and every real frame was replaced by zeros (silence at 48 / 96 kHz, flaky 44.1). A gap above 4096 frames is now skipped in one step.
  - **Dropped capture frames.** Frames the library dropped before the pump started (capture ring overflow) were not counted in the shm timeline, so `play_pos` ran ~18k frames ahead of the capture
    position (audible as distortion with a loopback-style monitor path). They are now counted as inserted silence, and the library rings are doubled at mid rates (`ring_frames = 16384 * M3_RATE_MULT`).
  - **Start check.** At 88.2 / 96 kHz the start-up under/overruns exceed the absolute thresholds; the check takes a baseline at 2 s and judges the growth by 6 s.
- Open: `off-cursor cycles` in the plug-in stats grow ~200/s at 96 kHz (sound is clean); 176.4 / 192 kHz (high class) are not supported.
- Direct test without CoreAudio: `build/mbox3_demo tone 6 440 rate=96000` with the daemon stopped.

## 31. `clock did not lock` on the first start after the daemon (case record)

- Symptom: the first start after every daemon (re)start failed with `clock did not lock`; the second one worked, so the sound began about 6 s late.
  Reproduced 4 times out of 4 with `launchctl kickstart -k`, and also right after `install.sh`.
- Library log at the failure: `status 0x00000100 (locked 0, rate index 1, wanted 1)`: the device runs at the right rate but does not lock to our
  transmit stream (the 0 received frames say nothing: before the lock the device only sends no-data packets). A start that works locks 806-821 ms after the clock
  switch (`LOCK_MIN_WAIT_MS` 500 + `LOCK_SETTLE_MS` 300), so the 4 s timeout was too long; it is 2.5 s now.
- Not the cause: the stream config snapshot (identical in failing and working starts), the sample rate before the restart (44.1 -> 44.1 failed as well), the XPC peer check.
- What decides it: the time since the previous process that used the device ended. After 30 s without any program, the first start worked. Several
  `mbox3_demo` runs one after another, typed by hand with some seconds in between, worked too. A switch of the sample rate inside the daemon (close and reopen, 500 ms) does not
  trigger it, so it is the death of the process (the kernel's cleanup of its FireWire resources), not the release of the ownership. The exact mechanism inside the device is not known.
- Handling: nothing is needed beyond the retry, which the daemon does 0.5 s after a lock failure. Waiting before the first start would cost the same time as the failed attempt.
  The log now says so (`the device often needs a few seconds ...`).

## 32. Low-latency defaults (v0.2.0)

- Request: the DAW round trip was about 86 ms and felt too slow for software monitoring.
- Where the time went (44.1 kHz, frames): output = prefill 768 + send look-ahead 224 slots x 5.5 + converters = about 2050 (46 ms); output safety offset 1024; input = 128 + safety offset 512.
- Changes: prefill 384, queue 320, send look-ahead 64 slots (output about 786 frames, 17 ms); safety offsets 192 (output) and 192 (input). `M3_OUT_LATENCY_FOR` now reports prefill + rate x 384 / 44100 (768 frames at 44.1 kHz), so the DAW's latency compensation matches the real value.
- Hardware tests (Intel MacBook Air 2017, 44.1 kHz): `768 512 128`, `640 448 112`, `512 384 96` (recording stable) and `384 320 64` (playback and recording clean by ear, `late 0`) were fine. `256 256 48` gave slight noise on playback. An input safety offset of 96 gave noise on recording; 192 did not.
- Not verified with the new defaults: 48 / 88.2 / 96 kHz (the constants scale with the rate class), other Macs, long sessions (the 600 s statistics show `late 0`).
- Pro Tools input monitoring of a record-armed track crackled at every setting (also at a 256-frame buffer) while the recorded file was clean: it does not depend on the driver's buffers, so it is treated as a property of that monitoring path. Direct monitoring in the control panel is not affected.
- Going back: `scripts/tune.sh 768 512 224` (the reported latency stays at the new value, so the DAW's compensation is about 1250 frames too small then).

## 33. Apple Silicon beta (v0.3.0-beta.1)

- Request: make the driver usable on Apple Silicon. There is no Apple Silicon Mac with a FireWire path available to the author, so this is a build for testers, not a verified port.
- Checked in the sources: time is taken with `mach_timebase_info` / `CLOCK_UPTIME_RAW` (no Intel-only 1:1 timebase assumption); DMA buffers are aligned with `getpagesize()` (16 KB pages on Apple Silicon); no CPU-specific assembler.
- Changed: `scripts/make_pkg.sh` builds each architecture alone (`cc -arch x86_64`, `cc -arch arm64`), joins the binaries with `lipo` and signs them ad hoc (arm64 code must be signed); `hostArchitectures` is `x86_64,arm64`. A source build on an Apple Silicon Mac is native arm64 and needs no change.
- Memory ordering: x86 keeps loads and stores in order, arm64 does not. `atomic_thread_fence` calls were added where the library reads what the FireWire DMA wrote (after the receive header check, after the transmit time stamp) and before a transmit payload becomes reachable. These are precautions, not a fix of an observed fault.
- Not known: whether the FireWire stack works through the Thunderbolt adapters on Apple Silicon at the macOS version in use (it is gone in macOS 26); whether the XPC peer check, the `coreaudiod` plug-in loading and the real-time thread priorities behave the same. The Intel behaviour was not changed.

## 34. Older macOS, and where this driver stands next to ASFireWire

- ASFireWire (https://www.asfw.net/, Apache 2.0, requires macOS 26) rebuilds the FireWire stack in DriverKit for Apple Silicon on macOS 26, where Apple removed it. This driver uses Apple's own stack (IOFireWireLib), so it cannot work on macOS 26 and does not compete there. Its niche: Intel Macs (also with OpenCore Legacy Patcher) and macOS 13 to 15 on Apple Silicon, with 88.2 / 96 kHz (ASFireWire documents 44.1 and 48 kHz).
- The package binaries are built with `-mmacosx-version-min=12.0` (`MINVER` in `scripts/make_pkg.sh`). macOS 12 and older were not tried. Possible problems: Apple's own or Avid's driver holding the device (the daemon would fail with `Open failed: 0xe00002c5`), and APIs newer than the version in use.
- Seen once (v0.3 development, Ventura on Intel): the daemon waited with `no FireWire device found` while `system_profiler SPFireWireDataType` printed nothing, `SPThunderboltDataType` still listed the adapter, and no FireWire controller was in `SPPCIDataType` or `ioreg`: the PCI Express tunnel behind the adapter was down. A restart of the Mac fixed it. It is not a driver fault and the daemon needs no change.

## 35. Round-trip latency: lead, drift and resume (v0.3.0-beta.2)

- Request: the measured round trip (Oblique RTL Utility, buffer 128) was about 3000 samples (68 ms), far above the reported 1536.
- Findings (`m3_shm_check watch`): the output cursor sat about 1350 frames ahead of the daemon (design about 500) because it was placed at CoreAudio's own time; and both cursors drifted by 3 to 5 frames a second (the cursor followed CoreAudio's time while the device clock runs slightly differently), so the round trip grew by roughly 4 ms a minute until a jump.
- Changes (`driver/Mbox3Driver.c`, `driver/proto.h`, `daemon/mbox3d.c`): the output cursor is placed `OutLead` ahead of the daemon (`frames + output safety + (prefill - queue) + write slack`); the daemon publishes the extra part (`out_lead_extra`) and `scripts/tune.sh [PREFILL QUEUE [AHEAD [SLACK]]]` sets the write slack (default 128 frames). The slow stretch controller now holds the output lead over the daemon at the value it settled at, not the offset to CoreAudio's time. The input cursor has the same kind of controller that holds the read margin at a target (192 + 64 frames at 44.1 kHz). If a block arrives more than one block late (play restarted after a pause), the cursor is re-placed at once instead of after three late blocks.
- Measured (Intel MacBook Air 2017, 44.1 kHz, buffer 128): round trip about 1430 to 1630 samples (33 to 37 ms) instead of about 3000; five consecutive measurements within 10 samples; write margin about 300, read margin about 145 (minimum per second), flat for 90 s; no late writes or reads in steady state.
- Seen: a dropout when YouTube comments scrolled on screen (the Mac's CPU/GPU load, not the driver; gone with comments off). The value changes between sessions by up to about 300 samples (1426 to 2006 seen); the cause is not found yet.
- Not verified: 48 / 88.2 / 96 kHz, buffers other than 128 and 512, long sessions, Apple Silicon.
