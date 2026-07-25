#ifndef _LIBRARY_H
#define _LIBRARY_H

// DEPRECATED / not currently called: main.c no longer calls runLibraryPickerScreen() - shadPS4
// sessions now launch straight into shadPS4's own --big-picture mode on the PC side (see
// OrbitNX/ShadPs4/ShadPs4Launcher.cs in the host repo) instead of picking a specific title here
// first. Left intact (library.c too) rather than deleted in case this is worth reviving - if so,
// re-add the call in main.c's init() (see git history) and re-wire ShadPs4LibraryChannel/
// LibraryProtocol on the PC side.

#include "context.h"
#include "network.h"
#include "starsystem.h"

typedef struct
{
    char titleId[LIBRARY_TITLE_ID_BUF_LEN];
    char title[LIBRARY_TITLE_BUF_LEN];
    SDL_Texture *iconTexture; // NULL if this game's icon was missing/failed to decode
} LibraryGame;

/* Blocking, runs on the main thread during init() (like runAppSelectMenu) - opens a fresh library
 * listener, waits for the PC to connect and send its shadPS4 game list (cancellable via B,
 * animating the same starfield/top-bar/IP-line background as drawSplash), shows a scrollable
 * picker if any games arrived, sends the choice back to the PC, and tears the socket down
 * completely before returning.
 *
 * On every non-happy-path (no PC connects in time, zero games, malformed data, user cancels,
 * an internal deadline is hit) this leaves the app exactly as if it had never been called - the
 * caller proceeds straight into createVideoContext()/startInput() either way, which is what makes
 * it safe to call unconditionally whenever the user picked shadPS4.
 *
 * Returns true and fills outTitleId (NUL-terminated) only when the user actually picked a game;
 * false in every other case (outTitleId left untouched). */
bool runLibraryPickerScreen(RenderContext *context, StarSystem *starSystem,
                            char *outTitleId, size_t outTitleIdCap);

#endif
