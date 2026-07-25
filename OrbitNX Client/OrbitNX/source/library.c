#include <switch.h>
#include <SDL2/SDL_image.h>
#include <string.h>
#include <stdlib.h>

#include "library.h"
#include "renderer.h"
#include "logger.h"

#define LIBRARY_WAIT_FOR_PC_DEADLINE_SECONDS 20.0
#define LIBRARY_ACCEPT_POLL_TIMEOUT_MS 200
#define LIBRARY_RECEIVE_LIST_DEADLINE_SECONDS 30.0
#define LIBRARY_RECV_TIMEOUT_MS 5000
// Must stay shorter than the PC's own read timeout on the selection response (120s, see
// OrbitNX/ShadPs4/ShadPs4LibraryChannel.cs) so the well-behaved case always has the Switch send
// *some* answer before the PC gives up waiting.
#define LIBRARY_PICKER_INACTIVITY_DEADLINE_SECONDS 90.0

#define LIBRARY_PAGE_COLS 4
#define LIBRARY_PAGE_ROWS 2

static double ticksSinceToSeconds(u64 startTick)
{
    return (double)armTicksToNs(armGetSystemTick() - startTick) / 1e9;
}

static bool waitForPcConnection(RenderContext *context, StarSystem *starSystem, LibrarySocket *sock, PadState *pad)
{
    u64 startTick = armGetSystemTick();
    const float dt = 1.0f / 70.0f;

    while (appletMainLoop())
    {
        padUpdate(pad);
        if (padGetButtonsDown(pad) & HidNpadButton_B)
        {
            orbitnx_log("library", "wait-for-PC cancelled by user (B)");
            return false;
        }

        int result = acceptLibraryConnection(sock, LIBRARY_ACCEPT_POLL_TIMEOUT_MS);
        if (result == 1) return true;
        if (result < 0)
        {
            orbitnx_log("library", "acceptLibraryConnection hard error, giving up on library screen");
            return false;
        }

        if (ticksSinceToSeconds(startTick) >= LIBRARY_WAIT_FOR_PC_DEADLINE_SECONDS)
        {
            orbitnx_log("library", "wait-for-PC timed out after %.1fs, no PC connected", LIBRARY_WAIT_FOR_PC_DEADLINE_SECONDS);
            return false;
        }

        starsystem_update(starSystem, dt);

        SDL_Color bg = {5, 5, 8, 255};
        SDL_ClearScreen(context, bg);
        starsystem_draw(context, starSystem);
        drawOrbitNxTopBar(context);
        drawIpAddressLine(context);

        SDL_Color white = {255, 255, 255, 255};
        const char *status = "Waiting for PC (shadPS4 library)... (B to cancel)";
        int textWidth = FC_GetWidth(context->font, "%s", status);
        SDL_DrawText(context, RESX / 2 - textWidth / 2, RESY / 2 - 20, white, status);

        SDL_RenderPresent(context->renderer);
    }
    return false;
}

// Reads the 12-byte list header + gameCount per-game records. On ANY failure (short read/timeout,
// magic/version mismatch, oversized field) the whole exchange is treated as failed - a desynced
// TCP stream can't be safely partially recovered, so this discards any textures already built and
// returns false rather than presenting a partial list.
static bool receiveGameList(RenderContext *context, LibrarySocket *sock, LibraryGame *games, int *outCount)
{
    *outCount = 0;
    u64 startTick = armGetSystemTick();
    int builtSoFar = 0;

    uint8_t header[12];
    if (recvLibraryExact(sock, header, sizeof(header), LIBRARY_RECV_TIMEOUT_MS) != 1)
    {
        orbitnx_log("library", "failed to receive list header");
        return false;
    }

    if (header[0] != LIBRARY_MAGIC0 || header[1] != LIBRARY_MAGIC1 ||
        header[2] != LIBRARY_MAGIC2 || header[3] != LIBRARY_MAGIC3)
    {
        orbitnx_log("library", "list header magic mismatch - protocol desync or unexpected sender");
        return false;
    }

    uint8_t version = header[4];
    uint8_t msgType = header[5];
    // header[6..7] reserved, deliberately ignored (must be sent zero, readers must not reject on it)
    uint32_t gameCount;
    memcpy(&gameCount, &header[8], sizeof(uint32_t));

    if (version != LIBRARY_PROTOCOL_VERSION || msgType != LIBRARY_MSG_LIST)
    {
        orbitnx_log("library", "unexpected list header version=%u msgType=%u", version, msgType);
        return false;
    }
    if (gameCount > LIBRARY_MAX_GAMES)
    {
        orbitnx_log("library", "gameCount %u exceeds LIBRARY_MAX_GAMES %d - treating as desync", gameCount, LIBRARY_MAX_GAMES);
        return false;
    }

    for (uint32_t i = 0; i < gameCount; i++)
    {
        if (ticksSinceToSeconds(startTick) >= LIBRARY_RECEIVE_LIST_DEADLINE_SECONDS)
        {
            orbitnx_log("library", "receive-list overall deadline exceeded at game %u/%u", i, gameCount);
            goto fail;
        }

        uint8_t subHeader[8];
        if (recvLibraryExact(sock, subHeader, sizeof(subHeader), LIBRARY_RECV_TIMEOUT_MS) != 1)
        {
            orbitnx_log("library", "failed to receive per-game sub-header at index %u", i);
            goto fail;
        }

        uint16_t titleIdLen, titleLen;
        uint32_t iconLen;
        memcpy(&titleIdLen, &subHeader[0], sizeof(uint16_t));
        memcpy(&titleLen, &subHeader[2], sizeof(uint16_t));
        memcpy(&iconLen, &subHeader[4], sizeof(uint32_t));

        if (titleIdLen > LIBRARY_TITLE_ID_WIRE_MAX || titleLen > LIBRARY_TITLE_WIRE_MAX || iconLen > LIBRARY_ICON_MAX_BYTES)
        {
            orbitnx_log("library", "per-game record at index %u has an oversized field - aborting exchange", i);
            goto fail;
        }

        char titleIdBuf[LIBRARY_TITLE_ID_WIRE_MAX];
        if (titleIdLen > 0 && recvLibraryExact(sock, titleIdBuf, titleIdLen, LIBRARY_RECV_TIMEOUT_MS) != 1)
        {
            orbitnx_log("library", "failed to receive titleId bytes at index %u", i);
            goto fail;
        }

        char titleBuf[LIBRARY_TITLE_WIRE_MAX];
        if (titleLen > 0 && recvLibraryExact(sock, titleBuf, titleLen, LIBRARY_RECV_TIMEOUT_MS) != 1)
        {
            orbitnx_log("library", "failed to receive title bytes at index %u", i);
            goto fail;
        }

        SDL_Texture *iconTexture = NULL;
        if (iconLen > 0)
        {
            uint8_t *iconBuf = (uint8_t *)malloc(iconLen);
            if (iconBuf == NULL)
            {
                orbitnx_log("library", "malloc(%u) failed for icon at index %u - aborting exchange", iconLen, i);
                goto fail;
            }
            if (recvLibraryExact(sock, iconBuf, iconLen, LIBRARY_RECV_TIMEOUT_MS) != 1)
            {
                orbitnx_log("library", "failed to receive icon bytes at index %u", i);
                free(iconBuf);
                goto fail;
            }

            // Decode-and-free-immediately rather than holding raw PNG bytes for the whole library
            // at once - this codebase has no precedent for bulk medium-sized buffers (the biggest
            // existing allocation is the ~1.84MB video pixel planes, made once at startup).
            SDL_Surface *surface = IMG_Load_RW(SDL_RWFromMem(iconBuf, (int)iconLen), 1);
            free(iconBuf);
            if (surface != NULL)
            {
                iconTexture = SDL_CreateTextureFromSurface(context->renderer, surface);
                SDL_FreeSurface(surface);
                if (iconTexture == NULL)
                {
                    orbitnx_log("library", "SDL_CreateTextureFromSurface failed for icon at index %u: %s", i, SDL_GetError());
                }
            }
            else
            {
                orbitnx_log("library", "IMG_Load_RW failed for icon at index %u: %s", i, IMG_GetError());
            }
        }

        LibraryGame *game = &games[builtSoFar];
        size_t copyIdLen = titleIdLen < LIBRARY_TITLE_ID_BUF_LEN - 1 ? titleIdLen : LIBRARY_TITLE_ID_BUF_LEN - 1;
        memcpy(game->titleId, titleIdBuf, copyIdLen);
        game->titleId[copyIdLen] = '\0';
        size_t copyTitleLen = titleLen < LIBRARY_TITLE_BUF_LEN - 1 ? titleLen : LIBRARY_TITLE_BUF_LEN - 1;
        memcpy(game->title, titleBuf, copyTitleLen);
        game->title[copyTitleLen] = '\0';
        game->iconTexture = iconTexture;
        builtSoFar++;
    }

    *outCount = builtSoFar;
    return true;

fail:
    for (int j = 0; j < builtSoFar; j++)
    {
        if (games[j].iconTexture != NULL) SDL_DestroyTexture(games[j].iconTexture);
    }
    *outCount = 0;
    return false;
}

static bool sendSelectionResponse(LibrarySocket *sock, bool selected, const char *titleId)
{
    uint8_t msg[LIBRARY_SELECTION_MSG_SIZE];
    memset(msg, 0, sizeof(msg));
    msg[0] = LIBRARY_MAGIC0;
    msg[1] = LIBRARY_MAGIC1;
    msg[2] = LIBRARY_MAGIC2;
    msg[3] = LIBRARY_MAGIC3;
    msg[4] = LIBRARY_PROTOCOL_VERSION;
    msg[5] = LIBRARY_MSG_SELECTION;
    msg[6] = selected ? LIBRARY_SELECTION_STATUS_SELECTED : LIBRARY_SELECTION_STATUS_CANCELLED;

    uint8_t idLen = 0;
    if (selected && titleId != NULL)
    {
        size_t len = strlen(titleId);
        idLen = (uint8_t)(len < LIBRARY_TITLE_ID_WIRE_MAX ? len : LIBRARY_TITLE_ID_WIRE_MAX);
        memcpy(&msg[8], titleId, idLen);
    }
    msg[7] = idLen;

    if (sendLibraryExact(sock, msg, sizeof(msg)) != 1)
    {
        orbitnx_log("library", "failed to send selection response");
        return false;
    }
    return true;
}

static void drawLibraryCard(RenderContext *context, int x, int y, int w, int h, bool isSelected, const LibraryGame *game)
{
    const int cornerRadius = 14;
    const int borderThickness = 2;

    SDL_Color borderColour = isSelected ? (SDL_Color){255, 200, 60, 255} : (SDL_Color){255, 255, 255, 160};
    SDL_Color fillColour = {0, 0, 0, 130};

    fillRoundedRect(context, x, y, w, h, cornerRadius, fillColour);
    for (int i = 0; i < borderThickness; i++)
    {
        drawRoundedRect(context, x + i, y + i, w - i * 2, h - i * 2, cornerRadius, borderColour);
    }

    // Same labelHeight/offset values already proven live to clear the font without touching the
    // card's rounded border (see menu.c's drawCard, tuned the same way after the exact same
    // complaint) - a smaller value here clipped the label against the bottom edge.
    const int labelHeight = 70;
    const int iconPadding = 8;
    int iconAreaW = w - iconPadding * 2;
    int iconAreaH = h - labelHeight - iconPadding * 2;
    int iconSize = iconAreaW < iconAreaH ? iconAreaW : iconAreaH;
    SDL_Rect iconDest = {
        x + (w - iconSize) / 2,
        y + iconPadding + (iconAreaH - iconSize) / 2,
        iconSize, iconSize};

    if (game->iconTexture != NULL)
    {
        SDL_RenderCopy(context->renderer, game->iconTexture, NULL, &iconDest);
    }
    else
    {
        // Placeholder when the icon failed to decode/wasn't sent - keeps the grid stable rather
        // than leaving a hole.
        SDL_Color placeholder = {40, 40, 46, 255};
        draw_rect(context, iconDest.x, iconDest.y, iconDest.w, iconDest.h, placeholder);
    }

    SDL_Color labelColour = isSelected ? (SDL_Color){255, 255, 255, 255} : (SDL_Color){210, 210, 210, 255};
    int labelWidth = FC_GetWidth(context->font, "%s", game->title);
    int maxLabelWidth = w - 8;
    // Titles can be long (up to 127 stored chars) - centering text wider than the card would push
    // it off both edges, so fall back to left-aligned instead.
    int labelX = labelWidth <= maxLabelWidth ? (x + w / 2 - labelWidth / 2) : (x + 4);
    SDL_DrawText(context, labelX, y + h - labelHeight + 14, labelColour, game->title);
}

static void drawPickerFrame(RenderContext *context, LibraryGame *games, int count, int selectedIndex)
{
    SDL_Color bg = {5, 5, 8, 255};
    SDL_ClearScreen(context, bg);
    drawOrbitNxTopBar(context);

    const int cardW = 260, cardH = 300, gapX = 24, gapY = 24;
    const int totalWidth = cardW * LIBRARY_PAGE_COLS + gapX * (LIBRARY_PAGE_COLS - 1);
    const int firstX = (RESX - totalWidth) / 2;
    const int totalHeight = cardH * LIBRARY_PAGE_ROWS + gapY * (LIBRARY_PAGE_ROWS - 1);
    const int topBarHeight = 90, ipLineHeight = 60;
    const int firstY = topBarHeight + (RESY - topBarHeight - ipLineHeight - totalHeight) / 2;

    const int perPage = LIBRARY_PAGE_COLS * LIBRARY_PAGE_ROWS;
    int page = selectedIndex / perPage;
    int pageStart = page * perPage;

    for (int slot = 0; slot < perPage; slot++)
    {
        int idx = pageStart + slot;
        if (idx >= count) break;

        int col = slot % LIBRARY_PAGE_COLS;
        int row = slot / LIBRARY_PAGE_COLS;
        int x = firstX + col * (cardW + gapX);
        int y = firstY + row * (cardH + gapY);
        drawLibraryCard(context, x, y, cardW, cardH, idx == selectedIndex, &games[idx]);
    }

    drawIpAddressLine(context);
    SDL_RenderPresent(context->renderer);
}

static bool runPickerLoop(RenderContext *context, LibraryGame *games, int count, PadState *pad, int *outSelectedIndex)
{
    int selectedIndex = 0;
    u64 lastInputTick = armGetSystemTick();

    while (appletMainLoop())
    {
        padUpdate(pad);
        u64 down = padGetButtonsDown(pad);
        bool hadInput = true;

        if (down & HidNpadButton_Left) selectedIndex = (selectedIndex - 1 + count) % count;
        else if (down & HidNpadButton_Right) selectedIndex = (selectedIndex + 1) % count;
        else if (down & HidNpadButton_Up) selectedIndex = (selectedIndex - LIBRARY_PAGE_COLS + count) % count;
        else if (down & HidNpadButton_Down) selectedIndex = (selectedIndex + LIBRARY_PAGE_COLS) % count;
        else if (down & HidNpadButton_A)
        {
            *outSelectedIndex = selectedIndex;
            return true;
        }
        else if (down & HidNpadButton_B)
        {
            return false;
        }
        else
        {
            hadInput = false;
        }

        if (hadInput) lastInputTick = armGetSystemTick();

        if (ticksSinceToSeconds(lastInputTick) >= LIBRARY_PICKER_INACTIVITY_DEADLINE_SECONDS)
        {
            orbitnx_log("library", "picker inactivity deadline reached, cancelling");
            return false;
        }

        drawPickerFrame(context, games, count, selectedIndex);
        svcSleepThread(14285714ULL); // ~70fps, matches the rest of the app's screens
    }
    return false;
}

bool runLibraryPickerScreen(RenderContext *context, StarSystem *starSystem, char *outTitleId, size_t outTitleIdCap)
{
    LibrarySocket *sock = createLibrarySocket();

    PadState libraryPad;
    padInitializeDefault(&libraryPad);

    if (!waitForPcConnection(context, starSystem, sock, &libraryPad))
    {
        freeLibrarySocket(sock);
        return false;
    }

    static LibraryGame games[LIBRARY_MAX_GAMES];
    int count = 0;
    bool received = receiveGameList(context, sock, games, &count);

    if (!received || count == 0)
    {
        sendSelectionResponse(sock, false, NULL);
        closeLibraryClientConnection(sock);
        freeLibrarySocket(sock);
        return false;
    }

    int selectedIndex = -1;
    bool picked = runPickerLoop(context, games, count, &libraryPad, &selectedIndex);

    bool result = false;
    if (picked && selectedIndex >= 0 && selectedIndex < count)
    {
        size_t idLen = strlen(games[selectedIndex].titleId);
        if (idLen < outTitleIdCap)
        {
            memcpy(outTitleId, games[selectedIndex].titleId, idLen + 1);
            result = true;
        }
        sendSelectionResponse(sock, true, games[selectedIndex].titleId);
    }
    else
    {
        sendSelectionResponse(sock, false, NULL);
    }

    for (int i = 0; i < count; i++)
    {
        if (games[i].iconTexture != NULL) SDL_DestroyTexture(games[i].iconTexture);
    }

    closeLibraryClientConnection(sock);
    freeLibrarySocket(sock);
    return result;
}
