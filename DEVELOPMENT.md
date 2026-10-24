# Dev loop (PC host)

## Edit → build → run → test

1. Edit `OrbitNX/**/*.cs`.
2. Press `F5` in VS Code — this runs the `build` task
   (`dotnet build ... Release`) then launches `OrbitNX.exe`
   automatically (`preLaunchTask` in `.vscode/launch.json`).
3. On first run (or to switch consoles), enter your Switch's IP on the
   Home screen — this is saved to `config.json` next to the exe.
4. Test against the console: build `Client/` and send it with the VS Code
   "Switch (Debug, logs via nxlink)" launch config (see below for a
   HOME-menu tile instead).
5. Logs: `OrbitNX.log`, written next to the exe
   (`bin/{Debug,Release}/net10.0-windows10.0.18362.0/`).

No separate publish/installer step is part of this loop — `installer.iss`
and `publish/win-x64` are release packaging, not dev iteration.

## Building the installer

VS Code task `build-installer`, or `OrbitNX/scripts/build-installer.ps1`
directly. Runs `dotnet publish` (win-x64, self-contained) then compiles
`installer.iss` via Inno Setup's `ISCC.exe` (auto-detected on PATH or the
default install location). Produces `OrbitNX/Setup/OrbitNX-Setup.exe`,
bundling `THIRD-PARTY-NOTICES.md` alongside the app. Requires
[Inno Setup 6](https://jrsoftware.org/isdl.php) installed.

## Switch HOME-menu tile (forwarder NSP)

`Client/tools/build-forwarder.ps1` builds `Client/OrbitNX-Forwarder.nsp`: an installable title that
puts an OrbitNX tile on the Switch HOME menu and launches `sdmc:/switch/OrbitNX/OrbitNX-Switch.nro`.

1. Build the client (`make` in `Client/`) and copy `OrbitNX-Switch.nro` to the SD card as
   `/switch/OrbitNX/OrbitNX-Switch.nro`.
2. `powershell -ExecutionPolicy Bypass -File Client/tools/build-forwarder.ps1`
   (needs `python -m pip install nton` and your own console's keys at `%USERPROFILE%\.switch\prod.keys`
   - never commit or share them).
3. Install the `.nsp` on the console with DBI (or similar).

The Title ID is pinned (`01E7E36C3EA40000`), so installing a rebuild updates the existing tile. The
tile only launches the `.nro`, so updating the app later is just copying a new `.nro` - no reinstall.
Launched this way the app runs as a full application (not the album applet), so it gets the full memory
budget; nxlink logging is only available when sent from the PC with the debug launch config.

## Before touching interop-heavy code

Read the `orbitnx-pc-architect` skill first, especially
`references/switch-protocol.md` (the wire formats are mirrored by hand in
`Client/` — change both sides together), `references/steam-bigpicture.md`, and
`references/ffmpeg-pipeline.md` — each documents already-tried-and-rejected
approaches so you don't repeat a dead end.
