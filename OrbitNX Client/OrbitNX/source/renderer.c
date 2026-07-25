#include "renderer.h"

#include <stdbool.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <unistd.h>
#include "video.h"
#include <time.h>
#include <SDL2/SDL2_gfxPrimitives.h>
float timeThen = 0;
float timeNow = 1;
float delta = 1;
uint32_t renderFPS = 0;
uint32_t getFPS()
{
    return renderFPS;
}
void initDelta()
{
    timeThen = svcGetSystemTick();
    timeNow = svcGetSystemTick();
    delta = (timeNow - timeThen) / 10000000;
}
void loopStart()
{
    timeNow = svcGetSystemTick();
    delta = (timeNow - timeThen) / 10000000;
    if (delta > 1)
    {
        // to much lag. set to 1
        delta = 1.0f;
    }
}

void loopEnd()
{
    timeThen = timeNow;
}

RenderContext *makeRenderer()
{
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER);
    IMG_Init(IMG_INIT_PNG);
    TTF_Init();
    chdir("romfs:/");
    plInitialize(PlServiceType_User);
    RenderContext *context = malloc(sizeof(RenderContext));
    context->window = SDL_CreateWindow("sdl2_gles2", 0, 0, RESX, RESY, SDL_WINDOW_FULLSCREEN);
    if (context->window == NULL)
    {
        SDL_Log("SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        while (1)
            ;
    }

    context->renderer = SDL_CreateRenderer(context->window, 0, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (context->renderer == NULL)
    {
        SDL_Log("SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_Quit();
        while (1)
            ;
    }
    SDL_SetRenderDrawBlendMode(context->renderer, SDL_BLENDMODE_BLEND); // enable transparency

    // Create font cache
    context->yuv_text = SDL_CreateTexture(context->renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, RESX, RESY);
    // Separate texture for hardware (NVTEGRA) decoded frames, which come out as NV12 (Y + one
    // interleaved U/V plane) rather than IYUV's 3 separate planes - see handleFrame/displayFrame.
    context->nv12_text = SDL_CreateTexture(context->renderer, SDL_PIXELFORMAT_NV12, SDL_TEXTUREACCESS_STREAMING, RESX, RESY);

    context->rect.x = 0;
    context->rect.y = 0;
    context->rect.w = RESX;
    context->rect.h = RESY;

    mutexInit(&context->texture_mut);
    mutexInit(&context->frame_avail_mut);
    mutexInit(&context->video_active_mut);
    context->frame_avail = false;
    context->video_active = false;
    context->frame_is_nv12 = false;

    PlFontData fontData, fontExtData;
    plGetSharedFontByType(&fontData, PlSharedFontType_Standard);
    plGetSharedFontByType(&fontExtData, PlSharedFontType_NintendoExt);
    SDL_Log((char *)fontData.address);
    context->font = FC_CreateFont();
    FC_LoadFont_RW(context->font, context->renderer, SDL_RWFromMem((void *)fontData.address, fontData.size), SDL_RWFromMem((void *)fontExtData.address, fontExtData.size), 1, 40, FC_MakeColor(0, 0, 0, 255), TTF_STYLE_NORMAL);
    initDelta();

    return context;
}

void setFrameAvail(RenderContext *context)
{
    mutexLock(&context->frame_avail_mut);
    context->frame_avail = true;
    mutexUnlock(&context->frame_avail_mut);
}

bool checkFrameAvail(RenderContext *context)
{
    bool ret;
    mutexLock(&context->frame_avail_mut);
    ret = context->frame_avail;
    context->frame_avail = false;
    mutexUnlock(&context->frame_avail_mut);
    return ret;
}

bool isVideoActive(RenderContext *context)
{
    bool ret;
    mutexLock(&context->video_active_mut);
    ret = context->video_active;
    mutexUnlock(&context->video_active_mut);
    return ret;
}

void setVideoActive(RenderContext *context, bool active)
{
    mutexLock(&context->video_active_mut);
    context->video_active = active;
    mutexUnlock(&context->video_active_mut);
}

void SDL_ClearScreen(RenderContext *context, SDL_Color colour)
{
    // SDL2_gfx's *RGBA primitives (roundedRectangleRGBA/roundedBoxRGBA) set the renderer's blend
    // mode themselves based on the alpha of whatever they last drew, forcing it to NONE for a
    // fully-opaque (alpha 255) call - and that mode leaks into every draw call afterwards,
    // including the next frame's, until something sets it back. Since every frame starts here,
    // this is the one place that reliably restores it before anything else this frame relies on
    // alpha blending (translucent bars, trails, ...).
    SDL_SetRenderDrawBlendMode(context->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(context->renderer, colour.r, colour.g, colour.b, colour.a);
    SDL_RenderClear(context->renderer);
}

void SDL_DrawText(RenderContext *context, int x, int y, SDL_Color colour, const char *text)
{
    FC_DrawColor(context->font, context->renderer, x, y, colour, text);
}
void draw_rect(RenderContext *context, int x, int y, int w, int h, SDL_Color colour)
{
    SDL_SetRenderDrawColor(context->renderer, colour.r, colour.g, colour.b, colour.a);
    SDL_Rect r = {x, y, w, h};
    SDL_RenderFillRect(context->renderer, &r);
}
void strokeCircle(RenderContext *context, float centreX, float centreY, float radius, SDL_Color colour)
{
    int diameter = (radius * 2);
    int x = (radius - 1);
    int y = 0;
    int tx = 1;
    int ty = 1;
    int error = (tx - diameter);
    SDL_SetRenderDrawColor(context->renderer, colour.r, colour.g, colour.b, colour.a);
    while (x >= y)
    {
        //  Each of the following renders an octant of the circle
        SDL_RenderDrawPoint(context->renderer, centreX + x, centreY - y);
        SDL_RenderDrawPoint(context->renderer, centreX + x, centreY + y);
        SDL_RenderDrawPoint(context->renderer, centreX - x, centreY - y);
        SDL_RenderDrawPoint(context->renderer, centreX - x, centreY + y);
        SDL_RenderDrawPoint(context->renderer, centreX + y, centreY - x);
        SDL_RenderDrawPoint(context->renderer, centreX + y, centreY + x);
        SDL_RenderDrawPoint(context->renderer, centreX - y, centreY - x);
        SDL_RenderDrawPoint(context->renderer, centreX - y, centreY + x);

        if (error <= 0)
        {
            ++y;
            error += ty;
            ty += 2;
        }

        if (error > 0)
        {
            --x;
            tx += 2;
            error += (tx - diameter);
        }
    }
}
void drawCircle(RenderContext *context, float centreX, float centreY, float radius, int lineThicknes, SDL_Color colour)
{
    for (int i = 0; i < lineThicknes; i++)
    {
        strokeCircle(context, centreX, centreY, radius - i, colour);
    }
}
void fillCircle(RenderContext *context, float centreX, float centreY, float radius, SDL_Color colour)
{
    for (double dy = 1; dy <= radius; dy += 1.0)
    {
        double dx = floor(sqrt((2.0 * radius * dy) - (dy * dy)));
        SDL_SetRenderDrawColor(context->renderer, colour.r, colour.g, colour.b, colour.a);
        SDL_RenderDrawLine(context->renderer, centreX - dx, centreY + dy - radius, centreX + dx, centreY + dy - radius);
        SDL_RenderDrawLine(context->renderer, centreX - dx, (centreY + 1) - dy + radius, centreX + dx, (centreY + 1) - dy + radius);
    }
}
void drawRoundedRect(RenderContext *context, int x, int y, int w, int h, int radius, SDL_Color colour)
{
    roundedRectangleRGBA(context->renderer, x, y, x + w, y + h, radius, colour.r, colour.g, colour.b, colour.a);
}

void fillRoundedRect(RenderContext *context, int x, int y, int w, int h, int radius, SDL_Color colour)
{
    roundedBoxRGBA(context->renderer, x, y, x + w, y + h, radius, colour.r, colour.g, colour.b, colour.a);
}

void drawGradient(RenderContext *context, int x, int y, int w, int h, SDL_Color colourStart, SDL_Color colourEnd, int direction)
{
    int drawLines = 1;
    switch (direction)
    {
    case 1: // Top to bottom gradient
        drawLines = h;
        break;
    case 2: // Bottom to top gradient
        drawLines = h;
        break;
    case 3: // Left to right gradient
        drawLines = w;
        break;
    case 4: // Right to left gradient
        drawLines = w;
        break;
    }
    for (int i = 0; i < drawLines; i++)
    { // Top to bottom gradient
        float t = ((float)(i)) / ((float)(drawLines));
        int r = ((float)colourStart.r) * (1.0f - t) + ((float)colourEnd.r) * t;
        int g = ((float)colourStart.g) * (1.0f - t) + ((float)colourEnd.g) * t;
        int b = ((float)colourStart.b) * (1.0f - t) + ((float)colourEnd.b) * t;
        int a = ((float)colourStart.a) * (1.0f - t) + ((float)colourEnd.a) * t;
        SDL_Color glc = {r, g, b, a};
        switch (direction)
        {
        case 1:
            draw_rect(context, x, y + i, w, 1, glc);
            break;
        case 2:
            draw_rect(context, x, (y + h) - i, w, 1, glc);
            break;
        case 3:
            draw_rect(context, x + 1, y, 0, h, glc);
            break;
        case 4:
            draw_rect(context, (x + w) - i, y, 0, h, glc);
            break;
        }
    }
}

void drawOrbitNxTopBar(RenderContext *context)
{
    SDL_Color barColour = {150, 150, 150, 26}; // ~0.1 alpha
    draw_rect(context, 0, 0, RESX, 70, barColour);

    SDL_Color white = {255, 255, 255, 255};
    SDL_Color red = {230, 57, 70, 255};
    SDL_Color blue = {58, 134, 255, 255};

    int x = 24;
    int y = 14;
    SDL_DrawText(context, x, y, white, "Orbit");
    x += FC_GetWidth(context->font, "%s", "Orbit");
    SDL_DrawText(context, x, y, red, "N");
    x += FC_GetWidth(context->font, "%s", "N");
    SDL_DrawText(context, x, y, blue, "X");
}

void drawIpAddressLine(RenderContext *context)
{
    u32 ip = gethostid();
    char ipBuf[64];
    snprintf(ipBuf, sizeof(ipBuf), "IP Address: %u.%u.%u.%u",
             ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);

    SDL_Color white = {255, 255, 255, 255};
    int textWidth = FC_GetWidth(context->font, "%s", ipBuf);
    SDL_DrawText(context, RESX / 2 - textWidth / 2, RESY - 46, white, ipBuf);
}

void drawSplash(RenderContext *context, StarSystem *starSystem)
{
    loopStart();

    SDL_Color bg = {5, 5, 8, 255};
    SDL_ClearScreen(context, bg);

    starsystem_update(starSystem, delta);
    starsystem_draw(context, starSystem);

    drawOrbitNxTopBar(context);
    drawIpAddressLine(context);

    SDL_RenderPresent(context->renderer);
    loopEnd();
}
// Copies a plane respecting the AVFrame's own linesize (stride), which for a hardware-decoded
// frame's planes is not guaranteed to equal the plain pixel width the way the software decoder's
// output already conveniently did (that's why the original code could get away with a single flat
// memcpy sized off the destination buffer alone) - a hardware decoder/VIC engine can pad each row
// out to whatever alignment its memory layout wants.
static void copyPlane(uint8_t *dst, int dst_stride, const uint8_t *src, int src_stride, int row_bytes, int height)
{
    for (int y = 0; y < height; y++)
    {
        memcpy(dst + (size_t)y * dst_stride, src + (size_t)y * src_stride, row_bytes);
    }
}

u64 old_time = 0, new_time = 0;
void handleFrame(RenderContext *renderContext, VideoContext *videoContext, AVFrame *frame)
{
    mutexLock(&renderContext->texture_mut);
    if (frame->format == AV_PIX_FMT_NV12)
    {
        copyPlane(renderContext->YPlane, RESX, frame->data[0], frame->linesize[0], RESX, RESY);
        copyPlane(renderContext->UVPlane, RESX, frame->data[1], frame->linesize[1], RESX, RESY / 2);
        renderContext->frame_is_nv12 = true;
    }
    else
    {
        // Unchanged from before this frame took an explicit AVFrame* param - still a flat memcpy
        // assuming linesize==width, which happens to hold for this software decode path at the
        // fixed 1280x720 this app targets. Not touched/generalized further to avoid any risk of
        // regressing the already-proven-working software path while adding the new hardware one.
        memcpy(renderContext->YPlane, frame->data[0], sizeof(renderContext->YPlane));
        memcpy(renderContext->UPlane, frame->data[1], sizeof(renderContext->UPlane));
        memcpy(renderContext->VPlane, frame->data[2], sizeof(renderContext->VPlane));
        renderContext->frame_is_nv12 = false;
    }
    mutexUnlock(&renderContext->texture_mut);
    setFrameAvail(renderContext);

    if (++videoContext->video_frame_count % 60 == 0)
    {
        new_time = svcGetSystemTick();
        renderFPS = (uint32_t)(60.0 / ((new_time - old_time) / 19200000.0));
        old_time = new_time;
    }
}

void displayFrame(RenderContext *renderContext)
{
    if (checkFrameAvail(renderContext))
    {
        SDL_RenderClear(renderContext->renderer);

        mutexLock(&renderContext->texture_mut);
        bool isNv12 = renderContext->frame_is_nv12;
        SDL_Texture *activeTexture;
        if (isNv12)
        {
            SDL_UpdateNVTexture(renderContext->nv12_text, &renderContext->rect,
                                 renderContext->YPlane, RESX,
                                 renderContext->UVPlane, RESX);
            activeTexture = renderContext->nv12_text;
        }
        else
        {
            SDL_UpdateYUVTexture(renderContext->yuv_text, &renderContext->rect, renderContext->YPlane, RESX,
                                 renderContext->UPlane, RESX / 2,
                                 renderContext->VPlane, RESX / 2);
            activeTexture = renderContext->yuv_text;
        }
        mutexUnlock(&renderContext->texture_mut);

        SDL_RenderCopy(renderContext->renderer, activeTexture, NULL, NULL);
        SDL_RenderPresent(renderContext->renderer);
    }
}

void freeRenderer(RenderContext *context)
{
    free(context);
    plExit();
    IMG_Quit();
    TTF_Quit();
    SDL_Quit();
}