#include <switch.h>
#include <SDL2/SDL_image.h>
#include <unistd.h>

#include "menu.h"
#include "renderer.h"
#include "input.h"
#include "logger.h"

#define MENU_OPTION_COUNT 2

static const char *kOptionLabels[MENU_OPTION_COUNT] = {
    "Big Picture (Steam)",
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

// Same violet gradient + grey clear color as drawSplash (renderer.c) - the menu is the very first
// thing the user sees, so it should look like part of the same app instead of a bolted-on debug
// screen, without pulling in drawSplash's bubble particle system (unrelated decoration, no reason
// to couple this to it).
static void drawMenuFrame(RenderContext *context, int selectedIndex, SDL_Texture *icons[MENU_OPTION_COUNT])
{
    SDL_Color bg = {50, 50, 50, 255};
    SDL_ClearScreen(context, bg);

    SDL_Color bgf = {95, 0, 135, 255};
    SDL_Color bgt = {87, 1, 94, 255};
    drawGradient(context, 0, 0, 1280, 720, bgf, bgt, 1);

    SDL_Color white = {230, 230, 230, 255};
    SDL_Color dim = {210, 200, 215, 255};
    SDL_Color borderSelected = {255, 200, 60, 255};
    SDL_Color borderIdle = {255, 255, 255, 60};
    SDL_Color blockSelectedBg = {255, 255, 255, 235};
    SDL_Color blockIdleBg = {255, 255, 255, 25};

    const char *title = "Выберите режим трансляции";
    SDL_DrawText(context, 1280 / 2 - FC_GetWidth(context->font, "%s", title) / 2, 56, white, title);

    const int blockSize = 380;
    const int blockGap = 60;
    const int blockY = 170;
    const int totalWidth = blockSize * MENU_OPTION_COUNT + blockGap * (MENU_OPTION_COUNT - 1);
    const int firstBlockX = (1280 - totalWidth) / 2;
    const int borderThickness = 6;
    const int iconPadding = 40;

    for (int i = 0; i < MENU_OPTION_COUNT; i++)
    {
        int blockX = firstBlockX + i * (blockSize + blockGap);
        bool isSelected = (i == selectedIndex);

        // Option name above its block.
        int labelWidth = FC_GetWidth(context->font, "%s", kOptionLabels[i]);
        SDL_DrawText(context, blockX + blockSize / 2 - labelWidth / 2, blockY - 44,
                     isSelected ? white : dim, kOptionLabels[i]);

        // Border drawn as a slightly larger rect behind the block itself.
        draw_rect(context, blockX - borderThickness, blockY - borderThickness,
                  blockSize + borderThickness * 2, blockSize + borderThickness * 2,
                  isSelected ? borderSelected : borderIdle);
        draw_rect(context, blockX, blockY, blockSize, blockSize,
                  isSelected ? blockSelectedBg : blockIdleBg);

        if (icons[i] != NULL)
        {
            SDL_Rect iconDest = {
                blockX + iconPadding, blockY + iconPadding,
                blockSize - iconPadding * 2, blockSize - iconPadding * 2};
            SDL_RenderCopy(context->renderer, icons[i], NULL, &iconDest);
        }
    }

    u32 ip = gethostid();
    char ip_buf[64];
    snprintf(ip_buf, sizeof(ip_buf), "IP-Address: %u.%u.%u.%u",
             ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
    SDL_DrawText(context, 1280 / 2 - FC_GetWidth(context->font, "%s", ip_buf) / 2, 660, white, ip_buf);

    SDL_RenderPresent(context->renderer);
}

AppTarget runAppSelectMenu(RenderContext *context)
{
    // padConfigureInput must run before any padInitialize/padUpdate call - normally done once by
    // input.c's input_init(), but that now runs *after* this menu (see main.c) so the menu needs
    // its own call here, or every button read on real hardware silently comes back empty.
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);

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

        drawMenuFrame(context, selectedIndex, icons);
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
