# Network protocol (Switch client side)

Counterpart repo: `OrbitNX` (`SteamShadowHost/OrbitNX`, C#/.NET
WinForms). That repo has its own skill (`orbitnx-pc-architect`) covering the
PC-side implementation. This file covers the protocol from the Switch/C side
only.

## Video (PC → Switch, UDP/MPEG-TS)

`network.h`:
```c
#define URL "udp://0.0.0.0:2222?overrun_nonfatal=1&fifo_size=100000"
```

This app is passive — it opens the stream via
`avformat_open_input(&fmt_ctx, URL, 0, &opts)` in `video.c`. Self-synchronizing
MPEG-TS demuxing (188-byte packets) handles UDP's inherent lack of framing.

UDP was chosen over plain TCP specifically because a raw H.264 Annex-B stream
over TCP was observed dying every ~7–20 seconds under load — documented in a
`network.h` comment. Don't revert to TCP for video without re-reading that
history.

## Audio (PC → Switch, UDP/RTP)

Opened via `avio_open2(&avio_ctx, "udp://0.0.0.0:2224", AVIO_FLAG_READ, ...)` in
`audio.c`. See `references/audio-pipeline.md` for the full parsing details —
this is hand-rolled RTP, not standard `avformat` RTP demuxing.

## Input / HID (Switch → PC, TCP)

**This app is the TCP server**: `connectJoyConSocket` (`network.c`) binds,
listens, and accepts on port 2223 (hardcoded literal in `input.c`, no named
constant). The PC connects in. This is confirmed independently from the C code
and matches the PC-side `SessionController`'s "dial out to
`ClientIp:HidPort`" behavior — **no discrepancy on connection direction**.

### `JoyPkg` wire format (`network.h`)

```c
typedef struct {
    uint64_t streamStart;      // sentinel: UINT64_MAX
    struct {
        uint32_t heldKeys;
        int32_t  lJoyX, lJoyY;
        int32_t  rJoyX, rJoyY;
    } controllers[4];
    uint32_t touchX1, touchY1, touchX2, touchY2;
    float    accelX, accelY, accelZ;
    float    gyroX, gyroY, gyroZ;
    uint32_t renderFPS;
    uint32_t controllerCount;
    uint64_t streamEnd;        // sentinel: UINT64_MAX / 2
} JoyPkg;
```

Sent raw via `send(sock, pkg, sizeof(JoyPkg), 0)` in `network.c` — no explicit
endianness handling, relies on matching struct layout between sender and
receiver. Computed size is roughly **144 bytes**. Framing uses the two `u64`
sentinel fields (`streamStart`/`streamEnd`), not a separate marker byte
sequence. Sent at roughly a 23.3ms interval (`svcSleepThread(23333333ULL)`),
i.e. close to but not locked to the render frame rate.

## ⚠️ Known unresolved discrepancy — verify before changing anything here

Cross-repo research found this C struct does **not** match what was inferred
from the PC-side `Input/HidProtocol.cs`:

- **This side's actual struct** (`JoyPkg`, above): sentinel-framed
  (`streamStart`/`streamEnd`), ~144 bytes.
- **PC side's apparent assumption**: a 128-byte payload framed by a distinct
  8-byte `0xFF` start marker.

This has **not been reconciled**. It may be an inaccurate reading of the C#
code, an actual protocol version drift between the two repos, or a PC-side
adaptation layer that wasn't found during research. **Before changing `JoyPkg`,
any port number, or the sentinel scheme**, read `source/network.h` and
`source/input.c` here side by side against
`OrbitNX/Input/HidProtocol.cs` in the PC repo, field by field. Do not
assume either side is currently correct.
