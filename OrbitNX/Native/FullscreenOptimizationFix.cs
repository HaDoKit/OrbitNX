using Microsoft.Win32;

namespace OrbitNX.Native;

/// <summary>
/// Disables Windows' "Fullscreen Optimizations" compatibility setting for a specific game exe by
/// writing the same per-user AppCompatFlags registry entry the "Disable fullscreen optimizations"
/// checkbox (exe Properties -&gt; Compatibility tab) writes - so the user never has to find and check
/// that box themselves for every game.
///
/// Why this matters here: even a borderless window exactly matching the monitor's resolution can
/// get silently promoted by DWM into an "independent flip" presentation mode - visually identical to
/// a normal borderless window, but invisible to WGC-based capture (gfxcapture, see VideoEncoder) the
/// same way true exclusive fullscreen is. Confirmed live (Balatro): switching the game's own in-game
/// display mode to Borderless did NOT fix a black/frozen capture - this Windows-level setting is the
/// actual, documented fix for this whole class of "borderless game invisible to DWM-based capture"
/// problem (the same one OBS users hit). HKCU (not HKLM) - no admin elevation needed, matches what
/// the Properties dialog itself does; this is a per-user setting.
///
/// Only takes effect on that exe's *next* launch (compatibility flags are read at process creation,
/// not something that can be applied retroactively to an already-running process) - so a game caught
/// having this problem needs one more relaunch after this fix is applied before it captures cleanly.
/// </summary>
internal static class FullscreenOptimizationFix
{
    private const string LayersKeyPath = @"Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers";

    // The exact internal flag name Windows' own compatibility layer looks for - not a name we
    // chose, has to match what the Properties dialog itself writes.
    private const string DisableFullscreenOptimizationsFlag = "~ DISABLEDXMAXIMIZEDWINDOWEDMODE";

    // Avoids a registry round-trip on every watcher tick (this gets called once/second for
    // whichever game is currently active) - once confirmed set in this process's lifetime, skip
    // re-checking. Safe even across app restarts since the registry write itself is idempotent.
    private static readonly HashSet<string> AlreadyEnsuredThisRun = new(StringComparer.OrdinalIgnoreCase);

    public static void EnsureDisabled(string exePath)
    {
        if (!AlreadyEnsuredThisRun.Add(exePath)) return;

        try
        {
            using var key = Registry.CurrentUser.CreateSubKey(LayersKeyPath, writable: true);
            var existing = key.GetValue(exePath) as string ?? "";
            var flags = existing.Split(' ', StringSplitOptions.RemoveEmptyEntries);
            if (Array.IndexOf(flags, DisableFullscreenOptimizationsFlag) >= 0)
            {
                Logger.Log("FullscreenOptimizationFix", $"already disabled for {exePath}");
                return;
            }

            var updated = string.IsNullOrEmpty(existing)
                ? DisableFullscreenOptimizationsFlag
                : existing + " " + DisableFullscreenOptimizationsFlag;
            key.SetValue(exePath, updated, RegistryValueKind.String);
            Logger.Log("FullscreenOptimizationFix", $"disabled fullscreen optimizations for {exePath} (takes effect on its next launch)");
        }
        catch (Exception ex)
        {
            Logger.Log("FullscreenOptimizationFix", $"failed to set compatibility flag for {exePath}: {ex}");
        }
    }
}
