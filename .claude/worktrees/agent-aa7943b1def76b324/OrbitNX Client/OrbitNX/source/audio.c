#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <inttypes.h>
#include <switch.h>

#include <libavformat/avio.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <opus/opus.h>

#include "network.h"
#include "logger.h"

/// RTP/Opus receive, via ffmpeg's `avio` udp:// protocol directly, RTP parsed and Opus-decoded by
/// hand, using `avio_read_partial()` rather than `avio_read()`. `avio_read()` is a *stream* read -
/// internally it loops, calling the underlying protocol's read function as many times as it takes
/// to fill the entire requested size. For UDP that's wrong: a single real recvfrom() already gives
/// one whole RTP packet, and looping for more on a slow/bursty sender can hand back a buffer still
/// full of leftover/uninitialized memory. `avio_read_partial()` reads *at most* the requested size
/// but returns as soon as the underlying read call produces anything, matching one-packet-per-call
/// semantics. recv_buf is page-aligned via memalign rather than plain malloc.

// More queued output buffers give the real-time audio thread more slack to absorb a scheduling
// delay before an underrun is audible, at the cost of a little more worst-case latency.
#define BUF_COUNT 10
AudioOutBuffer audiobuf[BUF_COUNT];
u8 *buf_data[BUF_COUNT];
int curBuf = 0;
#define swapbuf (curBuf = (curBuf + 1) % (BUF_COUNT))

AudioOutBuffer *audout_released_buf;
int audout_filled = 0;

void play_buf(int buffer_size, int data_size)
{
    if (audout_filled >= BUF_COUNT)
    {
        u32 released_count;
        audoutWaitPlayFinish(&audout_released_buf, &released_count, UINT64_MAX);
    }

    audiobuf[curBuf].next = 0;
    audiobuf[curBuf].buffer = buf_data[curBuf];
    audiobuf[curBuf].buffer_size = buffer_size;
    audiobuf[curBuf].data_size = data_size;
    audiobuf[curBuf].data_offset = 0;
    audoutAppendAudioOutBuffer(&audiobuf[curBuf]);

    audout_filled++;
    swapbuf;
}

// Must agree exactly with what AudioCaptureSender.cs/RtpPacketizer.cs actually send.
#define OPUS_RTP_PAYLOAD_TYPE 97
#define OPUS_SAMPLE_RATE 48000
#define OPUS_CHANNELS 2
#define RTP_HEADER_SIZE 12
// Not part of real RTP - see RtpPacketizer.cs's doc comment. A single avio_read_partial() call can
// return more than one RTP/Opus packet back to back, a genuine property of this platform's UDP
// path - this 2-byte big-endian payload length, written right after the 12-byte RTP header, is
// what lets parse_rtp_unit below walk a single read as however many complete units actually fit
// in it instead of assuming exactly one packet per read.
#define LENGTH_PREFIX_SIZE 2
#define RTP_UNIT_HEADER_SIZE (RTP_HEADER_SIZE + LENGTH_PREFIX_SIZE)
// Comfortably above any Opus frame size this app actually produces (20ms stereo frames top out
// well under 1000 bytes at any sane bitrate), with room for several of them to be coalesced into
// one read (see above). Exactly one page (0x1000) - see this file's top doc comment for why
// recv_buf is page-aligned; sizing it to exactly one page keeps that simple rather than spanning
// into a second page.
#define MAX_RTP_PACKET_SIZE 0x1000

typedef struct
{
    u8 payload_type;
    u16 sequence_number;
    u16 payload_length;
} RtpHeader;

// Parses one [12-byte RTP header][2-byte length][payload] unit starting at buf. Returns the total
// number of bytes this unit occupies (RTP_UNIT_HEADER_SIZE + payload length) on success, so the
// caller can advance to the next unit in the same read - or -1 if this isn't a well-formed unit: a
// version/flags byte other than 0x80 (no padding, no extension, no CSRC - this app never sets any
// of those), a payload type other than the fixed one RtpPacketizer.cs always uses, or a declared
// payload length that doesn't actually fit in the remaining bytes (a torn/partial unit, or garbage
// masquerading as one) all indicate this isn't safe to decode.
static int parse_rtp_unit(const u8 *buf, int len, RtpHeader *out)
{
    if (len < RTP_UNIT_HEADER_SIZE)
        return -1;
    if (buf[0] != 0x80)
        return -1;
    if ((buf[1] & 0x7f) != OPUS_RTP_PAYLOAD_TYPE)
        return -1;

    u16 payload_length = ((u16)buf[12] << 8) | buf[13];
    if (RTP_UNIT_HEADER_SIZE + (int)payload_length > len)
        return -1;

    out->payload_type = buf[1] & 0x7f;
    out->sequence_number = ((u16)buf[2] << 8) | buf[3];
    out->payload_length = payload_length;
    return RTP_UNIT_HEADER_SIZE + payload_length;
}

// How long the read loop can go without successfully decoding+playing a single packet before the
// watchdog gives up on this session and forces a reopen. Well above ordinary jitter (heartbeats
// elsewhere in this app use a 5s window and see plenty of healthy gaps shorter than that), well
// below the worst avformat-jitter-buffer stalls seen before this rewrite (up to ~60s).
#define WATCHDOG_TIMEOUT_S 3.0

// One receive session, start to finish - opens the UDP socket via avio, decodes until a fatal I/O
// error or the watchdog above decides it's stalled, then falls through to be reopened by the
// caller. Returns 1 if the socket actually opened and a receive loop ran (however briefly), 0 if
// opening itself failed - distinguishes "session ended, try again immediately" from "nothing is
// listening yet, don't spin".
static int run_audio_session(u32 buffer_size)
{
    int ran = 0;
    const char *fail_reason = NULL;
    AVIOContext *avio_ctx = NULL;
    OpusDecoder *dec = NULL;
    u8 *recv_buf = NULL;
    opus_int16 *pcm_out = NULL;
    int pcm_out_capacity_samples = 0;

    // Page-aligned - see this file's top doc comment. memalign (not av_malloc/malloc) to make that
    // guarantee explicit and match the exact pattern already proven for buf_data below.
    recv_buf = memalign(0x1000, MAX_RTP_PACKET_SIZE);
    if (recv_buf == NULL)
    {
        fail_reason = "memalign(recv_buf) failed";
        goto done;
    }

    // No "timeout" option here (unlike the original version of this code): live nxlink debugging
    // showed it made avio_read_partial return a hard AVERROR(EIO) on literally every call, forever,
    // even while the host was actively sending real data - not the "nothing arrived within the
    // window" EAGAIN/ETIMEDOUT this option is documented to produce. Whatever this platform's udp.c
    // does internally to implement that timeout (likely poll()/select() under the hood) doesn't work
    // right against libnx's bsd service. Without it, avio_read_partial just blocks until a real
    // recvfrom() returns - fine here since the host genuinely is sending; the watchdog below still
    // catches a session that's truly never going to receive anything.
    // Deliberately no fifo_size option here (unlike video.c's URL): setting it switches ffmpeg's
    // udp.c protocol into its threaded circular-buffer mode, which on this platform corrupts RTP
    // headers on a large fraction of reads. This uses the simple synchronous
    // one-recvfrom()-per-read path instead.
    AVDictionary *opts = NULL;

    char url[64];
    snprintf(url, sizeof(url), "udp://0.0.0.0:%d", AUDIO_PORT);

    orbitnx_log("audio", "calling avio_open2(%s)", url);
    int open_ret = avio_open2(&avio_ctx, url, AVIO_FLAG_READ, NULL, &opts);
    av_dict_free(&opts);
    if (open_ret < 0)
    {
        char errbuf[64];
        av_strerror(open_ret, errbuf, sizeof(errbuf));
        orbitnx_log("audio", "avio_open2 failed: %s", errbuf);
        fail_reason = "avio_open2 failed";
        goto done;
    }
    orbitnx_log("audio", "avio_open2 returned successfully");

    int opus_err = 0;
    dec = opus_decoder_create(OPUS_SAMPLE_RATE, OPUS_CHANNELS, &opus_err);
    if (dec == NULL || opus_err != OPUS_OK)
    {
        orbitnx_log("audio", "opus_decoder_create failed: %s", opus_strerror(opus_err));
        fail_reason = "opus_decoder_create failed";
        goto done;
    }

    // Max Opus frame at 48kHz stereo is 120ms = 5760 samples/channel - size the scratch buffer for
    // that regardless of what frame size this app actually uses so opus_decode never overflows it.
    pcm_out_capacity_samples = 5760 * OPUS_CHANNELS;
    pcm_out = av_malloc(pcm_out_capacity_samples * sizeof(opus_int16));
    if (pcm_out == NULL)
    {
        fail_reason = "av_malloc(pcm_out) failed";
        goto done;
    }

    ran = 1;
    orbitnx_log("audio", "session opened: udp://0.0.0.0:%d, opus %dHz/%dch", AUDIO_PORT, OPUS_SAMPLE_RATE, OPUS_CHANNELS);

    bool have_sequence = false;
    u16 expected_sequence = 0;
    u64 packets_played_this_period = 0;
    u64 packets_dropped_this_period = 0;
    u64 packets_lost_this_period = 0;
    u64 last_heartbeat_tick = armGetSystemTick();
    u64 last_progress_tick = armGetSystemTick();
    bool watchdog_triggered = false;
    bool fatal_error = false;
    // The host can take a few seconds after this session opens before it actually starts sending.
    // The watchdog below must not punish that as a stall - it only starts counting once real audio
    // has actually flowed at least once.
    bool ever_played_a_packet = false;
    int consecutive_lossy_heartbeats = 0;
#define AUDIO_LOSSY_HEARTBEATS_BEFORE_RESTART 2

    while (appletMainLoop())
    {
        // avio_read_partial, NOT avio_read - see this file's top doc comment. avio_read loops
        // internally trying to fill the entire buffer across multiple underlying reads, which is
        // exactly wrong for a datagram socket; avio_read_partial stops after the first underlying
        // read produces anything, preserving one-packet-per-call semantics.
        int n = avio_read_partial(avio_ctx, recv_buf, MAX_RTP_PACKET_SIZE);
        if (n == AVERROR(EAGAIN) || n == AVERROR(ETIMEDOUT))
        {
            // Nothing arrived within the "timeout" option's window above - not an error, just an
            // opportunity to run the watchdog/heartbeat checks below on schedule even during a
            // genuine lull.
        }
        else if (n < 0)
        {
            // Root cause of a prior "audio dies ~0.8s in, forever" bug was the "timeout" AVOption
            // above (now removed) making avio_read_partial return AVERROR(EIO) on every call under
            // this platform's bsd service instead of the documented EAGAIN/ETIMEDOUT - so this branch
            // shouldn't fire in normal operation anymore. Kept non-fatal (no close/reopen) regardless:
            // avio_closep() on an avio_ctx that just hit that EIO was confirmed to hang forever, so
            // retrying reads on the same still-open avio_ctx is cheap insurance against hitting that
            // hang again for any other reason, known or not.
            static u64 s_last_logged_tick = 0;
            u64 now_tick_err = armGetSystemTick();
            if (armTicksToNs(now_tick_err - s_last_logged_tick) >= 1000000000ULL) // 1s, rate-limited
            {
                char errbuf[64];
                av_strerror(n, errbuf, sizeof(errbuf));
                orbitnx_log("audio", "avio_read_partial non-fatal error (retrying): %s (code=%d), ever_played=%d",
                           errbuf, n, ever_played_a_packet);
                s_last_logged_tick = now_tick_err;
            }
            svcSleepThread(20000000ULL); // 20ms backoff - avoid hot-looping if this does recur
        }
        else if (n > 0)
        {
            // One avio_read_partial call can return more than one coalesced [header][length]
            // [payload] unit - see LENGTH_PREFIX_SIZE's doc comment above. Walk all of them.
            int offset = 0;
            while (offset < n)
            {
                RtpHeader hdr;
                int unit_len = parse_rtp_unit(recv_buf + offset, n - offset, &hdr);
                if (unit_len < 0)
                {
                    packets_dropped_this_period++;
                    // No length to trust here - the rest of this read can't be reliably resynced,
                    // so stop rather than risk misinterpreting garbage as a unit boundary. No
                    // per-drop throttling: these can come in bursts of 20+ between real packets, so
                    // a fixed sleep here would serialize onto the burst and add real latency.
                    break;
                }

                int payload_offset = offset + RTP_UNIT_HEADER_SIZE;
                int payload_len = hdr.payload_length;

                if (have_sequence && hdr.sequence_number != expected_sequence)
                {
                    // Bounded: a large jump (reconnect, or the sequence counter wrapping past a
                    // huge gap after this app was suspended/backgrounded) shouldn't spend seconds
                    // synthesizing concealment for a gap that was never real audio to begin with.
                    u16 gap = hdr.sequence_number - expected_sequence;
                    int lost = (gap < 50) ? gap : 0;
                    for (int i = 0; i < lost; i++)
                    {
                        int lost_samples = opus_decode(dec, NULL, 0, pcm_out, pcm_out_capacity_samples / OPUS_CHANNELS, 0);
                        if (lost_samples > 0)
                        {
                            int data_size = lost_samples * OPUS_CHANNELS * (int)sizeof(opus_int16);
                            if (data_size <= (int)buffer_size)
                            {
                                memcpy(buf_data[curBuf], pcm_out, data_size);
                                play_buf(buffer_size, data_size);
                            }
                        }
                    }
                    packets_lost_this_period += lost;
                }

                int decoded_samples = opus_decode(dec, recv_buf + payload_offset, payload_len,
                                                    pcm_out, pcm_out_capacity_samples / OPUS_CHANNELS, 0);
                if (decoded_samples > 0)
                {
                    int data_size = decoded_samples * OPUS_CHANNELS * (int)sizeof(opus_int16);
                    if (data_size <= (int)buffer_size)
                    {
                        memcpy(buf_data[curBuf], pcm_out, data_size);
                        play_buf(buffer_size, data_size);
                        packets_played_this_period++;
                        last_progress_tick = armGetSystemTick();
                        ever_played_a_packet = true;
                    }
                    else
                    {
                        // Unlike the bad-rtp-unit case above, this is a well-formed unit that
                        // legitimately decoded - rare enough (never actually the high-frequency
                        // path) that logging every occurrence isn't the same hazard.
                        packets_dropped_this_period++;
                        orbitnx_log("audio", "drop: decoded_samples=%d too large, data_size=%d > buffer_size=%u",
                                   decoded_samples, data_size, buffer_size);
                    }
                }
                else
                {
                    packets_dropped_this_period++;
                    orbitnx_log("audio", "drop: opus_decode failed: %s (payload_len=%d, seq=%u)",
                               opus_strerror(decoded_samples), payload_len, hdr.sequence_number);
                }

                expected_sequence = hdr.sequence_number + 1;
                have_sequence = true;
                offset += unit_len;
            }
        }

        // Watchdog: see WATCHDOG_TIMEOUT_S and this file's top doc comment - forces this session
        // closed if too long passes without successfully playing a single packet.
        u64 now_tick = armGetSystemTick();
        double since_progress_s = (double)armTicksToNs(now_tick - last_progress_tick) / 1e9;
        if (ever_played_a_packet && since_progress_s >= WATCHDOG_TIMEOUT_S)
        {
            orbitnx_log("audio", "watchdog: no packet played in %.1fs, forcing session restart", since_progress_s);
            watchdog_triggered = true;
            break;
        }

        double elapsed_s = (double)armTicksToNs(now_tick - last_heartbeat_tick) / 1e9;
        if (elapsed_s >= 5.0)
        {
            orbitnx_log("audio", "heartbeat: %llu played, %llu dropped, %llu concealed(lost), in last %.1fs",
                       (unsigned long long)packets_played_this_period, (unsigned long long)packets_dropped_this_period,
                       (unsigned long long)packets_lost_this_period, elapsed_s);

            // Separate from the "no packet played at all" watchdog above: audio can keep playing
            // *something* every heartbeat (so that watchdog never fires) while still showing real,
            // audible loss (packets_lost_this_period - genuine concealed gaps, not the harmless
            // high-frequency "dropped" garbage reads). Mirrors video.c's approach: a few consecutive
            // heartbeats with real loss forces a clean reopen.
            if (packets_lost_this_period > 0)
                consecutive_lossy_heartbeats++;
            else
                consecutive_lossy_heartbeats = 0;

            if (consecutive_lossy_heartbeats >= AUDIO_LOSSY_HEARTBEATS_BEFORE_RESTART)
            {
                orbitnx_log("audio", "watchdog: real packet loss for %d consecutive heartbeats, forcing session restart",
                           consecutive_lossy_heartbeats);
                watchdog_triggered = true;
                packets_played_this_period = 0;
                packets_dropped_this_period = 0;
                packets_lost_this_period = 0;
                last_heartbeat_tick = now_tick;
                break;
            }

            packets_played_this_period = 0;
            packets_dropped_this_period = 0;
            packets_lost_this_period = 0;
            last_heartbeat_tick = now_tick;
        }
    }

    if (ran && !watchdog_triggered && !fatal_error)
        orbitnx_log("audio", "session ending: application closing");

done:
    if (!ran && fail_reason != NULL)
        orbitnx_log("audio", "session setup failed: %s", fail_reason);
    av_free(pcm_out);
    if (dec != NULL)
        opus_decoder_destroy(dec);
    free(recv_buf);
    if (avio_ctx != NULL)
    {
        orbitnx_log("audio", "calling avio_closep");
        avio_closep(&avio_ctx);
        orbitnx_log("audio", "avio_closep returned");
    }
    return ran;
}

void audioHandlerLoop(void *dummy)
{
    // A 20ms/960-sample stereo s16 Opus frame is 3840 bytes, and the worst case (120ms PLC
    // concealment burst) is 23040 bytes - comfortable headroom over that either way.
    u32 buffer_size = (24576 + 0xfff) & ~0xfff;

    for (int i = 0; i < BUF_COUNT; i++)
        buf_data[i] = memalign(0x1000, buffer_size);

    while (appletMainLoop())
    {
        int session_ran = run_audio_session(buffer_size);
        if (!session_ran)
        {
            // Opening the socket itself failed rather than a session that opened and later ended -
            // avoid spinning a hot open/fail loop while waiting for the host to start.
            svcSleepThread(1000000000ULL); // 1s
        }
        else
        {
            // Reopening with no delay at all here can span expensive re-init calls back to back
            // tightly enough to visibly starve the video decode thread running concurrently. A
            // brief pause costs a bit of extra reconnect latency but avoids that CPU thrashing.
            svcSleepThread(200000000ULL); // 200ms
        }
    }

    for (int i = 0; i < BUF_COUNT; i++)
        free(buf_data[i]);
}
