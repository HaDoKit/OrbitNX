#include "menu.h"

#include <switch.h>
#include <SDL2/SDL_image.h>
#include <unistd.h>

#include "renderer.h"
#include "../input/input.h"
#include "../common/logger/logger.h"

#define MENU_OPTION_COUNT 2

static const char *kOptionLabels[MENU_OPTION_COUNT] = {
    "Steam",
    "shadPS4",
};

static const char *kIconPaths[MENU_OPTION_COUNT] = {
    "romfs:/icons/steam.png",
    "romfs:/icons/shadps4.png",
};

static SDL_Texture *loadIcon(MenuContext *context, const char *path)
{
    SDL_Surface *surface = IMG_Load(path);
    if (surface == NULL)
    {
        logWrite("menu", "loadIcon: failed to load %s: %s", path, IMG_GetError());
        return NULL;
    }

    SDL_Texture *texture = SDL_CreateTextureFromSurface(context->renderer, surface);
    if (texture == NULL)
    {
        logWrite("menu", "loadIcon: failed to create texture from %s: %s", path, SDL_GetError());
    }
    SDL_FreeSurface(surface);
    return texture;
}

// Portrait rounded rect, thin border that turns gold when selected, dark translucent fill, icon
// in the upper area, label under it.
static void drawCard(MenuContext *context, int blockX, int blockY, int blockW, int blockH,
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

static void drawMenuFrame(MenuContext *context, StarSystem *starSystem, int selectedIndex, SDL_Texture *icons[MENU_OPTION_COUNT])
{
    SDL_Color bg = {5, 5, 8, 255};
    SDL_ClearScreen(context, bg);
    starsystem_draw(context, starSystem);

    drawOrbitNxTopBar(context);

    const int blockW = 340;
    const int blockH = 440;
    const int blockGap = 60;
    const int blockY = (MENU_RESY - blockH) / 2;
    const int totalWidth = blockW * MENU_OPTION_COUNT + blockGap * (MENU_OPTION_COUNT - 1);
    const int firstBlockX = (MENU_RESX - totalWidth) / 2;

    for (int i = 0; i < MENU_OPTION_COUNT; i++)
    {
        int blockX = firstBlockX + i * (blockW + blockGap);
        drawCard(context, blockX, blockY, blockW, blockH, i == selectedIndex, icons[i], kOptionLabels[i]);
    }

    drawIpAddressLine(context);

    SDL_RenderPresent(context->renderer);
}

OrbitNxAppTarget runAppSelectMenu(MenuContext *context, StarSystem *starSystem)
{
    // padConfigureInput must run before any padInitialize/padUpdate call. This menu runs before
    // input.c's inputInit() (see main.c) - input.c does its own padConfigureInput for 4 players
    // once this menu is done, so this call is only for this screen's own single-controller read
    // in the meantime.
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);

    PadState menuPad;
    padInitializeDefault(&menuPad);

    SDL_Texture *icons[MENU_OPTION_COUNT];
    for (int i = 0; i < MENU_OPTION_COUNT; i++)
    {
        icons[i] = loadIcon(context, kIconPaths[i]);
    }

    int selectedIndex = 0;

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

        float dt = rendererTick();
        starsystem_update(starSystem, dt);
        drawMenuFrame(context, starSystem, selectedIndex, icons);
        svcSleepThread(14285714ULL); // ~70fps
    }

    for (int i = 0; i < MENU_OPTION_COUNT; i++)
    {
        if (icons[i] != NULL) SDL_DestroyTexture(icons[i]);
    }

    OrbitNxAppTarget chosen = (OrbitNxAppTarget)selectedIndex;
    logWrite("menu", "app target selected: %d (%s)", (int)chosen, kOptionLabels[selectedIndex]);
    inputSetAppTarget(chosen);
    return chosen;
}
