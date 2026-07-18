using System.Diagnostics;
using System.Runtime.InteropServices;

namespace OrbitNX.ShadPs4;

/// <summary>
/// Launches shadPS4 (https://shadps4.net) - a portable, installer-less PS4 emulator - on the
/// normal interactive desktop, exactly like SteamLauncher does for Steam/Big Picture but without
/// any of that class's Steam-Input-specific machinery (no focus-hook DLL injection, no DWM/game-
/// window detection, no AttachThreadInput focus-stealing dance). shadPS4 is an ordinary single
/// top-level window, so a plain launch/focus/close is all v1 needs; whether it keeps accepting the
/// virtual gamepad while unfocused is unverified and should be checked live before relying on it.
/// </summary>
public sealed class ShadPs4Launcher
{
    /// <summary>
    /// Launches shadPS4 if it isn't already running, or brings its window to the front if it is.
    /// Returns false (without launching anything) if <paramref name="exePath"/> is empty or the
    /// file doesn't exist - shadPS4 has no registry/install-path auto-detect (it's a portable zip,
    /// see the class doc comment), so an unset/wrong path is an expected, user-fixable state rather
    /// than an error to throw on.
    /// </summary>
    public Task<bool> LaunchOrFocusAsync(string exePath, CancellationToken ct)
    {
        if (string.IsNullOrWhiteSpace(exePath) || !File.Exists(exePath))
        {
            Logger.Log("ShadPs4Launcher", $"LaunchOrFocusAsync: exePath not configured or missing ('{exePath}')");
            return Task.FromResult(false);
        }

        var processName = Path.GetFileNameWithoutExtension(exePath);
        if (Process.GetProcessesByName(processName).Length > 0)
        {
            Focus(exePath);
            return Task.FromResult(true);
        }

        // WorkingDirectory matters here specifically because shadPS4 is portable: it creates its
        // "user" config folder next to the exe on first run, keyed off the process's working
        // directory rather than any installed/registered location.
        Process.Start(new ProcessStartInfo(exePath)
        {
            UseShellExecute = true,
            WorkingDirectory = Path.GetDirectoryName(exePath)!,
        });
        Logger.Log("ShadPs4Launcher", $"launched shadPS4 ({exePath})");
        return Task.FromResult(true);
    }

    /// <summary>Brings shadPS4's window to the foreground if it's already running. No
    /// AttachThreadInput dance (unlike SteamLauncher.ForceForegroundWindow) - accepted v1
    /// simplification; Windows' focus-stealing prevention may occasionally ignore this.</summary>
    public void Focus(string exePath)
    {
        var hwnd = FindMainWindow(exePath);
        if (hwnd == IntPtr.Zero) return;

        ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
    }

    /// <summary>
    /// Polls for shadPS4's main window, up to timeout. Much simpler than
    /// SteamLauncher.WaitForWindowAsync's EnumWindows/class-name matching - the exact process we
    /// launched is already known, so there's no Big-Picture-vs-game window ambiguity to resolve.
    /// </summary>
    public async Task<IntPtr> WaitForWindowAsync(string exePath, TimeSpan timeout, CancellationToken ct)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline && !ct.IsCancellationRequested)
        {
            var hwnd = FindMainWindow(exePath);
            if (hwnd != IntPtr.Zero) return hwnd;
            await Task.Delay(300, ct);
        }
        Logger.Log("ShadPs4Launcher", "WaitForWindowAsync: timed out without finding shadPS4 window");
        return IntPtr.Zero;
    }

    /// <summary>
    /// Closes shadPS4's window - called when the client disconnects. Unlike Steam (where Steam
    /// itself stays running and only Big Picture is exited), shadPS4 has no persistent background
    /// process to preserve - it's one ordinary window, so closing it is simply "close the app."
    /// </summary>
    public void Close(string exePath)
    {
        var hwnd = FindMainWindow(exePath);
        if (hwnd == IntPtr.Zero)
        {
            Logger.Log("ShadPs4Launcher", "Close: no shadPS4 window found, nothing to close");
            return;
        }

        PostMessage(hwnd, WM_CLOSE, IntPtr.Zero, IntPtr.Zero);
        Logger.Log("ShadPs4Launcher", $"Close: WM_CLOSE posted to hwnd={hwnd}");
    }

    /// <summary>Opens shadPS4's official download page in the default browser - used when
    /// AppOrchestrator reports the exe path isn't configured/found (see MainForm's
    /// ShadPs4NotConfigured handler).</summary>
    public static void OpenDownloadPage()
    {
        Process.Start(new ProcessStartInfo("https://shadps4.net/downloads/") { UseShellExecute = true });
    }

    /// <summary>Filenames shadPS4's official portable zip has shipped its executable under -
    /// "shadPS4QTLauncher.exe" is the current Qt-based launcher; "shadPS4.exe" covers older/renamed
    /// builds. Checked in this order wherever a candidate folder is scanned.</summary>
    private static readonly string[] KnownExeNames = { "shadPS4QTLauncher.exe", "shadPS4.exe" };

    /// <summary>
    /// Best-effort scan for an already-extracted shadPS4 install, since there's no registry entry
    /// to consult (see class doc comment) - checks a handful of common places a user would have
    /// extracted the portable zip to (Desktop, Downloads, Program Files, drive roots), plus any
    /// top-level folder at those locations whose name contains "shadps4". Not exhaustive by design -
    /// a portable app can end up literally anywhere, so this only ever saves the common case; the
    /// manual "Обзор" picker in MainForm remains the reliable fallback. Returns null if nothing
    /// matched.
    /// </summary>
    public static string? TryAutoDetect()
    {
        var roots = new List<string>
        {
            Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory),
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Downloads"),
            Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments),
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
            Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86),
        };

        try
        {
            foreach (var drive in DriveInfo.GetDrives())
            {
                if (drive.DriveType == DriveType.Fixed && drive.IsReady)
                {
                    roots.Add(drive.RootDirectory.FullName);
                }
            }
        }
        catch (IOException)
        {
            // Best-effort - a drive enumeration failure just means fewer roots get checked.
        }

        foreach (var root in roots)
        {
            if (string.IsNullOrEmpty(root) || !Directory.Exists(root)) continue;

            // The root itself might already be the extracted folder (e.g. the user extracted
            // straight into a drive root or Downloads with no wrapping subfolder).
            foreach (var exeName in KnownExeNames)
            {
                var direct = Path.Combine(root, exeName);
                if (File.Exists(direct)) return direct;
            }

            IEnumerable<string> subdirs;
            try
            {
                subdirs = Directory.EnumerateDirectories(root);
            }
            catch (Exception ex) when (ex is UnauthorizedAccessException or IOException)
            {
                continue;
            }

            foreach (var dir in subdirs)
            {
                if (Path.GetFileName(dir).IndexOf("shadps4", StringComparison.OrdinalIgnoreCase) < 0) continue;

                foreach (var exeName in KnownExeNames)
                {
                    var candidate = Path.Combine(dir, exeName);
                    if (File.Exists(candidate)) return candidate;
                }
            }
        }

        return null;
    }

    private static IntPtr FindMainWindow(string exePath)
    {
        var processName = Path.GetFileNameWithoutExtension(exePath);
        foreach (var process in Process.GetProcessesByName(processName))
        {
            if (process.MainWindowHandle != IntPtr.Zero) return process.MainWindowHandle;
        }
        return IntPtr.Zero;
    }

    // --- Win32 interop ---

    private const uint WM_CLOSE = 0x0010;
    private const int SW_RESTORE = 9;

    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    private static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
}
