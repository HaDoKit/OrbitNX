---
name: orbitnx-pc-architect
description: This skill should be used when writing, reviewing, or reasoning about code in OrbitNX/**/*.cs — the .NET 8 WinForms PC host app in this repo. Trigger on requests like "review this C# code in OrbitNX", "add a service to AppOrchestrator", "why does the video stream use UDP", "how does Steam Big Picture focus spoofing work", "add a new virtual gamepad", "change the ffmpeg encoder args", "add a DllImport", or any work touching Capture/, Audio/, Input/, Session/, BigPicture/, Native/, or Config.cs. This is NOT a generic C# guide and NOT a web-framework skill — it documents this specific desktop/native-interop/streaming app.
---

# OrbitNX (PC host) — architecture and interop guide

## What this app actually is

OrbitNX is the **PC-side host** for remote play with a homebrew client called
`skyNX` that runs directly **on a Nintendo Switch** (own repo, see
`references/switch-protocol.md` for the cross-repo link). It is not a capture-card
app and does not talk to a stock/unmodified Switch — the "console" side is a
separate homebrew C application. Model: same shape as Steam Link/Moonlight/Sunshine,
purpose-built for this one client.

Three independent, unsynchronized network streams to the Switch's IP:

| Stream | Transport | Port | Direction | Payload |
|---|---|---|---|---|
| Video | UDP (MPEG-TS) | 2222 | PC → Switch | H.264, encoded by `ffmpeg.exe` |
| Audio | UDP (RTP) | 2224 | PC → Switch | Opus via `Concentus`, hand-rolled RTP |
| Input (HID) | TCP | 2223 | PC connects out to Switch (Switch listens) | Binary controller/touch/gyro packet |

Separately and unrelated to the Switch link: this app also drives **Steam Big
Picture mode** on the same PC via registry lookup, `steam://` URIs, window
enumeration, and DLL injection — see `references/steam-bigpicture.md`.

## Module map

| Folder | Responsibility |
|---|---|
| `Capture/` | `VideoEncoder.cs` — owns the ffmpeg.exe subprocess (capture+encode+send) |
| `Audio/` | WASAPI loopback capture → Opus encode → RTP → UDP send |
| `Input/` | `HidProtocol.cs` parses the incoming Switch HID packet; `VirtualGamepad.cs`/`VirtualGamepadHub` replays it as ViGEm virtual Xbox 360 pads |
| `Session/` | `SessionController.cs` — dials out to the Switch's HID port, gates session start on a valid HID payload |
| `BigPicture/` | `SteamLauncher.cs` — Steam detection/launch/Big Picture window control |
| `Native/` | `DllInjector.cs`, `GameActiveSignal.cs`, plus `Native/FocusHook/` (injected C++ DLL) and `Native/TouchHelper/` (child process) |
| `Config.cs` | All ports, resolution, fps, bitrate, sample rate constants |
| `AppOrchestrator.cs` | Composition root — wires all services together |
| `Program.cs` | Thin bootstrap: single-instance mutex, tray icon, `Application.Run` |
| `MainForm.cs` / `SettingsForm.cs` | UI only — hold a reference to `AppOrchestrator`, no business logic |

**New functionality goes in a service registered by `AppOrchestrator`, never
directly in a Form.** This is the one hard architectural rule in this codebase.

## Project-wide conventions

- `sealed class ... : IDisposable` for owned-resource services. `Dispose()` does
  best-effort teardown: swallow exceptions with a comment explaining why, never
  rethrow from `Dispose`. Example: `Input/VirtualGamepadHub.cs`.
- Logging only via the static `Logger.Log(category, message)` — never
  `Console.WriteLine`/`Debug.WriteLine`. Appends to `OrbitNX.log`, truncated
  once per process start, all I/O wrapped in a swallowing `try/catch` (logging is
  best-effort by design, must never throw).
- Fire-and-forget is done via `_ = Task.Delay(...).ContinueWith(...)`, not
  `async void`. This is a deliberate, established pattern in this codebase — don't
  reflexively rewrite it to `async`/`await` without understanding why (little
  `async`/`await` is used here at all).
- Raw `IntPtr` handles, closed manually in `finally`. No `SafeHandle` wrappers
  exist in this project — don't introduce one in isolation without discussing it,
  it would be an inconsistent pattern next to everything else.
- XML `<summary>` comments are long-form "engineering diary" entries: what was
  tried, what failed, why the current approach won. **Preserve this style** —
  don't strip these comments down to a one-line "what it does" when touching
  nearby code; the *why* is the valuable, non-obvious part.
- Mixed-language strings: Russian for user-facing UI text (tray menu items),
  English for code/comments/logs. Keep this split.

## Three hot areas — read the reference before touching these

Each of these areas has a documented history of rejected approaches. Before
changing anything here, read the linked reference file — it records what was
already tried and why it failed, so you don't re-run a dead end.

- **Switch network protocol** → `references/switch-protocol.md`. Also flags a
  **known unresolved discrepancy** between this repo's inferred packet format and
  the actual C struct in the `SkyNX` client repo — read it before touching
  `HidProtocol.cs` or `Config.cs` ports.
- **Steam Big Picture integration** → `references/steam-bigpicture.md`. DLL
  injection into `steam.exe`/`steamwebhelper.exe` is a deliberate, tested design
  choice, not a shortcut — alternatives were tried and confirmed not to work.
- **ffmpeg video pipeline** → `references/ffmpeg-pipeline.md`. Capture method,
  transport, encoder rate-control mode, and watchdog timing have each already
  gone through multiple failed iterations documented in code comments.

Also see `references/native-interop.md` (CsWin32 vs manual P/Invoke, unsafe code,
DLL injection as a pattern) and `references/architecture-conventions.md` (more
detail on the composition root and service conventions above).

## Project-specific review checklist

**Switch protocol**
- Any change to the HID packet layout, or to ports 2222/2223/2224, must be
  cross-checked against `SkyNX/OrbitNX/source/network.h` in the client repo — these
  two repos have no build/CI dependency, so a mismatch will not be caught by the
  compiler. See the discrepancy already on record in `switch-protocol.md`.
- Don't add PC-side audio/video sync logic — the client already handles this
  independently; adding it here would double-handle synchronization.
- Respect `SwapAbxy` when touching button mapping — the Switch's button layout is
  diagonally mirrored relative to Xbox.

**Steam Big Picture**
- Don't replace DLL injection with plain `WM_ACTIVATE`-family window messages —
  confirmed to have zero effect on Big Picture's internal foreground check.
- Don't narrow the injection target to a single process name — which process
  actually performs the focus check isn't reliably identified; keep injecting into
  all of `steam.exe`/`steamwebhelper.exe` except sandboxed CEF `--type=` children.
- Don't switch the game-window detection to a deny-list — a deny-list already
  grabbed the user's own VS Code window; keep the allow-list (class name + path
  under the Steam install dir).
- Any change to the `GameActiveSignal` protocol (named `EventWaitHandle`) must be
  applied to both the managed side and `Native/FocusHook/focushook.cpp`
  simultaneously — they're two independent binaries with no shared build.

**ffmpeg pipeline**
- Don't change `-rc`/`-preset`/bitrate flags without testing on real NVENC —
  known bug: a quality-target rate mode fighting hard bitrate bounds caused the
  encoder to fall behind real time.
- Don't raise `-g` (GOP) without accounting for UDP packet loss — current value
  is a deliberate hedge, smaller than the original ~2s GOP.
- Don't lower watchdog timeouts casually — a prior version's aggressive restart
  threshold caused a self-induced restart storm that was misdiagnosed as "video
  became very slow."
- If touching the capture method or transport, re-read the rejected-approach
  history first (DXGI+stdin → gdigrab → gfxcapture; TCP → UDP → brief TCP
  regression → UDP) — see `ffmpeg-pipeline.md`.
- Known stale comment: `AppOrchestrator.cs` still references `gdigrab` in
  comments even though `VideoEncoder.cs` moved to `gfxcapture` — don't trust that
  comment as current behavior; fix it if you're already editing nearby code.

**Native interop / general**
- New `[DllImport]` that duplicates an already-generated CsWin32 method (check
  `NativeMethods.txt` first).
- `IntPtr`/handle leaked without a `finally` block.
- `Dispose()` that throws, or isn't safe to call twice.
- Business logic added to `MainForm`/`SettingsForm` instead of a service wired
  through `AppOrchestrator`.

## Reference files

- **`references/switch-protocol.md`** — HID packet format, port roles, known
  cross-repo discrepancy, ViGEm mapping
- **`references/steam-bigpicture.md`** — Steam detection/launch, DLL injection,
  FocusHook, GameActiveSignal, known Steam-specific quirks
- **`references/ffmpeg-pipeline.md`** — exact ffmpeg invocation, capture/transport
  history, NVENC probing, watchdog behavior
- **`references/native-interop.md`** — CsWin32 vs manual P/Invoke, unsafe code,
  handle lifetime rules
- **`references/architecture-conventions.md`** — composition root, Dispose/Logger/
  fire-and-forget patterns in depth
