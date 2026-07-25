# Rendering and input capture

Files: `source/renderer.c`, `source/input.c`.

## Rendering (`renderer.c`)

SDL2 texture streaming — not a direct deko3d/NVN renderer.
`SDL_CreateRenderer(..., SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC)`,
fixed 1280×720 (`RESX`/`RESY` in `context.h`), fullscreen window. There is no
dynamic docked/handheld resolution-scaling logic in this codebase — the
resolution is fixed regardless of dock state.

Two textures, selected by the decoded frame's pixel format:

- `yuv_text` (`SDL_PIXELFORMAT_IYUV`) — for the software-decode path
  (3-plane YUV420P).
- `nv12_text` (`SDL_PIXELFORMAT_NV12`) — for the NVTEGRA hardware-decode path
  (see `references/decode-pipeline.md`).

`handleFrame` picks the path based on `frame->format`, copying via a
stride-aware `copyPlane()` helper — this is necessary because hardware-decoded
planes may be padded to a stride wider than the logical width, unlike the flat
`memcpy` used on the fixed-size software-decode path. **Don't collapse these two
copy paths into one `memcpy`** — the hardware path's padding would corrupt the
image if copied as if it were tightly packed.

`displayFrame()`: `checkFrameAvail()` → `SDL_UpdateYUVTexture` /
`SDL_UpdateNVTexture` (matching whichever texture is active) → `SDL_RenderCopy`
→ `SDL_RenderPresent`. Presentation is vsync-paced via
`SDL_RENDERER_PRESENTVSYNC`. FPS is computed every 60 frames via
`armGetSystemTick`/`svcGetSystemTick` deltas.

## Frame handoff synchronization

`RenderContext` (`context.h`) holds fixed-size plane buffers
(`YPlane`/`UPlane`/`VPlane`, sized for 1280×720 — see
`references/platform-constraints.md`) plus three `Mutex`es:
`texture_mut`, `frame_avail_mut`, `video_active_mut`. These guard the handoff
between the decode thread (writes a new frame) and the main thread's render
loop (reads and presents it) — this is the only cross-thread synchronization
point for video in the whole app.

## Input capture (`input.c`)

Reads up to 4 `PadState` via `padUpdate`/`padGetButtons`/`padGetStickPos`, touch
via `hidGetTouchScreenStates`, and accelerometer/gyro via
`hidGetSixAxisSensorStates`, on handles selected by controller style
(`Handheld`/`FullKey`/`JoyDual`, with explicit left/right disambiguation for
split Joy-Cons). This uses the standard `pad`/`hid` APIs — **not** `hidbus` —
don't introduce a `hidbus` dependency without a specific reason the higher-level
API can't satisfy.

Packed into the `JoyPkg` struct (see `references/network-protocol.md` for the
exact layout) and sent over the input thread's TCP connection at roughly a
23.3ms interval (`svcSleepThread(23333333ULL)`) — independent of, and not
locked to, the render frame rate.
