using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32;
using OrbitNX.Native;

namespace OrbitNX.BigPicture;

/// <summary>
/// Launches Steam (and therefore Big Picture) on the normal interactive desktop and leaves its
/// windows exactly where Steam itself puts them - no hiding, no moving onto a separate virtual
/// monitor. Big Picture/games are visible on the real screen.
/// </summary>
public sealed class SteamLauncher
{
    // Matched by window class only, not title: Big Picture's SDL_app window can read "Steam"
    // instead of "Big Picture" during a transitional/returning-to-Big-Picture state, and "SDL_app"
    // is never used by more than one visible top-level window at a time, so the class name alone
    // is a reliable match.
    private static readonly string[] CandidateClassNames = { "SDL_app", "vguiPopupWindow" };

    /// <summary>
    /// Launches Steam if it isn't already running, then opens Big Picture. Deliberately does NOT
    /// shut down an already-running Steam first - a session's end only closes the game that was
    /// running and exits Big Picture (see CloseActiveGame/ExitBigPicture), leaving Steam itself
    /// running so the next connect can jump straight back into Big Picture without a relaunch.
    /// </summary>
    public async Task LaunchSteamAsync(CancellationToken ct)
    {
        if (Process.GetProcessesByName("steam").Length == 0)
        {
            var steamExe = GetSteamExePath();
            Logger.Log("SteamLauncher", $"LaunchSteamAsync: steamExe={steamExe ?? "(null)"}");
            if (steamExe is null)
            {
                Logger.Log("SteamLauncher", "Could not find Steam install path in registry (HKCU\\Software\\Valve\\Steam\\SteamExe)");
                return;
            }

            Process.Start(new ProcessStartInfo(steamExe, "-silent") { UseShellExecute = true });
            Logger.Log("SteamLauncher", $"launched Steam ({steamExe})");

            // Give the client a moment to reach its "ready for IPC" state before asking for Big Picture.
            await Task.Delay(4000, ct);
        }

        OpenBigPicture();
        Logger.Log("SteamLauncher", "LaunchSteamAsync: OpenBigPicture invoked, returning");
    }

    public void OpenBigPicture()
    {
        Process.Start(new ProcessStartInfo("steam://open/bigpicture") { UseShellExecute = true });
    }

    /// <summary>
    /// Leaves Big Picture Mode back to Steam's normal desktop UI - called when the client
    /// disconnects. The undocumented "steam://exitbigpicture" URI is a silent no-op on the current
    /// Steam client, so this posts WM_CLOSE to the Big Picture window instead, which correctly
    /// leaves Big Picture rather than quitting Steam entirely.
    /// </summary>
    public void ExitBigPicture()
    {
        var hwnd = FindBigPictureWindow();
        if (hwnd == IntPtr.Zero)
        {
            Logger.Log("SteamLauncher", "ExitBigPicture: no Big Picture window found, nothing to close");
            return;
        }

        PostMessage(hwnd, WM_CLOSE, IntPtr.Zero, IntPtr.Zero);
        Logger.Log("SteamLauncher", $"ExitBigPicture: WM_CLOSE posted to hwnd={hwnd}");
    }

    /// <summary>
    /// Polls for the Big Picture window and injects FocusHook. Returns its handle, or
    /// IntPtr.Zero if it never appeared.
    /// </summary>
    public async Task<IntPtr> WaitForWindowAsync(TimeSpan timeout, CancellationToken ct)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline && !ct.IsCancellationRequested)
        {
            var hwnd = FindBigPictureWindow();
            if (hwnd != IntPtr.Zero)
            {
                // Which process actually performs Big Picture's focus check isn't reliably
                // identifiable from the window alone (steam.exe and every steamwebhelper.exe
                // instance all load steamclient64.dll, any of which could be the one doing it), so
                // inject into all of them rather than guessing a single target.
                InjectFocusHookIntoAllSteamProcesses();
                return hwnd;
            }
            await Task.Delay(300, ct);
        }
        Logger.Log("SteamLauncher", "WaitForWindowAsync: timed out without finding Big Picture window");
        return IntPtr.Zero;
    }

    /// <summary>
    /// Allow-list, not deny-list: a candidate window only counts as "content" if its owning
    /// process is steam.exe/steamwebhelper.exe by name, or its executable lives under the Steam
    /// installation directory (covers games in the default library) - anything else, no matter
    /// how large or long-lived, is left completely alone. A deny-list is not safe enough here -
    /// it risks grabbing an unrelated large visible window (e.g. the user's own applications).
    /// </summary>
    private static readonly string[] SteamProcessNames = { "steam", "steamwebhelper" };

    private static bool IsSteamFamilyProcess(Process process, string? steamInstallDir)
    {
        if (Array.IndexOf(SteamProcessNames, process.ProcessName) >= 0) return true;
        if (steamInstallDir is null) return false;

        try
        {
            var exePath = process.MainModule?.FileName;
            return exePath is not null && exePath.StartsWith(steamInstallDir, StringComparison.OrdinalIgnoreCase);
        }
        catch
        {
            // MainModule can throw (access denied across integrity levels, 32/64-bit mismatch,
            // process exited mid-query) - treat as "not ours" rather than risk a false positive.
            return false;
        }
    }

    /// <summary>The hwnd FindActiveContentWindowAndInjectFocusHook picked on its most recent call
    /// (IntPtr.Zero if none). Exposed so AppOrchestrator can target a real, one-shot
    /// ForceForegroundWindow call at exactly this window on the Big-Picture-to-game transition -
    /// see ForceForegroundWindow's doc comment for why a real focus change is still necessary
    /// despite the FocusHook/ReassertFocus approach.</summary>
    public IntPtr LastActiveContentWindow { get; private set; }

    /// <summary>
    /// Finds whichever visible top-level window belonging to the Steam family is currently the
    /// "real content" - Big Picture's own window while navigating menus, or a launched game's
    /// window once one is running - and injects FocusHook into its owning process. Launching a
    /// game from Big Picture opens a second, separate top-level window (e.g. class "HL_WIN") with
    /// its own focus check, needing its own injection. Rather than hardcode per-engine window
    /// classes, this picks the largest visible Steam-family window above a size floor (excludes
    /// small utility/overlay windows like the ~50x50 UAC input indicator) - the actual content
    /// (Big Picture or a game) is always fullscreen-sized. Windows are left exactly where
    /// Steam/the game itself puts them - no repositioning.
    ///
    /// Returns whether the currently active content window belongs to an actual game (true) or to
    /// steam.exe/steamwebhelper.exe itself, i.e. Big Picture (false) - null if no Steam-family
    /// content window was found at all. Used by AppOrchestrator to detect the game-just-closed
    /// transition (true -&gt; false) and kick the virtual gamepad, since Steam Input's own
    /// per-context (game vs. Big Picture) controller binding handoff doesn't always pick a ViGEm
    /// virtual controller back up cleanly on its own.
    /// </summary>
    public bool? FindActiveContentWindowAndInjectFocusHook()
    {
        const int minDimension = 400;
        var steamInstallDir = Path.GetDirectoryName(GetSteamExePath());

        var best = IntPtr.Zero;
        var bestArea = 0L;

        EnumWindowsProc callback = (hWnd, _) =>
        {
            if (!IsWindowVisible(hWnd)) return true;
            GetWindowRect(hWnd, out var rect);
            var width = rect.Right - rect.Left;
            var height = rect.Bottom - rect.Top;
            if (width < minDimension || height < minDimension) return true;

            GetWindowThreadProcessId(hWnd, out var ownerPid);
            try
            {
                var process = Process.GetProcessById((int)ownerPid);
                if (!IsSteamFamilyProcess(process, steamInstallDir)) return true;
            }
            catch (ArgumentException) { return true; /* process exited between EnumWindows and this check */ }

            var area = (long)width * height;
            if (area > bestArea)
            {
                bestArea = area;
                best = hWnd;
            }
            return true;
        };

        EnumWindows(callback, IntPtr.Zero);

        LastActiveContentWindow = best;
        if (best == IntPtr.Zero) return null;

        GetWindowThreadProcessId(best, out var bestOwnerPid);
        try
        {
            var ownerProcess = Process.GetProcessById((int)bestOwnerPid);
            InjectFocusHookIntoProcess((int)bestOwnerPid, ownerProcess.ProcessName);
            ReassertFocus(best);
            var isGame = Array.IndexOf(SteamProcessNames, ownerProcess.ProcessName) < 0;

            // Diagnostic: logs what this heuristic picks every tick alongside real OS foreground,
            // to catch "best" flipping back to Big Picture while a game is running (which would
            // explain ReassertFocus re-activating Big Picture's own window).
            var classNameSb = new StringBuilder(64);
            GetClassName(best, classNameSb, classNameSb.Capacity);
            var realForeground = GetForegroundWindow();
            GetWindowThreadProcessId(realForeground, out var realForegroundPid);
            string realForegroundProcessName;
            try { realForegroundProcessName = Process.GetProcessById((int)realForegroundPid).ProcessName; }
            catch { realForegroundProcessName = "?"; }
            Logger.Log("SteamLauncher", $"tick: best=0x{best:X} class={classNameSb} process={ownerProcess.ProcessName} isGame={isGame} | realForeground=0x{realForeground:X} process={realForegroundProcessName}");

            // DWM can silently capture-block a borderless game window the same way it does true
            // exclusive fullscreen - see FullscreenOptimizationFix's doc comment. Only applies to
            // games, not steam.exe/steamwebhelper.exe themselves. Disabling fullscreen
            // optimizations does not fix every capture-block case, but is harmless to always set.
            if (isGame)
            {
                var exePath = ownerProcess.MainModule?.FileName;
                if (exePath is not null) FullscreenOptimizationFix.EnsureDisabled(exePath);

                // Diagnostic: GetWindowDisplayAffinity reports whether the window itself asked
                // Windows to exclude it from any capture API - WDA_EXCLUDEFROMCAPTURE (0x11). If a
                // game sets this, gfxcapture (or any other capture method) cannot see it at all -
                // no software fix on this end is possible. Logged once per detected exe.
                if (GetWindowDisplayAffinity(best, out var affinity))
                {
                    Logger.Log("SteamLauncher", $"game window display affinity for {ownerProcess.ProcessName} (hwnd={best}): {affinity} ({(affinity == WDA_EXCLUDEFROMCAPTURE ? "EXCLUDED FROM CAPTURE" : "capturable")})");
                }
            }

            return isGame;
        }
        catch (ArgumentException)
        {
            /* process exited between finding the window and here */
            return null;
        }
    }

    /// <summary>
    /// Closes whatever game is currently running under Steam (if any) - called when the client
    /// disconnects, alongside ExitBigPicture. Deliberately does NOT touch
    /// steam.exe/steamwebhelper.exe itself (see IsSteamFamilyProcess for why those are excluded
    /// here) - only an actual game process, identified by living under the Steam installation
    /// directory.
    /// </summary>
    public void CloseActiveGame()
    {
        var steamInstallDir = Path.GetDirectoryName(GetSteamExePath());
        if (steamInstallDir is null) return;

        foreach (var process in Process.GetProcesses())
        {
            try
            {
                if (Array.IndexOf(SteamProcessNames, process.ProcessName) >= 0) continue;
                var exePath = process.MainModule?.FileName;
                if (exePath is null || !exePath.StartsWith(steamInstallDir, StringComparison.OrdinalIgnoreCase)) continue;

                Logger.Log("SteamLauncher", $"CloseActiveGame: closing {process.ProcessName} (pid={process.Id})");
                process.CloseMainWindow();
                if (!process.WaitForExit(3000)) process.Kill();
            }
            catch
            {
                // process already exited, access denied (different integrity level/bitness), or
                // similar - nothing more we can safely do for this one, move on to the next.
            }
        }
    }

    /// <summary>
    /// Posts the same WM_ACTIVATEAPP/WM_NCACTIVATE/WM_ACTIVATE/WM_SETFOCUS sequence a real focus
    /// change would send, alongside (not instead of) the GetForegroundWindow hook. Big Picture's
    /// own "am I focused?" check polls GetForegroundWindow() directly, but this SDL3-based UI
    /// separately tracks its own internal keyboard/gamepad focus flag off real WM_ACTIVATE-family
    /// messages - messages that never naturally arrive when the window was already "foreground"
    /// the whole time from Windows' point of view (e.g. right after closing a game). Sending these
    /// on every watcher tick, alongside the hook, covers that gap.
    ///
    /// Does not call SetForegroundWindow: real OS foreground is a single, global, shared resource,
    /// and calling it every tick against a heuristic guess would steal focus away from an actual
    /// running game onto Big Picture's own window - see ForceForegroundWindow for the one-shot,
    /// confirmed-transition-only alternative.
    /// </summary>
    private static void ReassertFocus(IntPtr hwnd)
    {
        const int WA_ACTIVE = 1;
        SendMessage(hwnd, WM_ACTIVATEAPP, (IntPtr)1, IntPtr.Zero);
        SendMessage(hwnd, WM_NCACTIVATE, (IntPtr)1, IntPtr.Zero);
        SendMessage(hwnd, WM_ACTIVATE, (IntPtr)WA_ACTIVE, IntPtr.Zero);
        SendMessage(hwnd, WM_SETFOCUS, IntPtr.Zero, IntPtr.Zero);
    }

    /// <summary>
    /// A genuine, one-shot real-OS-foreground-focus change, called by AppOrchestrator exactly once
    /// on the confirmed Big-Picture-to-game transition, targeting the exact hwnd that triggered it.
    /// Necessary because ViGEmBus's virtual XInput controller isn't focus-gated, but Steam Input's
    /// own per-context binding keys off *real* OS foreground - and nothing else in this app calls
    /// SetForegroundWindow, so real focus would otherwise stay wherever it was before the game
    /// launched (typically Big Picture's own window, since window creation alone doesn't grant
    /// foreground activation rights under Windows' focus-stealing prevention). GameActiveSignal's
    /// spoof-suppression alone is not sufficient: it only stops FocusHook from lying about who's
    /// focused, it can't manufacture a real focus change that never happened.
    ///
    /// SetForegroundWindow alone is usually refused by Windows' focus-stealing prevention unless the
    /// calling thread is already "attached" (via AttachThreadInput) to the thread that currently
    /// owns real foreground - this temporarily attaches to borrow that permission, the standard
    /// documented workaround for a background process needing to force a real focus change.
    /// </summary>
    public static void ForceForegroundWindow(IntPtr hwnd)
    {
        if (hwnd == IntPtr.Zero) return;

        var foregroundHwnd = GetForegroundWindow();
        var foregroundThreadId = GetWindowThreadProcessId(foregroundHwnd, out _);
        var targetThreadId = GetWindowThreadProcessId(hwnd, out _);
        var currentThreadId = GetCurrentThreadId();

        var attachedToForeground = foregroundThreadId != currentThreadId && AttachThreadInput(currentThreadId, foregroundThreadId, true);
        var attachedToTarget = targetThreadId != currentThreadId && AttachThreadInput(currentThreadId, targetThreadId, true);
        try
        {
            var ok = SetForegroundWindow(hwnd);
            BringWindowToTop(hwnd);
            Logger.Log("SteamLauncher", $"ForceForegroundWindow(hwnd={hwnd}): SetForegroundWindow returned {ok}");
        }
        finally
        {
            if (attachedToForeground) AttachThreadInput(currentThreadId, foregroundThreadId, false);
            if (attachedToTarget) AttachThreadInput(currentThreadId, targetThreadId, false);
        }
    }

    private static string? GetSteamExePath()
    {
        using var key = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam");
        return key?.GetValue("SteamExe") as string;
    }

    private static IntPtr FindBigPictureWindow()
    {
        IntPtr found = IntPtr.Zero;

        EnumWindowsProc callback = (hWnd, _) =>
        {
            if (!IsWindowVisible(hWnd)) return true;

            var className = new StringBuilder(256);
            GetClassName(hWnd, className, className.Capacity);

            var title = new StringBuilder(256);
            GetWindowText(hWnd, title, title.Capacity);

            var classStr = className.ToString();
            var titleStr = title.ToString();

            var classMatches = Array.IndexOf(CandidateClassNames, classStr) >= 0;

            if (classMatches)
            {
                found = hWnd;
                Logger.Log("SteamLauncher", $"matched hwnd={hWnd} class={classStr} title='{titleStr}'");
                return false; // stop enumeration
            }
            return true;
        };

        EnumWindows(callback, IntPtr.Zero);
        return found;
    }

    /// <summary>
    /// Injects FocusHook.dll into steam.exe so Big Picture's internal "am I focused?" check -
    /// which polls GetForegroundWindow() directly rather than reacting to WM_ACTIVATE-family
    /// messages - always sees its own window as foreground. Real OS foreground is a single,
    /// shared, per-desktop concept, so the instant the first user clicks their own window, real
    /// focus moves away regardless of which monitor either window is physically on.
    ///
    /// Called periodically, not just once when Big Picture first appears: Steam can spin up a
    /// *new* steamwebhelper.exe instance to host Big Picture again after a game closes, rather
    /// than reusing the one this hook was already injected into - re-scanning all current
    /// steam.exe/steamwebhelper.exe processes catches that new instance within one watcher tick.
    /// Re-injecting into an already-hooked process is a safe no-op: LoadLibrary on an already-loaded
    /// module just bumps its ref count without re-running DllMain, so this never double-patches.
    /// </summary>
    public static void InjectFocusHookIntoAllSteamProcesses()
    {
        foreach (var name in SteamProcessNames)
        {
            foreach (var process in Process.GetProcessesByName(name))
            {
                // steamwebhelper.exe is CEF/Chromium-based - most of its instances are sandboxed
                // child processes (--type=renderer/gpu-process, --service-sandbox-type=audio/
                // service/...) that run under a restricted token specifically designed to block
                // LoadLibrary-style DLL injection. Only the main, unsandboxed browser/UI host
                // process (no "--type=" argument at all) can actually be injected into.
                if (name == "steamwebhelper" && HasSandboxTypeArgument(process)) continue;
                InjectFocusHookIntoProcess(process.Id, name);
            }
        }
    }

    private static bool HasSandboxTypeArgument(Process process)
    {
        try
        {
            using var searcher = new System.Management.ManagementObjectSearcher(
                $"SELECT CommandLine FROM Win32_Process WHERE ProcessId = {process.Id}");
            foreach (System.Management.ManagementObject obj in searcher.Get())
            {
                var cmdLine = obj["CommandLine"] as string;
                return cmdLine is not null && cmdLine.Contains("--type=", StringComparison.Ordinal);
            }
        }
        catch
        {
            // WMI unavailable or query failed - assume it's the main process rather than skip it.
        }
        return false;
    }

    /// <summary>
    /// A launched game is its own separate process with its own "am I focused?" check, unrelated
    /// to Big Picture's - without this, gamepad input to an actual game (as opposed to Big
    /// Picture's own menus) stops the instant real OS focus moves away.
    /// </summary>
    public static void InjectFocusHookIntoProcess(int processId, string processNameForLogging)
    {
        var dllPath = Path.Combine(AppContext.BaseDirectory, "focushook.dll");
        if (!File.Exists(dllPath))
        {
            Logger.Log("SteamLauncher", $"focushook.dll not found at {dllPath} - skipping injection");
            return;
        }

        var injected = DllInjector.Inject(processId, dllPath);
        Logger.Log("SteamLauncher", $"FocusHook injection into {processNameForLogging} (pid={processId}) success={injected}");
    }

    // --- Win32 interop ---

    private const uint WM_CLOSE = 0x0010;
    private const uint WM_ACTIVATE = 0x0006;
    private const uint WM_ACTIVATEAPP = 0x001C;
    private const uint WM_SETFOCUS = 0x0007;
    private const uint WM_NCACTIVATE = 0x0086;

    // Values SetWindowDisplayAffinity/GetWindowDisplayAffinity use (WDA_* constants).
    private const uint WDA_EXCLUDEFROMCAPTURE = 0x00000011;

    private delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern bool GetWindowDisplayAffinity(IntPtr hWnd, out uint pdwAffinity);

    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    private static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    private static extern IntPtr SendMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    private static extern int GetClassName(IntPtr hWnd, StringBuilder lpClassName, int nMaxCount);

    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    private static extern int GetWindowText(IntPtr hWnd, StringBuilder lpString, int nMaxCount);

    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);

    [DllImport("user32.dll")]
    private static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll")]
    private static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern bool BringWindowToTop(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern bool AttachThreadInput(uint idAttach, uint idAttachTo, bool fAttach);

    [DllImport("kernel32.dll")]
    private static extern uint GetCurrentThreadId();

    [StructLayout(LayoutKind.Sequential)]
    private struct RECT { public int Left, Top, Right, Bottom; }
}
