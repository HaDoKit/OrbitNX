# Video decode pipeline

File: `source/video.c`. Uses FFmpeg/libav (`libavformat`, `libavcodec`,
`libavutil/hwcontext_nvtegra.h`) — not a direct low-level libnx codec API call.

## Pipeline

1. `handleVid()` opens the input via `avformat_open_input` against the UDP URL
   from `network.h` (see `references/network-protocol.md`) — MPEG-TS demuxing.
2. `open_codec_context()` attempts **hardware decode first**:
   `av_hwdevice_ctx_create(&context->hw_device_ctx, AV_HWDEVICE_TYPE_NVTEGRA,
   NULL, NULL, 0)` — this is real Tegra hardware decode (NVDEC/VIC), with
   `get_hw_format` steering the codec to `AV_PIX_FMT_NVTEGRA`.
3. If hardware decode context creation fails, falls back to pure software
   decode. Software decode alone was found to be CPU-heavy enough to visibly
   degrade playback at the console's stock (non-overclocked) CPU frequency —
   part of why the CPU is overclocked at startup (see main `SKILL.md`).
4. Packets are pulled via `av_read_frame`, decoded via
   `avcodec_send_packet`/`avcodec_receive_frame` (`decode_frame`/
   `decode_packet`).
5. Hardware-decoded frames are copied out of VIC memory via
   `av_hwframe_transfer_data(context->sw_frame, context->frame, 0)`.

## `sw_frame` reuse — do not "fix" this

`sw_frame` is pre-allocated **once** and reused across calls rather than being
unreffed (`av_frame_unref`) and reallocated every frame. This looks like it
could be a resource-management smell, but it is a deliberate, measured fix:
unreffing every call capped video at ~39fps instead of a clean 60fps in live
testing. **Do not change this back to per-frame unref/realloc** without
re-measuring frame rate impact.

## fps watchdog — root cause not fully known

A per-session watchdog runs on the same 5s heartbeat as the CPU clock monitor:
tracks fps, and if it drops below 30.0 for 2 consecutive heartbeats, force-closes
and reopens the decode session.

The underlying slowdown this guards against has an **unresolved root cause**:
process memory usage stays completely flat while it happens, which rules out an
ordinary heap leak. The current working theory (not confirmed) is something in
the GPU/VIC hardware decode engine's own internal resource pool. If you're
investigating decode slowdowns, don't assume it's a straightforward memory leak
in this app's own code — that path has already been checked and ruled out.

## Rendering handoff

See `references/rendering-input.md` for how decoded frames (both the
hw-decode NV12 path and the software YUV420P path) get from this decode
pipeline into the SDL2 renderer, and the `Mutex`-based synchronization involved.
