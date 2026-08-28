#pragma once

#include "context.h"
#include "starsystem.h"
#include "../network/network.h"

// Blocking: draws the app-selection screen (Big Picture vs shadPS4) and polls its own pad input
// until the user confirms with A. Must run after makeRenderer() (needs the render context/font)
// and before the HID send loop ever starts (so the choice is set before the very first
// OrbitNxHidPacket is sent - see main.c and input.c's inputSetAppTarget()). starSystem must
// already be initialised (starsystem_init) by the caller - it's the same instance drawSplash
// keeps animating afterwards, so the cosmos background doesn't reset/jump when the menu hands off
// to the post-selection waiting screen.
OrbitNxAppTarget runAppSelectMenu(MenuContext *context, StarSystem *starSystem);
