# Audio pipeline

File: `source/audio.c`. Hand-rolled RTP parsing over a raw UDP `avio` context —
does **not** use avformat's built-in RTP demuxer.

## Why `avio_read_partial`, not `avio_read`

Opened via `avio_open2(&avio_ctx, "udp://0.0.0.0:2224", AVIO_FLAG_READ, ...)`,
then read via `avio_read_partial`. A source comment explains this directly:
`avio_read` loops internally to fill its entire requested buffer, which is the
**wrong** behavior for a datagram socket — it caused garbage reads by
potentially blocking across multiple independent UDP datagrams and returning
misaligned data. `avio_read_partial` returns whatever a single underlying read
produces, which is the correct match for UDP's message-boundary semantics.
**Don't "simplify" this back to `avio_read`** — it was already tried (this was
described as the "third attempt" at getting this right) and was wrong for this
exact reason.

## RTP framing — non-standard, by necessity

`parse_rtp_unit()` walks a custom wire format, because a single
`avio_read_partial()` call can return multiple coalesced RTP units in one
buffer:

```
[12-byte RTP header][2-byte big-endian payload length][Opus payload]
```

`RtpHeader` struct: `payload_type` (u8), `sequence_number` (u16),
`payload_length` (u16). Validation before treating a unit as valid: byte 0 must
be `0x80`, and `byte 1 & 0x7f` must equal `OPUS_RTP_PAYLOAD_TYPE` (97).

The 2-byte length prefix is **not** a standard RTP field — it's an
application-level addition specifically to let this code walk multiple
coalesced units out of one read. If you change the sender's framing on the PC
side, this parser must change in lockstep — there is no shared schema file
between the two repos for this.

## Decode and playback

`opus_decode` (libopus) decodes to PCM, queued via
`audoutAppendAudioOutBuffer` across `BUF_COUNT = 10` buffers (raised from an
original 5 specifically to give more slack absorbing a scheduling delay before
an underrun becomes audible — don't reduce this without accounting for that).
Sample rate/channels: 48000Hz / 2ch (`OPUS_SAMPLE_RATE`/`OPUS_CHANNELS`),
matching the PC-side `Config.cs` constants.

## Packet-loss concealment

On a detected sequence-number gap, calls Opus PLC via `opus_decode(dec, NULL, 0,
...)` once per lost packet, bounded to a gap size under 50 — beyond that, the
gap is assumed too large for PLC to meaningfully help and is treated
differently (session-health watchdog territory, below).

## Buffer sizing caution

Increasing `fifo_size` on the audio UDP URL, or any related buffer size, has
already been observed to cause **corrupted RTP headers** specifically on this
platform, per a direct comment in `audio.c` attributing it to "this platform's
more limited threading" compared to a desktop target. Test carefully before
changing buffer sizes here — a change that would be safe on a desktop UDP
consumer is not guaranteed safe on this platform.

## Watchdogs

- A 3-second no-progress watchdog forces a session reopen if no audio data
  arrives at all.
- A separate "lossy heartbeat" watchdog triggers a reopen after 2 consecutive
  5-second windows showing real packet loss (distinct from the no-progress
  case — this catches a degraded-but-still-flowing stream).

## Independence from video

There is no shared clock or muxing between the audio and video streams on
either side of this system — see `references/network-protocol.md` and the
PC-side `switch-protocol.md`. Any perceived audio/video sync issue is not a
missing-feature bug to "add sync logic" for; if sync drift is observed, it's a
timing/scheduling investigation on one or both independent pipelines, not a
missing synchronization primitive.
