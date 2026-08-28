#pragma once

#include <stdbool.h>

#include "../network/network.h"

// Configures pad/touch/sixaxis input for up to 4 controllers. Call once at startup.
bool inputInit(void);

// Sets the AppTarget every subsequent inputRead() will stamp onto the outgoing packet (see
// network.h's OrbitNxAppTarget) - chosen once via menu.c's runAppSelectMenu() before the HID send
// loop starts. Defaults to OrbitNxAppTarget_BigPicture if never called, so a build that somehow
// skips the menu still behaves like it did before this existed.
void inputSetAppTarget(OrbitNxAppTarget target);

// Fills *pkt with the current live input state (buttons, sticks, touch, motion) for the HID send
// loop to transmit - call once per outgoing packet (see main.c's startNetwork()). Safe to call
// repeatedly; each call re-scans all input sources.
void inputRead(OrbitNxHidPacket *pkt);

// Stops the sixaxis sensors started by inputInit(). Call once at shutdown.
void inputExit(void);
