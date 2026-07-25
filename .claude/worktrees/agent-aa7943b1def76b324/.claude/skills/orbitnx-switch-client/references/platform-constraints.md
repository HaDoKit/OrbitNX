# Platform constraints (devkitPro / libnx / real Switch hardware)

These are constraints specific to running on actual Switch hardware with a
homebrew toolchain — they don't have an obvious analogue on a normal desktop C
target, and several were tuned empirically. Don't change these values without
understanding why they're set the way they are.

## Fixed buffers over dynamic allocation

`RenderContext` (`context.h`) uses fixed-size arrays sized for the app's fixed
1280×720 resolution, not dynamically allocated per session:

- `YPlane[RESX*RESY]`
- `UPlane[RESX*RESY/4]`, `VPlane[RESX*RESY/4]`
- `UVPlane[RESX*RESY/2]` (for the NV12/hardware path)

This avoids allocation churn on every frame in a memory-constrained embedded
environment. If resolution ever becomes dynamic, this is a structural change,
not a one-line tweak.

## Memory alignment for audio buffers

Audio's `recv_buf` and the `BUF_COUNT` (10) output buffers use
`memalign(0x1000, ...)` — page-aligned allocation. This was originally added
under a cache-coherency theory that has since been debunked, but is kept
deliberately as "cheap insurance" per the source comment — the alignment costs
effectively nothing and removing it has no benefit, only downside risk if the
original theory turns out to be partially right after all. Don't remove it
opportunistically.

## FFmpeg-owned buffers

Video decode buffers (`av_malloc`/`av_frame_alloc`/`av_image_alloc`) are
FFmpeg-owned and must be explicitly freed on every reconnect. `handleVid`
(`video.c`) frees `AVFormatContext`/`AVCodecContext`/`hw_device_ctx` per
reconnect after a confirmed-live leak: not freeing these consistently across
reconnects exhausted process memory and, notably, starved the **audio** thread
specifically (cross-subsystem effect from a video-side leak) — a reminder that
memory pressure on this platform can manifest as a failure in an unrelated
subsystem. Any new reconnect/retry path must free everything it allocated
before reopening.

## `SocketInitConfig`

TCP tx/rx buffers: `0x80000`/`0x100000` (max `0x400000`). UDP tx/rx:
`0x1400`/`0x3500`. `num_bsd_sessions = 6`, raised from the libnx default of 3 —
a source comment explains this headroom exists because video, audio, and input
each own a blocking-socket thread concurrently, and the default of 3 sessions
wasn't enough for that. If you add a fourth network-owning thread, this may need
to increase again.

## Thread stack/priority/core tuning

| Thread | Stack | Priority | Core |
|---|---|---|---|
| `renderThread` | `0x800000` | `0x2b` | 2 |
| `audioHandlerThread` | `0x10000` | `0x20` | 1 |
| `inputHandlerThread` | `0x1000` | `0x2b` | 0 |

These values were tuned empirically for exactly these three threads' actual
workloads (video decode needs a large stack for FFmpeg's internal state; input
needs almost none). A source comment notes `audioHandlerThread`'s priority is
deliberately "preemptive" relative to the others. **Don't copy one of these
values for a new thread without reasoning about that thread's actual
stack/latency needs** — an oversized stack wastes scarce memory, an undersized
one crashes, and priority/core placement affects real-time behavior on a
4-core console shared with the OS itself.

## CPU overclock

`clkrstSetClockRate(&cpuSession, 1785000000)` at init — raises CPU clock above
stock, monitored live via `clkrstGetClockRate` every 5-second heartbeat
alongside the decode fps watchdog (see `references/decode-pipeline.md`). This
exists specifically because software-path H.264 decode was found to be too
CPU-heavy at stock clock to sustain target frame rate. If touching decode
performance, be aware the overclock is already part of the baseline the current
performance numbers assume.

## No RTC / no reliable wall clock

Logging (`orbitnx_log`, see main `SKILL.md`) timestamps as seconds-since-init, not
wall-clock time — there's no reliably available real-time clock to use.
Don't assume log timestamps are comparable to PC-side wall-clock log timestamps
without accounting for this when correlating the two logs across repos.
