#ifndef _MENU_H
#define _MENU_H

#include "context.h"
#include "network.h"
#include "starsystem.h"

/* Blocking: draws the app-selection screen (Big Picture vs shadPS4) and polls its own pad input
 * until the user confirms with A. Must run after makeRenderer() (needs the render context/font)
 * and before startInput() (so the choice is set before the very first JoyPkg is ever sent) - see
 * main.c's init(). Also sets input.c's g_selectedAppTarget as a side effect before returning.
 * starSystem must already be initialised (starsystem_init) by the caller - it's the same instance
 * drawSplash keeps animating afterwards, so the cosmos background doesn't reset/jump when the menu
 * hands off to the post-selection waiting screen. */
AppTarget runAppSelectMenu(RenderContext *context, StarSystem *starSystem);

#endif
