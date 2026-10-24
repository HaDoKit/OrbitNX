#include "appstate.h"

#include <stdatomic.h>

#include <switch.h>

static atomic_bool s_exiting = false;

bool appMainLoop(void)
{
    if (atomic_load(&s_exiting))
        return false;

    // Defensive: a non-main-thread caller (e.g. libavformat invoking video.c's interrupt
    // callback from one of its own worker threads) only reads the latched flag, never pops the
    // applet queue - see appstate.h for why popping it from two threads loses the exit request.
    if (threadGetCurHandle() != envGetMainThreadHandle())
        return true;

    if (!appletMainLoop())
    {
        atomic_store(&s_exiting, true);
        return false;
    }

    return true;
}

bool appIsRunning(void)
{
    return !atomic_load(&s_exiting);
}

static atomic_bool s_menuRequested = false;

void appRequestMenu(void)
{
    atomic_store(&s_menuRequested, true);
}

bool appMenuRequested(void)
{
    return atomic_load(&s_menuRequested);
}

void appClearMenuRequest(void)
{
    atomic_store(&s_menuRequested, false);
}
