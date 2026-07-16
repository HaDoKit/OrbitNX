# Steam Big Picture integration

Files: `BigPicture/SteamLauncher.cs`, `Native/DllInjector.cs`,
`Native/GameActiveSignal.cs`, `Native/FocusHook/focushook.cpp`.

No Steamworks SDK is used anywhere. All integration is external: registry reads,
`steam://` URIs, window enumeration, and DLL injection.

## Detection and launch

- Steam install path: registry `HKCU\Software\Valve\Steam\SteamExe`
  (`SteamLauncher.GetSteamExePath`).
- Running check: `Process.GetProcessesByName("steam")`.
- Launch: `Process.Start(steamExe, "-silent")`, followed by a 4s delay for the
  client to reach an "ready for IPC" state.
- Open Big Picture: `Process.Start(new ProcessStartInfo("steam://open/bigpicture"))`.
- Close Big Picture: **`steam://exitbigpicture` is a confirmed silent no-op** on
  the current Steam client (verified directly via Win+R) — do not rely on it.
  Instead, post `WM_CLOSE` to the Big Picture window, matched by class name
  (`SDL_app` or `vguiPopupWindow`), title-independent (title was dropped after a
  window returning to Big Picture was observed to read "Steam" instead of "Big
  Picture").

## Why DLL injection into steam.exe/steamwebhelper.exe

Big Picture's internal "am I focused?" check was confirmed (via testing) to poll
`GetForegroundWindow()` directly — posting `WM_ACTIVATE`-family messages was
tried and had zero measurable effect. Since another window/game can legitimately
hold real OS foreground while streaming, Big Picture must be spoofed into
believing it's still foreground, or gamepad input to it dies.

`DllInjector.cs` does classic `OpenProcess` → `VirtualAllocEx` →
`WriteProcessMemory` → `CreateRemoteThread` → `LoadLibraryW` injection of
`focushook.dll` into **all** `steam.exe`/`steamwebhelper.exe` processes, excluding
sandboxed CEF renderer children (detected via WMI `CommandLine` containing
`--type=`). This broad targeting is deliberate: the specific process that
actually performs the focus check was never reliably identified, so every
candidate process is covered rather than guessing one.

## `GameActiveSignal`

A named `EventWaitHandle("OrbitNX_GameActive")` tells `focushook.dll` to
stop spoofing while a real game is active. Without it, a game and Big Picture
would both react to the same virtual XInput input simultaneously — ViGEmBus's
virtual controllers aren't focus-gated by Windows the way real hardware input
would be filtered.

**If you change this protocol, change both sides together**: the managed .NET
side that raises/clears the signal, and `Native/FocusHook/focushook.cpp` that
polls it. They are two independent binaries with no shared build — a one-sided
change compiles fine and fails silently at runtime.

## Other documented Steam-specific quirks

- An earlier hidden-virtual-monitor architecture was abandoned entirely due to
  DPI-virtualization mismatches and Windows silently reassigning the primary
  monitor.
- Games launched from Big Picture open a **second** top-level window (e.g. class
  `HL_WIN`) that also needs injection — handled via a largest-visible-window
  heuristic, restricted to an **allow-list** (steam.exe/steamwebhelper.exe/exes
  under the Steam install directory). A deny-list approach was tried and rejected
  after it grabbed the user's own VS Code window.
- The `vguiPopupWindow` class was added after a settings dialog opened from Big
  Picture became completely uncontrollable by gamepad.
- `ForceForegroundWindow` (`SteamLauncher.cs`) does a one-shot
  `AttachThreadInput`+`SetForegroundWindow` even with the hook active, because
  Steam Input's own per-context binding still keys off *real* OS foreground in
  some paths.
- `FullscreenOptimizationFix`/`WDA_EXCLUDEFROMCAPTURE` checks exist for DWM
  capture-blocking behavior (observed with the game Balatro) — this is a known
  partial fix, not a complete one.
