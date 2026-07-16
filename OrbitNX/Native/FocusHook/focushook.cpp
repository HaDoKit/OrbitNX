// Injected into steam.exe so Big Picture's own "am I focused?" check (which polls
// GetForegroundWindow() directly rather than trusting WM_ACTIVATE-family messages - confirmed by
// a failed SendMessage-based experiment) always sees its own window as foreground, without any
// real OS-level focus/desktop change on the host machine.
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <cstring>
#include <cstdio>

static char g_logPath[MAX_PATH] = {0};

// Diagnostic-only: writing to a plain file next to the DLL is the only way to see what's
// happening inside an injected process without a debugger attached to steam.exe.
static void LogLine(const char *fmt, ...)
{
    if (g_logPath[0] == '\0') return;
    FILE *f = fopen(g_logPath, "a");
    if (!f) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);

    fprintf(f, "\n");
    fclose(f);
}

typedef HWND(WINAPI *GetForegroundWindow_t)();
static GetForegroundWindow_t Real_GetForegroundWindow = nullptr;

// Signaled by the C# host (AppOrchestrator, see OrbitNX/Native/GameActiveSignal.cs) whenever
// a launched game - not Big Picture itself - is the active content window. Confirmed live (user
// report): with the spoof unconditional, a game and Big Picture both reacted to the same gamepad
// input at once - ViGEmBus's virtual XInput controller isn't gated by real OS window focus the way
// keyboard/mouse are (any process polling XInputGetState() gets it regardless of focus), so once
// this hook made Big Picture permanently believe it was foreground, Steam Input kept routing to Big
// Picture's own desktop binding profile *in addition to* the game, rather than yielding control to
// the game's profile the way it would if Big Picture correctly saw itself lose real focus. This
// event lets the spoof step aside during exactly that window: while a game is active, real
// GetForegroundWindow() is returned unmodified (should genuinely be the game's window by then, so
// Big Picture correctly concludes it isn't focused and Steam Input stops double-routing to it) -
// the spoof only needs to hold while Big Picture itself is the active content, so a launcher
// window or the first user's own unrelated desktop window doesn't strip its focus.
static HANDLE g_gameActiveEvent = nullptr;
static const char *kGameActiveEventName = "OrbitNX_GameActive";

static bool IsGameActive()
{
    if (!g_gameActiveEvent)
    {
        // Opened lazily rather than at DLL_PROCESS_ATTACH - the host creates this event itself
        // (see GameActiveSignal.cs) and may not have done so yet at the exact moment this DLL is
        // injected; every hook call retries via OpenEvent until it succeeds, cheap enough to just
        // attempt on every call rather than adding separate init-order bookkeeping.
        g_gameActiveEvent = OpenEventA(SYNCHRONIZE, FALSE, kGameActiveEventName);
    }
    if (!g_gameActiveEvent) return false; // host hasn't created it yet - default to "not active" (spoof stays on)
    return WaitForSingleObject(g_gameActiveEvent, 0) == WAIT_OBJECT_0;
}

struct FindContext
{
    HWND result;
    int scanned;
    int ownedByUs;
};

// Confirmed live: the SDL_app/Big Picture window does NOT belong to the same process that
// performs the focus check (steamwebhelper.exe owns the window; steam.exe's own steamclient64.dll
// instance is apparently the one calling GetForegroundWindow) - filtering by "belongs to my own
// process" here would miss it entirely regardless of which process this DLL is hooking inside of.
// Match purely by class on the current desktop instead, same signature the C# host's own
// FindBigPictureWindow uses.
//
// Originally also required the window title to contain "Big Picture" - confirmed live that after
// closing a game launched from Big Picture, the visible SDL_app window's title can simply read
// "Steam" instead (a transitional/returning-to-Big-Picture state), which this check missed
// entirely, breaking gamepad focus. Confirmed live via EnumWindows that "SDL_app" is never used by
// more than one *visible* top-level window at a time on this system, so the class name alone is a
// reliable, title-independent match - dropped the title requirement entirely.
// Matches SteamLauncher.cs's CandidateClassNames - confirmed live (user report): a settings
// dialog opened from within Big Picture went completely uncontrollable by gamepad while the main
// Big Picture window behind it stayed fine, consistent with that dialog being a separate
// "vguiPopupWindow"-classed top-level window (Steam's older VGUI dialog system, distinct from the
// SDL3-based main UI) that this check never recognized as "ours" - GetForegroundWindow() kept
// getting spoofed to the main SDL_app window underneath instead of the popup actually on top and
// receiving real input focus, so Big Picture's own focus check for the popup never matched. This
// was previously SDL_app-only here, out of sync with the C# side's list, presumably because no
// popup had been hit by live testing yet.
static const char *kOwnWindowClasses[] = {"SDL_app", "vguiPopupWindow"};

static BOOL CALLBACK FindOwnWindowProc(HWND hwnd, LPARAM lParam)
{
    auto *ctx = (FindContext *)lParam;
    ctx->scanned++;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) ctx->ownedByUs++;

    if (!IsWindowVisible(hwnd)) return TRUE;

    char cls[64] = {0};
    GetClassNameA(hwnd, cls, sizeof(cls));

    bool matches = false;
    for (const char *candidate : kOwnWindowClasses)
    {
        if (lstrcmpiA(cls, candidate) == 0) { matches = true; break; }
    }
    if (!matches) return TRUE;

    // EnumWindows enumerates in Z-order (topmost first), so the first match here is already the
    // topmost one of interest - e.g. a settings popup drawn on top of the main window wins over the
    // main window itself, which is what should be treated as focused while it's up.
    ctx->result = hwnd;
    return FALSE;
}

static volatile LONG g_hookCallCount = 0;

static HWND WINAPI Hook_GetForegroundWindow()
{
    LONG callNum = InterlockedIncrement(&g_hookCallCount);

    // See IsGameActive's doc comment: while a game (not Big Picture itself) is the active content,
    // step aside entirely and answer honestly, so Big Picture's own focus check correctly concludes
    // it has lost focus instead of both it and the game reacting to the same gamepad input at once.
    bool gameActive = IsGameActive();
    if (gameActive)
    {
        if (callNum <= 20 || callNum % 200 == 0)
        {
            LogLine("Hook_GetForegroundWindow call #%ld: game active, spoof suppressed", callNum);
        }
        return Real_GetForegroundWindow ? Real_GetForegroundWindow() : NULL;
    }

    // Steam/Big Picture now run on the normal interactive desktop (the hidden-Desktop-object
    // architecture this hook was originally written for was abandoned - see SteamLauncher.cs) -
    // EnumWindows() against the calling thread's current desktop is exactly the right scope here,
    // no need to open anything by name.
    FindContext ctx = {NULL, 0, 0};
    EnumWindows(FindOwnWindowProc, (LPARAM)&ctx);

    // Log only occasionally - this can be called very frequently (every frame) and we don't want
    // to flood the file or slow down the hooked call itself.
    if (callNum <= 20 || callNum % 200 == 0)
    {
        LogLine("Hook_GetForegroundWindow call #%ld: scanned=%d ownedByUs=%d result=0x%p",
            callNum, ctx.scanned, ctx.ownedByUs, ctx.result);
    }

    if (ctx.result != NULL) return ctx.result;
    return Real_GetForegroundWindow ? Real_GetForegroundWindow() : NULL;
}

// MinGW GCC has no __try/__except (MSVC-only extension), so safety here comes from explicit
// bounds checks against the module's mapped size rather than structured exception handling -
// every loaded module is a Windows-loader-verified valid PE, so a well-formed parse that stays
// within SizeOfImage should never actually fault.
static int PatchModuleImport(HMODULE hModule, char *moduleNameOut, size_t moduleNameOutSize)
{
    if (!hModule) return 0;
    BYTE *base = (BYTE *)hModule;

    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return -1;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return -1;

    DWORD sizeOfImage = nt->OptionalHeader.SizeOfImage;
    auto inBounds = [&](DWORD rva, size_t len) { return rva != 0 && (size_t)rva + len <= sizeOfImage; };

    IMAGE_DATA_DIRECTORY importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!inBounds(importDir.VirtualAddress, sizeof(IMAGE_IMPORT_DESCRIPTOR))) return 0;

    int patched = 0;
    bool importsUser32 = false;

    for (DWORD descRva = importDir.VirtualAddress; inBounds(descRva, sizeof(IMAGE_IMPORT_DESCRIPTOR)); descRva += sizeof(IMAGE_IMPORT_DESCRIPTOR))
    {
        IMAGE_IMPORT_DESCRIPTOR *desc = (IMAGE_IMPORT_DESCRIPTOR *)(base + descRva);
        if (desc->Name == 0) break;
        if (!inBounds(desc->Name, 1)) continue;
        const char *dllName = (const char *)(base + desc->Name);
        if (lstrcmpiA(dllName, "user32.dll") != 0) continue;
        importsUser32 = true;

        auto thunkRva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
        if (!inBounds(thunkRva, sizeof(IMAGE_THUNK_DATA)) || !inBounds(desc->FirstThunk, sizeof(IMAGE_THUNK_DATA))) continue;

        IMAGE_THUNK_DATA *origThunk = (IMAGE_THUNK_DATA *)(base + thunkRva);
        IMAGE_THUNK_DATA *iatThunk = (IMAGE_THUNK_DATA *)(base + desc->FirstThunk);

        for (; origThunk->u1.AddressOfData != 0; origThunk++, iatThunk++)
        {
            if (origThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            DWORD nameRva = (DWORD)origThunk->u1.AddressOfData;
            if (!inBounds(nameRva, sizeof(IMAGE_IMPORT_BY_NAME))) continue;

            IMAGE_IMPORT_BY_NAME *byName = (IMAGE_IMPORT_BY_NAME *)(base + nameRva);
            if (strcmp((const char *)byName->Name, "GetForegroundWindow") != 0) continue;

            if (!Real_GetForegroundWindow)
            {
                Real_GetForegroundWindow = (GetForegroundWindow_t)(void *)iatThunk->u1.Function;
            }

            DWORD oldProtect;
            if (VirtualProtect(&iatThunk->u1.Function, sizeof(void *), PAGE_READWRITE, &oldProtect))
            {
                iatThunk->u1.Function = (ULONG_PTR)(void *)Hook_GetForegroundWindow;
                VirtualProtect(&iatThunk->u1.Function, sizeof(void *), oldProtect, &oldProtect);
                patched++;
            }
        }
    }

    if (patched > 0 && moduleNameOut)
    {
        GetModuleBaseNameA(GetCurrentProcess(), hModule, moduleNameOut, (DWORD)moduleNameOutSize);
    }
    return importsUser32 ? patched : 0;
}

static void PatchAllModules()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE)
    {
        LogLine("CreateToolhelp32Snapshot failed, err=%lu", GetLastError());
        return;
    }

    MODULEENTRY32 me;
    me.dwSize = sizeof(me);
    int moduleCount = 0;
    int totalPatched = 0;
    char patchedNames[512] = {0};

    if (Module32First(snap, &me))
    {
        do
        {
            moduleCount++;
            char nameBuf[MAX_PATH] = {0};
            int patched = PatchModuleImport(me.hModule, nameBuf, sizeof(nameBuf));
            if (patched > 0)
            {
                totalPatched += patched;
                strncat(patchedNames, me.szModule, sizeof(patchedNames) - strlen(patchedNames) - 2);
                strncat(patchedNames, ",", sizeof(patchedNames) - strlen(patchedNames) - 1);
            }
        } while (Module32Next(snap, &me));
    }
    else
    {
        LogLine("Module32First failed, err=%lu", GetLastError());
    }
    CloseHandle(snap);

    static int lastModuleCount = -1;
    static int lastTotalPatched = -1;
    if (moduleCount != lastModuleCount || totalPatched != lastTotalPatched)
    {
        LogLine("scan: modules=%d totalGetForegroundWindowImportsPatched=%d patchedIn=[%s]",
            moduleCount, totalPatched, patchedNames);
        lastModuleCount = moduleCount;
        lastTotalPatched = totalPatched;
    }
}

static DWORD WINAPI WorkerThread(LPVOID)
{
    LogLine("WorkerThread started, pid=%lu", GetCurrentProcessId());
    // Re-scan periodically: Steam loads additional modules (e.g. per-game overlay/controller
    // config DLLs) after Big Picture is already up, and each needs its own IAT patched.
    for (;;)
    {
        PatchAllModules();
        Sleep(2000);
    }
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hinst);

        char modulePath[MAX_PATH] = {0};
        GetModuleFileNameA(hinst, modulePath, sizeof(modulePath));
        char *lastSlash = strrchr(modulePath, '\\');
        if (lastSlash) *(lastSlash + 1) = '\0';
        snprintf(g_logPath, sizeof(g_logPath), "%sfocushook.log", modulePath);

        LogLine("DLL_PROCESS_ATTACH, pid=%lu", GetCurrentProcessId());
        CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
