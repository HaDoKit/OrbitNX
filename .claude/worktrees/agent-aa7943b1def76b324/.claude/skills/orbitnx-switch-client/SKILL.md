---
name: orbitnx-switch-client
description: This skill should be used when writing, reviewing, or reasoning about code in source/*.c or source/*.h in this repo — a Nintendo Switch homebrew app (devkitPro/libnx, C) that is the client side of a remote-play system whose PC-side host is "OrbitNX" (separate repo). Trigger on requests like "review this C code", "how does video decode work on the Switch side", "why does audio use avio_read_partial", "add a new thread", "change the JoyPkg struct", "why is NVTEGRA hw-decode failing", or any work touching video.c, audio.c, input.c, network.c, renderer.c, or main.c. This is Switch/libnx-specific systems C, not generic C advice.
---

# OrbitNX (Nintendo Switch client) — architecture and platform guide

## What this app actually is

OrbitNX is a homebrew application (devkitPro/libnx toolchain, builds a `.nro`) that
runs **on the Nintendo Switch itself**, acting as the client in a remote-play
system. The **PC-side host** is a separate repository, `OrbitNX`
(`SteamShadowHost/OrbitNX`, C#/.NET/WinForms), which has its own skill
(`orbitnx-pc-architect`) covering that side in depth.

Three independent, unsynchronized network streams from the PC:

| Stream | Transport | Port | Socket role on this side |
|---|---|---|---|
| Video | UDP (MPEG-TS) | 2222 | This app opens a passive UDP listener via `avformat_open_input` |
| Audio | UDP (RTP) | 2224 | This app opens a passive UDP reader via `avio_open2` |
| Input (HID) | TCP | 2223 | **This app binds+listens+accepts** (`connectJoyConSocket`) — the PC connects in |

See `references/network-protocol.md` for exact wire formats, and note the
**unresolved cross-repo discrepancy** flagged there before touching the `JoyPkg`
struct or any port constant.

## Toolchain

devkitPro/libnx (`switch_rules`), ARM `-march=armv8-a -mtune=cortex-a57
-mtp=soft -fPIE`, `-O3`. Key libraries (from the Makefile): SDL2 stack
(`SDL2, SDL2_ttf, SDL2_image, SDL2_mixer, SDL2_gfx, webp, jpeg`), GPU
(`glad, EGL, glapi, drm_nouveau`), FFmpeg (`avformat, avcodec, swresample,
swscale, avutil` + `vpx, dav1d, opus, ass, theora, vorbis`), text
(`freetype, harfbuzz, fribidi`), `nx` (libnx itself). **No mbedtls — this app
does not use TLS anywhere**, all networking is plain UDP/TCP.

## Threading model

Three independent libnx threads (`main.c`), each owning and blocking on its own
socket — there is no single event loop tying networking together:

| Thread | Stack | Priority | Core | Responsibility |
|---|---|---|---|---|
| `renderThread` (runs `videoLoop`) | `0x800000` | `0x2b` | 2 | Video receive + decode |
| `audioHandlerThread` | `0x10000` | `0x20` | 1 | Audio receive + decode + playback |
| `inputHandlerThread` | `0x1000` | `0x2b` | 0 | Controller/touch/gyro read + send |

`SocketInitConfig` raises `num_bsd_sessions` to 6 (from the libnx default of 3) —
specifically because these three threads each need their own concurrent
blocking-socket session; don't reduce this without accounting for that.

Decode-thread → render-thread handoff goes through a `Mutex`-guarded
`RenderContext` (`texture_mut`, `frame_avail_mut`, `video_active_mut`) — this is
the only cross-thread synchronization point for video; audio and input don't
share state with the other threads beyond their own independent sockets.

The main thread runs its own render/heartbeat loop (~70fps via
`svcSleepThread`), and pauses/resumes the three worker threads based on
`AppletFocusState` (e.g. when the Switch's home menu takes focus).

CPU is overclocked at init (`clkrstSetClockRate(&cpuSession, 1785000000)`),
monitored live every 5s heartbeat.

## Three hot areas — read the reference before touching these

- **Network protocol** → `references/network-protocol.md`. Includes the
  **unresolved discrepancy** with the PC-side `HidPacket` format — read before
  changing `JoyPkg` or any port.
- **Video decode** → `references/decode-pipeline.md`. NVTEGRA hardware decode
  path, software fallback, and an fps watchdog with an unresolved root cause —
  each already has documented failed-approach history.
- **Audio pipeline** → `references/audio-pipeline.md`. Hand-rolled RTP parsing
  with a specific, non-obvious reason for using `avio_read_partial` over
  `avio_read`, plus packet-loss concealment.

Also see `references/rendering-input.md` (SDL2 texture streaming, controller/
touch/gyro reading) and `references/platform-constraints.md` (fixed buffer
sizing, thread stack/priority tuning, memory alignment — all libnx/Switch
hardware-specific constraints that don't have obvious analogues on a normal
desktop target).

## Logging

`orbitnx_log(category, fmt, ...)` — printf-style, variadic, category string —
deliberately mirrors the PC-side `Logger.Log(category, message)` (this is stated
directly in a source comment: "mirrors the host's Logger.cs so the two logs can
be compared side by side"). Writes to `sdmc:/switch/OrbitNX/orbitnx.log`, timestamped
as seconds-since-init (there's no reliable RTC to use for wall-clock time).

Guarded by a `Mutex s_log_mutex`, added after unsynchronized `fprintf` calls
across 3+ threads were confirmed unsafe. The mutex itself is **rate-limited**
(`LOG_FLUSH_MIN_INTERVAL_NS = 200ms`) — an unthrottled version was found to
serialize the video decode thread against other threads' SD-card writes at
exactly the moments it could least afford to stall, collapsing frame rate.

**Don't remove the log mutex, and don't log unthrottled from the decode or audio
threads** — both changes have already caused a measured regression once.

## Project-specific review checklist

- **Never** change `JoyPkg`'s layout, size, or the `streamStart`/`streamEnd`
  sentinel values (`UINT64_MAX` / `UINT64_MAX/2`) without cross-checking against
  the PC-side `HidPacket`/`ControllerState` structs in the `OrbitNX` repo —
  see the documented discrepancy in `network-protocol.md`. These repos share no
  build dependency; a mismatch fails at runtime, not compile time.
- Don't remove `Mutex s_log_mutex`, and don't add unthrottled logging calls
  inside `videoLoop`/decode/audio hot paths — known fps-collapse regression.
- Don't change `sw_frame` handling in `video.c` from reuse-once-allocated back to
  unref-every-call — this was already tried and caused a measured 60fps→~39fps
  regression.
- Don't increase `fifo_size` or read-buffer sizes on the audio UDP URL without
  testing for RTP header corruption — already observed "on this platform's more
  limited threading" per the source comment in `audio.c`.
- Don't move video transport back to plain TCP without re-reading the documented
  history of connections dying every ~7–20 seconds under load.
- On any FFmpeg-context reconnect path (video or audio), ensure
  `AVFormatContext`/`AVCodecContext`/`hw_device_ctx` are freed consistently —
  a prior inconsistency here was a confirmed leak that starved the audio thread
  of memory.
- When adding a new thread, choose stack size/priority/core deliberately — the
  existing values (`0x800000`/`0x1000`/`0x10000` stacks; priorities `0x2b`/
  `0x20`; cores 0/1/2) were tuned empirically for exactly three threads sharing
  the console's four cores; don't copy a value without reasoning about the new
  thread's actual memory/latency needs.
- Any NVTEGRA hardware-decode change should account for the known fps-watchdog
  behavior in `decode-pipeline.md` — the root cause of the underlying slowdown
  it guards against was never fully pinned down.

## Reference files

- **`references/network-protocol.md`** — exact wire formats for video/audio/HID,
  socket roles, the unresolved cross-repo `JoyPkg` discrepancy
- **`references/decode-pipeline.md`** — FFmpeg + NVTEGRA hw-decode, software
  fallback, fps watchdog
- **`references/audio-pipeline.md`** — RTP parsing, `avio_read_partial` vs
  `avio_read`, Opus PLC, `audout` buffering
- **`references/rendering-input.md`** — SDL2 texture streaming (IYUV/NV12),
  controller/touch/gyro capture into `JoyPkg`
- **`references/platform-constraints.md`** — fixed buffer sizing, thread tuning,
  memory alignment, libnx-specific constraints
