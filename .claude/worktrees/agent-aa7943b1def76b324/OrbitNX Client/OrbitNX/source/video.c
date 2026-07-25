#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
#include <libavutil/timestamp.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_nvtegra.h>

#include <switch.h>

#include "video.h"
#include "network.h"
#include "renderer.h"
#include "logger.h"

VideoContext *createVideoContext()
{
    VideoContext *context = (VideoContext *)malloc(sizeof(VideoContext));
    context->fmt_ctx = NULL;
    context->video_dec_ctx = NULL;
    context->video_stream = NULL;
    context->video_stream_idx = -1;
    context->video_frame_count = 0;
    context->hw_device_ctx = NULL;
    context->use_hw_decode = false;

    // Frame
    context->frame = av_frame_alloc();
    if (context->frame == NULL)
    {
        fprintf(stderr, "Could not allocate frame\n");
        return NULL;
    }

    // Holds the system-memory copy of a hardware-decoded frame (see open_codec_context/
    // decode_packet) - allocated unconditionally since whether hardware decode actually ends up
    // available is only known once a session opens, not at context-creation time.
    context->sw_frame = av_frame_alloc();
    if (context->sw_frame == NULL)
    {
        fprintf(stderr, "Could not allocate sw_frame\n");
        return NULL;
    }

    return context;
}

void freeVideoContext(VideoContext *context)
{
    avcodec_free_context(&(context->video_dec_ctx));
    avformat_close_input(&(context->fmt_ctx));
    av_frame_free(&(context->frame));
    av_frame_free(&(context->sw_frame));
    if (context->hw_device_ctx != NULL)
        av_buffer_unref(&context->hw_device_ctx);
    free(context);
}

/* Decodes a single frame and returns 0 on success */
int decode_frame(AVCodecContext *avctx, AVFrame *frame, int *got_frame, AVPacket *pkt)
{
    int ret;
    *got_frame = 0;

    if (pkt)
    {
        ret = avcodec_send_packet(avctx, pkt);
        if (ret < 0)
            return ret == AVERROR_EOF ? 0 : ret;
    }

    ret = avcodec_receive_frame(avctx, frame);
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
        return ret;
    if (ret >= 0)
        *got_frame = 1;

    return 0;
}

/* Returns 1 if frame format is the same as the AVCodecContext format */
int expected_frame_format(AVCodecContext *avctx, AVFrame *frame)
{
    int width = avctx->width;
    int height = avctx->height;
    enum AVPixelFormat pix_fmt = avctx->pix_fmt;

    return frame->width == width && frame->height == height && frame->format == pix_fmt;
}

static int decode_packet(VideoContext *context, int *got_frame, AVPacket *pkt)
{
    if (pkt->stream_index == context->video_stream_idx &&
        decode_frame(context->video_dec_ctx, context->frame, got_frame, pkt) == 0)
    {
        // avcodec_receive_frame calls av_frame_unref(frame) at the start of EVERY call, success or
        // not, so on the common EAGAIN case (no new frame produced this round - normal, not an
        // error) context->frame comes back freshly unreffed (width=0/height=0/format=NONE).
        // Everything below only makes sense once a frame genuinely arrived.
        if (*got_frame)
        {
            if (!expected_frame_format(context->video_dec_ctx, context->frame))
            {
                fprintf(stderr, "Error: Width, height and pixel format have to be "
                                "constant in a rawvideo file, but the width, height or "
                                "pixel format of the input video changed:\n"
                                "old: width = %d, height = %d, format = %s\n"
                                "new: width = %d, height = %d, format = %s\n",
                        context->video_dec_ctx->width,
                        context->video_dec_ctx->height,
                        av_get_pix_fmt_name(context->video_dec_ctx->pix_fmt),
                        context->frame->width, context->frame->height,
                        av_get_pix_fmt_name(context->frame->format));
                // decode_packet returning -1 here only causes the caller to skip this one packet
                // and move on to the next (see handleVid's do/while over decode_packet) - the outer
                // av_read_frame loop keeps going, this does not end the session.
                orbitnx_log("video", "frame format mismatch on a genuinely decoded frame (old %dx%d %s, new %dx%d %s) - dropping this frame",
                           context->video_dec_ctx->width, context->video_dec_ctx->height,
                           av_get_pix_fmt_name(context->video_dec_ctx->pix_fmt),
                           context->frame->width, context->frame->height,
                           av_get_pix_fmt_name(context->frame->format));
                return -1;
            }

            // Hardware-decoded frames come back from avcodec_receive_frame still living in the
            // NVTEGRA/VIC engine's own memory (format==AV_PIX_FMT_NVTEGRA) - av_hwframe_transfer_data
            // is what actually copies them into normal, CPU-readable system memory (sw_frame), the
            // same way the software decoder's output already was.
            if (context->use_hw_decode && context->frame->format == AV_PIX_FMT_NVTEGRA)
            {
                // No av_frame_unref(context->sw_frame) before this call: av_hwframe_transfer_data
                // reuses sw_frame's existing buffer in place across calls, which is cheaper than
                // forcing a fresh buffer allocation every frame (60/sec, ~1.3MB each for a
                // 1280x720 NV12 frame) and does not leak.
                int transfer_ret = av_hwframe_transfer_data(context->sw_frame, context->frame, 0);
                if (transfer_ret < 0)
                {
                    char errbuf[100];
                    av_strerror(transfer_ret, errbuf, 100);
                    fprintf(stderr, "av_hwframe_transfer_data failed: %s\n", errbuf);
                    orbitnx_log("video", "av_hwframe_transfer_data failed: %s", errbuf);
                    return -1;
                }
            }

            handleFrame(context->renderContext, context, context->use_hw_decode ? context->sw_frame : context->frame);
        }
    }
    else
    {
        fprintf(stderr, "Error decoding video frame \n");
        // Rate-limited to once per 100ms: under real packet loss this can fail 20+ times within
        // milliseconds, and uncapped logging here (each call taking the logger's mutex) would
        // serialize this decode thread against every other thread's logging during exactly the
        // bursts it can least afford to stall for.
        static u64 s_last_logged_tick = 0;
        u64 now_tick = armGetSystemTick();
        if (armTicksToNs(now_tick - s_last_logged_tick) >= 100000000ULL) // 100ms
        {
            orbitnx_log("video", "decode_packet: decode_frame failed for stream_index=%d", pkt->stream_index);
            s_last_logged_tick = now_tick;
        }
        return -1;
    }

    return context->frame->pkt_size;
}

// Tells the decoder which of its offered pixel formats to actually decode into - required whenever
// a codec context has hw_device_ctx set, otherwise it has no way to know a hwaccel format should be
// preferred over a plain software one. Falls back to whatever the decoder's own first choice is if
// NVTEGRA isn't in the offered list for some reason (shouldn't happen given hw_device_ctx was just
// successfully created for exactly this purpose, but decode_packet's use_hw_decode check is what
// actually gates the hardware path either way, so this is just defensive).
static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts)
{
    for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++)
    {
        if (*p == AV_PIX_FMT_NVTEGRA) return *p;
    }
    orbitnx_log("video", "get_hw_format: NVTEGRA not offered by decoder, falling back to %s", av_get_pix_fmt_name(pix_fmts[0]));
    return pix_fmts[0];
}

static int open_codec_context(VideoContext *context, enum AVMediaType type)
{
    int ret, stream_index;
    AVStream *st;
    AVCodec *dec = NULL;

    ret = av_find_best_stream(context->fmt_ctx, type, -1, -1, NULL, 0);
    if (ret < 0)
    {
        fprintf(stderr, "Could not find %s stream in input file \n",
                av_get_media_type_string(type));
        return ret;
    }
    else
    {
        stream_index = ret;
        st = context->fmt_ctx->streams[stream_index];
        // find decoder for the stream

        dec = avcodec_find_decoder(st->codecpar->codec_id);
        if (dec == NULL)
        {
            fprintf(stderr, "Failed to find %s codec\n",
                    av_get_media_type_string(type));
            return AVERROR(EINVAL);
        }
        // Allocate a codec context for the decoder
        context->video_dec_ctx = avcodec_alloc_context3(dec);
        if (context->video_dec_ctx == NULL)
        {
            fprintf(stderr, "Failed to allocate the %s codec context\n",
                    av_get_media_type_string(type));
            return AVERROR(ENOMEM);
        }
        /*
        Copy codec parameters from input stream to output codec context
        */

        if ((ret = avcodec_parameters_to_context(context->video_dec_ctx, st->codecpar)) < 0)
        {
            fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n",
                    av_get_media_type_string(type));
            return ret;
        }

        // Software H.264 decode alone (no hwaccel), combined with Opus decode and the SDL render
        // loop, is CPU-heavy enough to drop fps below the stream's 60fps target. NVTEGRA hands
        // H.264 decode to the Tegra SoC's own dedicated video decode engine instead of the ARM
        // cores. Best-effort/optional: if this fails for any reason (older firmware, engine already
        // claimed elsewhere, driver issue), falls straight through to the software decode path -
        // hardware decode is a pure upgrade when available, never a hard requirement.
        int hw_ret = av_hwdevice_ctx_create(&context->hw_device_ctx, AV_HWDEVICE_TYPE_NVTEGRA, NULL, NULL, 0);
        if (hw_ret >= 0)
        {
            context->video_dec_ctx->hw_device_ctx = av_buffer_ref(context->hw_device_ctx);
            context->video_dec_ctx->get_format = get_hw_format;
            context->use_hw_decode = true;
            orbitnx_log("video", "hardware (NVTEGRA) video decode enabled");
        }
        else
        {
            char errbuf[100];
            av_strerror(hw_ret, errbuf, 100);
            orbitnx_log("video", "hardware decode unavailable, falling back to software: %s", errbuf);
        }

        //Init the decoders, without reference counting

        if ((ret = avcodec_open2(context->video_dec_ctx, dec, NULL)) < 0)
        {
            fprintf(stderr, "Failed to open %s codec\n",
                    av_get_media_type_string(type));
            return ret;
        }
        context->video_stream_idx = stream_index;
    }
    return 0;
}

void videoLoop(void *context_ptr)
{
    VideoContext* context = (VideoContext*) context_ptr;
    while(appletMainLoop())
        handleVid(context);
}

int handleVid(VideoContext *context)
{
    int ret = 0;
    int got_frame = 0;
    AVFormatContext *fmt_ctx = NULL;
    AVPacket pkt;

    // videoLoop calls this function again for every reconnect, reusing the same context - freeing
    // whatever the previous call left behind here, unconditionally, before opening anything new is
    // simpler and more robust than adding matching cleanup to every return path in this function.
    if (context->video_dec_ctx != NULL)
        avcodec_free_context(&context->video_dec_ctx);
    if (context->fmt_ctx != NULL)
        avformat_close_input(&context->fmt_ctx);
    // The NVTEGRA device context holds actual hardware decode engine handles (NVDEC/VIC channels),
    // a far more limited resource than regular heap memory - leaking one every reconnect would
    // eventually exhaust them and make every subsequent av_hwdevice_ctx_create fail, silently
    // falling back to software decode until the whole app restarts.
    if (context->hw_device_ctx != NULL)
        av_buffer_unref(&context->hw_device_ctx);
    context->use_hw_decode = false;
    context->video_stream = NULL;
    context->video_stream_idx = -1;

    // URL (network.h) is udp://, which just binds and receives once opened - no listen/accept step.
    AVDictionary *opts = 0;
    av_dict_set(&opts, "probesize", "50000", 0);

    //open input file, and allocate format context
    ret = avformat_open_input(&fmt_ctx, URL, 0, &opts);
    if (ret < 0)
    {
        char errbuf[100];
        av_strerror(ret, errbuf, 100);

        fprintf(stderr, "Input Error %s\n", errbuf);
        orbitnx_log("video", "avformat_open_input failed: %s", errbuf);
        return ret;
    }

    orbitnx_log("video", "session opened");
    setVideoActive(context->renderContext, true);

    context->fmt_ctx = fmt_ctx;

    // Retrieve stream information
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0)
    {
        fprintf(stderr, "Could not find stream information\n");
        return ret;
    }

    // Context for the video
    if (open_codec_context(context, AVMEDIA_TYPE_VIDEO) >= 0)
    {
        context->video_stream = fmt_ctx->streams[context->video_stream_idx];
    }

    if (context->video_stream == NULL)
    {
        fprintf(stderr, "Could not find stream in the input, aborting\n");
        ret = 1;
        return ret;
    }

    //initialize packet, set data to NULL, let the demuxer fill it

    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

    //read frames from the file
    int read_ret = 0;
    u64 frames_this_period = 0;
    u64 last_heartbeat_tick = armGetSystemTick();
    // A sustained decode degradation (fps well below 60 for several consecutive heartbeat windows)
    // can occur without recovering on its own for the rest of a connection. This watchdog bounds
    // the damage by forcing a clean reopen, the same way audio.c's equivalent watchdog does.
    int low_fps_heartbeats = 0;
    bool watchdog_triggered = false;
#define VIDEO_LOW_FPS_THRESHOLD 30.0
#define VIDEO_LOW_FPS_HEARTBEATS_BEFORE_RESTART 2
    while ((read_ret = av_read_frame(fmt_ctx, &pkt)) >= 0)
    {
        AVPacket orig_pkt = pkt;
        do
        {
            ret = decode_packet(context, &got_frame, &pkt);
            if (ret < 0)
                break;
            pkt.data += ret;
            pkt.size -= ret;
        }
        while (pkt.size > 0);
        av_packet_unref(&orig_pkt);
        if (got_frame) frames_this_period++;

        // Heartbeat, not per-frame - at 60fps that would be 60 log writes/sec, far too noisy to be
        // useful. If video ever hangs/freezes, this is what pins down the last moment it was still
        // actually decoding frames instead of leaving a silent gap with no data at all.
        u64 now_tick = armGetSystemTick();
        double elapsed_s = (double)armTicksToNs(now_tick - last_heartbeat_tick) / 1e9;
        if (elapsed_s >= 5.0)
        {
            double fps = frames_this_period / elapsed_s;
            orbitnx_log("video", "heartbeat: %llu frames decoded in last %.1fs (%.1f fps), hwDecode=%d",
                       (unsigned long long)frames_this_period, elapsed_s, fps, context->use_hw_decode);

            if (fps < VIDEO_LOW_FPS_THRESHOLD)
                low_fps_heartbeats++;
            else
                low_fps_heartbeats = 0;

            if (low_fps_heartbeats >= VIDEO_LOW_FPS_HEARTBEATS_BEFORE_RESTART)
            {
                orbitnx_log("video", "watchdog: fps below %.0f for %d consecutive heartbeats, forcing session restart",
                           VIDEO_LOW_FPS_THRESHOLD, low_fps_heartbeats);
                watchdog_triggered = true;
            }

            frames_this_period = 0;
            last_heartbeat_tick = now_tick;
        }

        if (watchdog_triggered)
            break;
    }

    if (watchdog_triggered)
    {
        // Already logged above - nothing more to add.
    }
    else
    {
        char errbuf[100];
        av_strerror(read_ret, errbuf, 100);
        orbitnx_log("video", "session ended: av_read_frame returned %d (%s) - host likely stopped/restarted its encoder", read_ret, errbuf);
    }

    //flush cached frames
    pkt.data = NULL;
    pkt.size = 0;
    do
    {
        decode_packet(context, &got_frame, &pkt);
    }
    while (got_frame);

    printf("Stream finished.\n");
    checkFrameAvail(context->renderContext);
    setVideoActive(context->renderContext, false);

    return ret;
}