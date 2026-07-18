#ifndef INPUT_H
#define INPUT_H

#include "network.h"

/* Which app to launch/focus on the PC host, chosen once by menu.c's runAppSelectMenu() before
 * startInput() is called and sent unchanged on every JoyPkg for the rest of the process's life -
 * see gamePadSend() in input.c. Defaults to Big Picture so a build that somehow skips the menu
 * still behaves like the app did before this field existed. */
extern AppTarget g_selectedAppTarget;

/* Loop to handle joycon inputs and send theme to the server */
void inputHandlerLoop(void *dummy);
void input_init();
void input_unInit();
#endif