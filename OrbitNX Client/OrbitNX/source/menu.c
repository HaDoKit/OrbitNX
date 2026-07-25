#include <switch.h>
#include <SDL2/SDL_image.h>
#include <unistd.h>

#include "menu.h"
#include "renderer.h"
#include "input.h"
#include "logger.h"
#include "starsystem.h"

#define MENU_OPTION_COUNT 2

static const char *kOptionLabels[MENU_OPTION_COUNT] = {
    "Steam",
    "shadPS4",
};

static const char *kIconPaths[MENU_OPTION_COUNT] = {
    "romfs:/icons/steam.png",
    "romfs:/icons/shadps4.png",
};

static SDL_Texture *loadIcon(RenderContext *context, const char *path)
{
    SDL_Surface *surface = IMG_Load(path);
    if (surface == NULL)
    {
        orbitnx_log("menu", "loadIcon: failed to load %s: %s", path, IMG_GetError());
        return NULL;
    }

    SDL_Texture *texture = SDL_CreateTextureFromSurface(context->renderer, surface);
    if (texture == NULL)
    {
        orbitnx_log("menu", "loadIcon: failed to create texture from %s: %s", path, SDL_GetError());
    }
    SDL_FreeSurface(surface);
    return texture;
}

// Same overall card language as the Unity prototype: portrait rounded rect, thin border that
// turns gold when selected, dark translucent fill, icon in the upper area, label under it.
static void drawCard(RenderContext *context, int blockX, int blockY, int blockW, int blockH,
                      bool isSelected, SDL_Texture *icon, const char *label)
{
    const int cornerRadius = 20;
    const int borderThickness = 3;

    SDL_Color borderColour = isSelected ? (SDL_Color){255, 200, 60, 255} : (SDL_Color){255, 255, 255, 200};
    SDL_Color fillColour = {0, 0, 0, 130};

    fillRoundedRect(context, blockX, blockY, blockW, blockH, cornerRadius, fillColour);
    for (int i = 0; i < borderThickness; i++)
    {
        drawRoundedRect(context, blockX + i, blockY + i, blockW - i * 2, blockH - i * 2, cornerRadius, borderColour);
    }

    const int labelHeight = 70;
    const int iconPadding = 10;
    if (icon != NULL)
    {
        int iconAreaW = blockW - iconPadding * 2;
        int iconAreaH = blockH - labelHeight - iconPadding * 2;
        int iconSize = iconAreaW < iconAreaH ? iconAreaW : iconAreaH;
        SDL_Rect iconDest = {
            blockX + (blockW - iconSize) / 2,
            blockY + iconPadding + (iconAreaH - iconSize) / 2,
            iconSize, iconSize};
        SDL_RenderCopy(context->renderer, icon, NULL, &iconDest);
    }

    SDL_Color labelColour = isSelected ? (SDL_Color){255, 255, 255, 255} : (SDL_Color){210, 210, 210, 255};
    int labelWidth = FC_GetWidth(context->font, "%s", label);
    SDL_DrawText(context, blockX + blockW / 2 - labelWidth / 2, blockY + blockH - labelHeight + 14, labelColour, label);
}

static void drawMenuFrame(RenderContext *context, StarSystem *starSystem, int selectedIndex, SDL_Texture *icons[MENU_OPTION_COUNT])
{
    SDL_Color bg = {5, 5, 8, 255};
    SDL_ClearScreen(context, bg);
    starsystem_draw(context, starSystem);

    drawOrbitNxTopBar(context);

    const int blockW = 340;
    const int blockH = 440;
    const int blockGap = 60;
    const int blockY = (RESY - blockH) / 2;
    const int totalWidth = blockW * MENU_OPTION_COUNT + blockGap * (MENU_OPTION_COUNT - 1);
    const int firstBlockX = (RESX - totalWidth) / 2;

    for (int i = 0; i < MENU_OPTION_COUNT; i++)
    {
        int blockX = firstBlockX + i * (blockW + blockGap);
        drawCard(context, blockX, blockY, blockW, blockH, i == selectedIndex, icons[i], kOptionLabels[i]);
    }

    drawIpAddressLine(context);

    SDL_RenderPresent(context->renderer);
}

AppTarget runAppSelectMenu(RenderContext *context, StarSystem *starSystem)
{
    // padConfigureInput must run before any padInitialize/padUpdate call - normally done once by
    // input.c's input_init(), but that now runs *after* this menu (see main.c) so the menu needs
    // its own call here, or every button read on real hardware silently comes back empty. Only done
    // once (main.c can re-enter this menu later, from the splash screen's "B to go back") - a second
    // call here would reconfigure npad for 1 player, undoing input_init()'s 4-player setup that runs
    // between the first and any later call.
    static bool s_padConfigured = false;
    if (!s_padConfigured)
    {
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        s_padConfigured = true;
    }

    // A dedicated, self-contained PadState rather than reaching into input.c's multi-controller
    // globals (pad/pad2..4, set up by input_init()) - the menu only ever needs a single confirming
    // controller, and staying self-contained here means input_init()/startInput() don't need any
    // reordering to support this screen.
    PadState menuPad;
    padInitializeDefault(&menuPad);

    SDL_Texture *icons[MENU_OPTION_COUNT];
    for (int i = 0; i < MENU_OPTION_COUNT; i++)
    {
        icons[i] = loadIcon(context, kIconPaths[i]);
    }

    int selectedIndex = 0;
    const float dt = 1.0f / 70.0f; // matches this loop's own ~70fps sleep cadence below

    while (appletMainLoop())
    {
        padUpdate(&menuPad);
        u64 down = padGetButtonsDown(&menuPad);

        if (down & (HidNpadButton_Up | HidNpadButton_Left))
        {
            selectedIndex = (selectedIndex - 1 + MENU_OPTION_COUNT) % MENU_OPTION_COUNT;
        }
        else if (down & (HidNpadButton_Down | HidNpadButton_Right))
        {
            selectedIndex = (selectedIndex + 1) % MENU_OPTION_COUNT;
        }
        else if (down & HidNpadButton_A)
        {
            break;
        }

        starsystem_update(starSystem, dt);
        drawMenuFrame(context, starSystem, selectedIndex, icons);
        svcSleepThread(14285714ULL); // ~70fps, same cadence as main()'s own loop
    }

    for (int i = 0; i < MENU_OPTION_COUNT; i++)
    {
        if (icons[i] != NULL) SDL_DestroyTexture(icons[i]);
    }

    AppTarget chosen = (AppTarget)selectedIndex;
    orbitnx_log("menu", "app target selected: %d (%s)", (int)chosen, kOptionLabels[selectedIndex]);
    g_selectedAppTarget = chosen;
    return chosen;
}
