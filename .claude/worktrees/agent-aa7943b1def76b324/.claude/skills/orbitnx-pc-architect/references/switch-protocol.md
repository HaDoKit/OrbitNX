# Switch network protocol (PC side)

Counterpart repo: `SkyNX` (`C:\Users\mihail\visual code project\SkyNX`), a
devkitPro/libnx homebrew app running on the Nintendo Switch. That repo has its
own skill (`orbitnx-switch-client`) covering the C-side implementation in depth.
This file covers the protocol from the PC/C# side only.

## Ports and roles (`Config.cs`)

| Constant | Value | Transport | Who connects to whom |
|---|---|---|---|
| `VideoPort` | 2222 | UDP | PC sends to `ClientIp:VideoPort`, Switch listens passively |
| `AudioPort` | 2224 | UDP | PC sends to `ClientIp:AudioPort`, Switch listens passively |
| `HidPort` | 2223 | TCP | Switch binds+listens+accepts; PC dials out to `ClientIp:HidPort` |

Other constants: `TargetWidth=1280`, `TargetHeight=720`, `Fps=60`,
`BitrateKbps=4000`, `AudioSampleRate=48000` (comment: "matches the Switch's native
audout rate"), `AudioChannels=2`.

`SessionController.cs` only considers a session started once a real HID payload
has been parsed off the TCP connection — session start is gated on data, not just
a successful connect.

## Video (PC → Switch, UDP/MPEG-TS)

Entirely owned by `ffmpeg.exe` — see `references/ffmpeg-pipeline.md` for the full
encoder pipeline. The C# side does not touch frame data; ffmpeg captures, encodes,
and sends over UDP itself.

## Audio (PC → Switch, UDP/RTP)

WASAPI loopback capture → Opus encode (`Concentus`) → hand-rolled RTP framing
(`RtpPacketizer`) → raw UDP socket send. Independent of the video stream — no
shared clock, no muxing, no PC-side synchronization attempt. **Do not add sync
logic here** — the client is already responsible for handling two independent
streams; duplicating that on the PC side would be redundant and could conflict
with however the client already reconciles them.

## Input / HID (Switch → PC, TCP)

`Input/HidProtocol.cs` parses the incoming binary packet into a `ControllerState`
(up to 4 controllers: buttons, sticks, touch, accelerometer, gyro, frame rate).
`Input/VirtualGamepad.cs` (`VirtualGamepadHub`) replays parsed state as up to 4
virtual **Xbox 360** controllers via ViGEmBus.

- `SwapAbxy` (default `true`): the Switch's A/B/X/Y layout is diagonally mirrored
  relative to Xbox's — any button-mapping change must account for this.
- ViGEmBus was chosen after `ScpVBus` was found to fail to load consistently in
  live testing; ViGEmBus is actively maintained and used by comparable projects
  (Sunshine, DS4Windows).
- Touch coordinates arrive pre-normalized to a 1280×720 reference frame.

## ⚠️ Known unresolved discrepancy — verify before changing the packet format

Cross-repo research found a mismatch between what was inferred from this repo's
`HidProtocol.cs` and the actual C struct in the `SkyNX` client repo
(`source/network.h`, `JoyPkg`):

- **This repo's apparent assumption**: a 128-byte payload framed by an 8-byte
  `0xFF` start marker.
- **The client repo's actual struct** (`JoyPkg`): framed by two `u64` sentinel
  fields — `streamStart = UINT64_MAX` and `streamEnd = UINT64_MAX/2` — with no
  separate 8-byte marker, and a computed struct size of roughly **144 bytes**, not
  128.

This has **not been reconciled**. Do not assume either side is correct without
reading both `Input/HidProtocol.cs` in this repo and `source/network.h` +
`source/input.c` in the `SkyNX` repo side by side, field by field, before making
any change to packet layout, port numbers, or the sentinel/marker scheme. A
silent mismatch here fails at runtime, not at compile time — these two repos
share no build dependency.
