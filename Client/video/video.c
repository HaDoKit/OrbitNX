#include "video.h"

#include <stdio.h>

#include <switch.h>
#include <SDL.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>

#include "../network/network.h"
#include "../common/logger/logger.h"

// Hardware H264 decode via NVTEGRA (the Tegra X1's NVDEC engine), through libavcodec's standard
// hwaccel API - devkitPro's ffmpeg portlib is built with --enable-nvtegra (confirmed directly in
// the installed libavutil.a: av_nvtegra_channel_open, ff_hwcontext_type_nvtegra, etc. are present
// as real exported symbols), so this needs no custom FFmpeg build.
//
// Decoded NVTEGRA frames live in Tegra-local hardware memory in NV12 layout and can't be handed
// to SDL2 directly - av_hwframe_transfer_data() reads them back into a normal system-memory NV12
// AVFrame first, which SDL2 renders via SDL_UpdateNVTexture(). This isn't zero-copy (a true
// zero-copy path would render the hardware surface directly via deko3d instead of SDL2, skipping
// this readback) - that's a possible later optimization once this path is proven; the readback
// still moves the actual decode work off the CPU, which is the expensive part.
//
// hw_device_ctx creation is attempted once in videoInit() and reused across reconnects (opening
// an NVDEC hardware channel isn't something to redo on every network hiccup). If it fails for any
// reason, get_format() below just never gets offered AV_PIX_FMT_NVTEGRA and libavcodec falls back
// to its normal software H264 decoder - the encoder's baseline profile (no B-frames, no CABAC;
// see OrbitNX/Capture/VideoEncoder.cs) keeps that fallback reasonably cheap too.

#define VIDEO_RECONNECT_DELAY_NS 1000000000ULL // 1s between reconnect attempts
#define VIDEO_HEARTBEAT_EVERY_FRAMES 300u       // ~once per 5s at 60fps

// fifo_size (the UDP receive fifo passed to avformat_open_input, in 188-byte MPEG-TS packets) is
// computed from the stream's actual bitrate instead of a hardcoded packet count, so it stays
// correctly sized if the host's encoder bitrate ever changes (OrbitNX/Config.cs's BitrateKbps).
// Sized in terms of a target buffering *duration*, not a fixed byte count - 300ms was confirmed
// live to be small enough to avoid the unbounded growing lag a much bigger fifo caused (100000
// packets/~18.8MB was ~37.6s of buffering, which just accumulated an ever-growing backlog once
// decode fell slightly behind real time - see NVTEGRA's cpu-transfer-fallback note on
// runStreamSession), while still absorbing ordinary network jitter.
#define VIDEO_BUFFER_TARGET_MS   300
#define VIDEO_MPEGTS_PACKET_SIZE 188
#define VIDEO_MIN_FIFO_PACKETS   100 // floor, in case a learned bitrate is implausibly low

// Chicken-and-egg: fifo_size has to be in the URL passed to avformat_open_input, before any video
// data (and therefore the real bitrate) has arrived. videoStart() calls
// networkWaitForSessionConfig() before every connection attempt (including the first), which
// blocks only until the *first* HID session ever completes its handshake - the host sends its
// real BitrateKbps as part of that (OrbitNX/Input/HidProtocol.cs's SessionConfig,
// SessionController.cs), so in practice this is known correctly before video's very first
// connection, not just from the second reconnect onward the way a purely-measured value would be.
// This constant only matters as a fallback if networkWaitForSessionConfig somehow times out
// without ever seeing a session (app exiting) - see its use in videoStart().
#define VIDEO_DEFAULT_BITRATE_KBPS 4000
static int s_lastKnownBitrateKbps = VIDEO_DEFAULT_BITRATE_KBPS;

// Bumped once per runStreamSession() return (stream dropped/failed to open) - see videoStart()'s
// loop. Read by input.c via videoGetReconnectCount() to stamp onto the outgoing HID packet; the
// host's AdaptiveBitrateController (OrbitNX/Session/AdaptiveBitrateController.cs) diffs this
// against its own last-seen value to detect video instability. Plain word, no locking - see
// network.h's telemetry field doc comment.
static uint32_t s_videoReconnectCount = 0;

uint32_t videoGetReconnectCount(void)
{
    return (uint32_t)s_videoReconnectCount;
}

uint32_t videoGetMeasuredBitrateKbps(void)
{
    return (uint32_t)s_lastKnownBitrateKbps;
}

static int computeFifoSizePackets(int bitrateKbps)
{
    double bytesPerSec = (double)bitrateKbps * 1000.0 / 8.0;
    double targetBytes = bytesPerSec * ((double)VIDEO_BUFFER_TARGET_MS / 1000.0);
    int packets = (int)(targetBytes / VIDEO_MPEGTS_PACKET_SIZE);
    return packets < VIDEO_MIN_FIFO_PACKETS ? VIDEO_MIN_FIFO_PACKETS : packets;
}

static SDL_Window *s_window = NULL;
static SDL_Renderer *s_renderer = NULL;
static SDL_Texture *s_texture = NULL;
static int s_textureWidth = 0;
static int s_textureHeight = 0;
static Uint32 s_textureFormat = 0;

static AVBufferRef *s_hwDeviceCtx = NULL;

bool videoInit(void)
{
    // See logger.h's doc comment on logInstallFfmpegCallback - unifies FFmpeg's own av_log output
    // with logWrite()'s mutex instead of racing it. Also called from audio.c's audioInit(); safe
    // to call from both, idempotent (av_log_set_callback just overwrites the same pointer).
    logInstallFfmpegCallback();

    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        logWrite("video", "SDL_Init failed: %s", SDL_GetError());
        return false;
    }

    s_window = SDL_CreateWindow("OrbitNX", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 720, SDL_WINDOW_SHOWN);
    if (s_window == NULL)
    {
        logWrite("video", "SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }

    s_renderer = SDL_CreateRenderer(s_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (s_renderer == NULL)
    {
        logWrite("video", "SDL_CreateRenderer failed: %s", SDL_GetError());
        return false;
    }

    avformat_network_init();

    int hwResult = av_hwdevice_ctx_create(&s_hwDeviceCtx, AV_HWDEVICE_TYPE_NVTEGRA, NULL, NULL, 0);
    if (hwResult < 0)
    {
        logWrite("video", "NVTEGRA hwaccel unavailable (%d) - falling back to software decode", hwResult);
        s_hwDeviceCtx = NULL;
    }
    else
    {
        logWrite("video", "NVTEGRA hwaccel ready");
    }

    logWrite("video", "SDL2 video ready");
    return true;
}

// (Re)creates the streaming texture only when the decoded frame size or pixel format actually
// changes, instead of trusting a hardcoded resolution/format - the host's Config.cs
// TargetWidth/Height is never actually transmitted on the wire, and whether NVTEGRA hwaccel is
// active can in principle differ session to session, so the decoded AVFrame is the only source of
// truth for both.
static bool ensureTexture(int width, int height, Uint32 sdlFormat)
{
    if (s_texture != NULL && width == s_textureWidth && height == s_textureHeight && sdlFormat == s_textureFormat)
        return true;

    if (s_texture != NULL)
    {
        SDL_DestroyTexture(s_texture);
        s_texture = NULL;
    }

    s_texture = SDL_CreateTexture(s_renderer, sdlFormat, SDL_TEXTUREACCESS_STREAMING, width, height);
    if (s_texture == NULL)
    {
        logWrite("video", "SDL_CreateTexture failed at %dx%d (format %u): %s", width, height, sdlFormat, SDL_GetError());
        return false;
    }

    s_textureWidth = width;
    s_textureHeight = height;
    s_textureFormat = sdlFormat;
    logWrite("video", "texture (re)created at %dx%d, format %u", width, height, sdlFormat);
    return true;
}

// Renders one decoded software-memory frame (already read back from hardware if it came from
// NVTEGRA - see runStreamSession). Handles the two layouts this app can actually produce: NV12
// from the hwaccel readback, or plain YUV420P from the software decoder fallback.
static void renderFrame(const AVFrame *swFrame, u32 *framesDecoded)
{
    if (swFrame->format == AV_PIX_FMT_NV12 && ensureTexture(swFrame->width, swFrame->height, SDL_PIXELFORMAT_NV12))
    {
        SDL_UpdateNVTexture(s_texture, NULL,
            swFrame->data[0], swFrame->linesize[0],
            swFrame->data[1], swFrame->linesize[1]);
    }
    else if (swFrame->format == AV_PIX_FMT_YUV420P && ensureTexture(swFrame->width, swFrame->height, SDL_PIXELFORMAT_IYUV))
    {
        SDL_UpdateYUVTexture(s_texture, NULL,
            swFrame->data[0], swFrame->linesize[0],
            swFrame->data[1], swFrame->linesize[1],
            swFrame->data[2], swFrame->linesize[2]);
    }
    else
    {
        logWrite("video", "unexpected pixel format %d (expected NV12 or YUV420P) - dropping frame", swFrame->format);
        return;
    }

    SDL_RenderClear(s_renderer);
    SDL_RenderCopy(s_renderer, s_texture, NULL, NULL);
    SDL_RenderPresent(s_renderer);

    (*framesDecoded)++;
    if (*framesDecoded % VIDEO_HEARTBEAT_EVERY_FRAMES == 0)
        logWrite("video", "connection stable - %u frames decoded", *framesDecoded);
}

static enum AVPixelFormat negotiatePixelFormat(AVCodecContext *ctx, const enum AVPixelFormat *pixFmts)
{
    (void)ctx;
    for (const enum AVPixelFormat *p = pixFmts; *p != AV_PIX_FMT_NONE; p++)
    {
        if (*p == AV_PIX_FMT_NVTEGRA)
            return *p;
    }
    return pixFmts[0]; // no NVTEGRA offered (hwaccel unavailable) - take the software format
}

// One connect-decode-render cycle; returns when the stream drops or the app is exiting, so the
// caller (videoStart) decides whether to reconnect.
static void runStreamSession(const char *url)
{
    AVFormatContext *fmtCtx = NULL;

    if (avformat_open_input(&fmtCtx, url, NULL, NULL) < 0)
    {
        logWrite("video", "avformat_open_input failed for %s", url);
        return;
    }

    // This stream carries H264 video only (audio is a separate UDP port with its own framing) and
    // the encoder sends a keyframe at least every ~0.5s (VideoEncoder.cs's -g setting) - keep
    // analysis short, there's only ever one stream to find.
    fmtCtx->flags |= AVFMT_FLAG_NOBUFFER;
    fmtCtx->probesize = 65536;
    fmtCtx->max_analyze_duration = 500000; // microseconds

    if (avformat_find_stream_info(fmtCtx, NULL) < 0)
    {
        logWrite("video", "avformat_find_stream_info failed");
        avformat_close_input(&fmtCtx);
        return;
    }

    int streamIndex = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (streamIndex < 0)
    {
        logWrite("video", "no video stream found in input");
        avformat_close_input(&fmtCtx);
        return;
    }

    AVCodecParameters *codecPar = fmtCtx->streams[streamIndex]->codecpar;

    // Corrects s_lastKnownBitrateKbps for the *next* reconnect's fifo_size, if avformat managed to
    // estimate a real bit_rate for this stream (it doesn't always - depends on how much it could
    // analyze during the probe). Doesn't affect the fifo_size already chosen for this session.
    if (codecPar->bit_rate > 0)
    {
        int measuredKbps = (int)(codecPar->bit_rate / 1000);
        if (measuredKbps != s_lastKnownBitrateKbps)
        {
            logWrite("video", "measured stream bitrate %d kbps (was assuming %d kbps) - will resize fifo on next reconnect",
                measuredKbps, s_lastKnownBitrateKbps);
            s_lastKnownBitrateKbps = measuredKbps;
        }
    }
    const AVCodec *decoder = avcodec_find_decoder(codecPar->codec_id);
    if (decoder == NULL)
    {
        logWrite("video", "no decoder for codec id %d", (int)codecPar->codec_id);
        avformat_close_input(&fmtCtx);
        return;
    }

    AVCodecContext *codecCtx = avcodec_alloc_context3(decoder);
    if (codecCtx == NULL || avcodec_parameters_to_context(codecCtx, codecPar) < 0)
    {
        logWrite("video", "decoder context setup failed");
        if (codecCtx != NULL)
            avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        return;
    }

    if (s_hwDeviceCtx != NULL)
    {
        codecCtx->hw_device_ctx = av_buffer_ref(s_hwDeviceCtx);
        codecCtx->get_format = negotiatePixelFormat;
    }

    if (avcodec_open2(codecCtx, decoder, NULL) < 0)
    {
        logWrite("video", "avcodec_open2 failed");
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        return;
    }

    logWrite("video", "stream connected - decoding with %s (%s)", decoder->name,
        s_hwDeviceCtx != NULL ? "NVTEGRA hwaccel" : "software");

    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    AVFrame *swFrame = av_frame_alloc(); // readback target for NVTEGRA frames only
    u32 framesDecoded = 0;

    while (appletMainLoop())
    {
        SDL_Event event;
        while (SDL_PollEvent(&event)) { /* no input handling yet - see input.c */ }

        int readResult = av_read_frame(fmtCtx, packet);
        if (readResult < 0)
        {
            logWrite("video", "av_read_frame failed (%d) after %u frames - reconnecting", readResult, framesDecoded);
            break;
        }

        if (packet->stream_index == streamIndex && avcodec_send_packet(codecCtx, packet) == 0)
        {
            while (avcodec_receive_frame(codecCtx, frame) == 0)
            {
                if (frame->format == AV_PIX_FMT_NVTEGRA)
                {
                    // Deliberately NOT pre-allocating swFrame with 256-byte alignment (tried live:
                    // it does make av_hwframe_transfer_data take a faster path - the "Frame
                    // address/pitch not aligned to 256, falling back to cpu transfer" log line
                    // disappeared entirely - but that faster path was confirmed live to bring real
                    // instability with it: video glitches, then total loss of HID input on a
                    // completely separate, dedicated core, which points at broader system
                    // instability rather than a video-only rendering glitch. This experimental
                    // hwaccel's internals aren't documented or debuggable from here (mailing-list
                    // patches, no source access to the actual transfer implementation) - reverted
                    // to the slower but proven-stable CPU-copy fallback rather than chase
                    // instability inside code neither of us can step through. The auto-allocated
                    // (unaligned) buffer below is what reliably takes that slow-but-stable path.
                    if (av_hwframe_transfer_data(swFrame, frame, 0) < 0)
                    {
                        logWrite("video", "av_hwframe_transfer_data failed - dropping frame");
                    }
                    else
                    {
                        renderFrame(swFrame, &framesDecoded);
                    }

                    av_frame_unref(swFrame);
                }
                else
                {
                    renderFrame(frame, &framesDecoded);
                }

                av_frame_unref(frame);
            }
        }

        av_packet_unref(packet);
    }

    av_frame_free(&swFrame);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codecCtx);
    avformat_close_input(&fmtCtx);
}

bool videoStart(void)
{
    if (s_renderer == NULL)
    {
        logWrite("video", "videoStart() called before videoInit() succeeded");
        return false;
    }

    while (appletMainLoop())
    {
        // Blocks (briefly, only before the very first session ever) until the host's real
        // BitrateKbps is known - see s_lastKnownBitrateKbps's doc comment. A false return only
        // happens if the app is exiting before any session ever connected, in which case the
        // loop condition below will also be false and this iteration's URL is never used.
        OrbitNxSessionConfig sessionConfig;
        if (networkWaitForSessionConfig(&sessionConfig) && sessionConfig.videoBitrateKbps > 0)
            s_lastKnownBitrateKbps = (int)sessionConfig.videoBitrateKbps;

        // Rebuilt every attempt (not hoisted above the loop) so a bitrate learned from the
        // host's SessionConfig or from the previous connection's own measured bit_rate (see
        // runStreamSession) actually takes effect on this reconnect - see
        // computeFifoSizePackets/s_lastKnownBitrateKbps above.
        int fifoPackets = computeFifoSizePackets(s_lastKnownBitrateKbps);
        char url[128];
        snprintf(url, sizeof(url), "udp://0.0.0.0:%d?overrun_nonfatal=1&fifo_size=%d", ORBITNX_VIDEO_PORT, fifoPackets);

        logWrite("video", "waiting for video stream on port %d (fifo_size=%d, ~%dms @ assumed %d kbps)...",
            ORBITNX_VIDEO_PORT, fifoPackets, VIDEO_BUFFER_TARGET_MS, s_lastKnownBitrateKbps);
        runStreamSession(url);
        s_videoReconnectCount++;
        svcSleepThread(VIDEO_RECONNECT_DELAY_NS);
    }

    return true;
}

void videoExit(void)
{
    if (s_texture != NULL) { SDL_DestroyTexture(s_texture); s_texture = NULL; }
    if (s_renderer != NULL) { SDL_DestroyRenderer(s_renderer); s_renderer = NULL; }
    if (s_window != NULL) { SDL_DestroyWindow(s_window); s_window = NULL; }
    if (s_hwDeviceCtx != NULL) { av_buffer_unref(&s_hwDeviceCtx); }
    avformat_network_deinit();
    SDL_Quit();
}
