#ifndef _CONTEXT_H
#define _CONTEXT_H

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <switch.h>
#include <SDL.h>
#include <SDL2/SDL_image.h>
#include "SDL_FontCache.h"

#define RESX 1280
#define RESY 720

typedef struct
{
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *yuv_text;
    // Separate texture for the hardware-decode (NVTEGRA/VIC) path - its output is NV12 (Y plane +
    // one interleaved U/V plane), a genuinely different SDL texture format/update call from the
    // 3-plane YUV420P the software decoder produces, not just a different memory layout of the
    // same thing. See handleFrame/displayFrame in renderer.c.
    SDL_Texture *nv12_text;

    SDL_Rect rect;

    Mutex texture_mut;

    u8 YPlane[RESX * RESY];
    u8 UPlane[RESX * RESY / 4];
    u8 VPlane[RESX * RESY / 4];
    // NV12's chroma plane: U and V interleaved instead of separate - half the resolution in each
    // dimension like U/VPlane above, but 2 bytes/sample-pair instead of 1, so this is the same
    // total byte count as UPlane+VPlane combined.
    u8 UVPlane[RESX * RESY / 2];

    // Which of the above the most recently copied-in frame actually used - set alongside the plane
    // data under texture_mut, read by displayFrame to pick yuv_text+3-plane update vs.
    // nv12_text+2-plane update.
    bool frame_is_nv12;

    bool frame_avail;
    Mutex frame_avail_mut;
    FC_Font *font;

    bool video_active;
    Mutex video_active_mut;

} RenderContext;

typedef struct
{
    AVFormatContext *fmt_ctx;
    AVCodecContext *video_dec_ctx;
    AVStream *video_stream;
    int video_stream_idx;
    AVFrame *frame;
    // Holds the system-memory copy of a hardware-decoded frame after av_hwframe_transfer_data -
    // only populated/used when use_hw_decode is true. Kept as a separate persistent AVFrame
    // (allocated once, reused every call) rather than allocated per-frame, same reasoning as
    // `frame` itself.
    AVFrame *sw_frame;
    // Non-NULL once a NVTEGRA (Tegra hardware video decode) device was successfully created for
    // this session - see open_codec_context. Deliberately optional/best-effort: if hardware decode
    // setup fails for any reason (older firmware, engine already claimed by another process,
    // driver hiccup, ...), this stays NULL and decoding falls back to the plain software path
    // exactly as before - hardware decode is a pure upgrade when available, never a hard
    // requirement to get video at all.
    AVBufferRef *hw_device_ctx;
    bool use_hw_decode;
    int video_frame_count;
    RenderContext *renderContext;
} VideoContext;

typedef struct
{
    int lissock;
    int sock;
} JoyConSocket;

#endif