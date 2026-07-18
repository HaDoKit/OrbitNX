#ifndef _MENU_H
#define _MENU_H

#include "context.h"
#include "network.h"

/* Blocking: draws the app-selection screen (Big Picture vs shadPS4) and polls its own pad input
 * until the user confirms with A. Must run after makeRenderer() (needs the render context/font)
 * and before startInput() (so the choice is set before the very first JoyPkg is ever sent) - see
 * main.c's init(). Also sets input.c's g_selectedAppTarget as a side effect before returning. */
AppTarget runAppSelectMenu(RenderContext *context);

#endif
