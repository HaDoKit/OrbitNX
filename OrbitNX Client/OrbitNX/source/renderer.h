#ifndef _RENDERER_H
#define _RENDERER_H

#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#include "context.h"
/* Allocates a render context */
RenderContext *makeRenderer(void);

/* Draws an image filling all screen */
void drawSplash(RenderContext *context);

/* Handles a decoded frame - explicit AVFrame* param since it may be either videoContext->frame
 * (software decode) or videoContext->sw_frame (hardware decode, already transferred to system
 * memory - see video.c's decode_packet) depending on which path decoded it. */
void handleFrame(RenderContext *context, VideoContext *videoContext, AVFrame *frame);

/* Draws a frame */
void displayFrame(RenderContext *renderContext);

uint32_t getFPS(void);

/* Deallocates the render context */
void freeRenderer(RenderContext *context);

/* Sets the variable that indicates that there's a frame ready to be drawn */
void setFrameAvail(RenderContext *context);

/* Checks if a frame is ready to be drawn and sets that variable to false */
bool checkFrameAvail(RenderContext *context);

/* Returns true if there is a video playing right now */
bool isVideoActive(RenderContext *context);

/* Sets the video-playing status */
void setVideoActive(RenderContext *context, bool active);

/* Drawing primitives also used by menu.c's app-selection screen, so it matches drawSplash's look
 * instead of inventing its own. */
void SDL_ClearScreen(RenderContext *context, SDL_Color colour);
void SDL_DrawText(RenderContext *context, int x, int y, SDL_Color colour, const char *text);
void draw_rect(RenderContext *context, int x, int y, int w, int h, SDL_Color colour);
void drawGradient(RenderContext *context, int x, int y, int w, int h, SDL_Color colourStart, SDL_Color colourEnd, int direction);

#endif