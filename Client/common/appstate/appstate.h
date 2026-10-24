#pragma once

#include <stdbool.h>

// Single owner of appletMainLoop() for the whole app.
//
// appletMainLoop() pops the next applet message off one shared queue, and only the call that
// happens to pop AppletMessage_ExitRequest gets false back - libnx doesn't latch it. With the audio
// thread calling it too (previously, in its own receive loop), whichever thread won that race
// consumed the exit request: if audio got it, audio exited and the main/video loop kept running,
// so the app never closed until the system force-killed it. Same shape of bug in menu.c - an exit
// requested from the menu was consumed there and every later loop carried on as if nothing
// happened.

// Meant for the main thread. Wraps appletMainLoop() and latches its false result, so every later
// call (from any main-thread loop - menu, video, networkWaitForSessionConfig) keeps returning false
// too. Called from any other thread it degrades to appIsRunning() instead of touching the queue.
// Each call is an applet IPC round-trip - don't call it per packet; rate-limit hot callers.
bool appMainLoop(void);

// Any thread. True until appMainLoop() has seen the app being asked to exit. Never touches the
// applet message queue itself - use this, not appletMainLoop(), from worker threads.
bool appIsRunning(void);

// "Back to the app-selection menu" request - any thread. Raised by input.c when the player holds
// Minus long enough (see INPUT_MENU_HOLD_NS); main.c then stops the video loop, the HID thread
// closes the host connection (the host's SessionStopped closes the game and exits Big Picture on
// the PC), the menu runs again, and main.c clears the request once a new target is picked.
void appRequestMenu(void);
bool appMenuRequested(void);
void appClearMenuRequest(void);
