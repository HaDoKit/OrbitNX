# Dev loop (PC host)

Licensed under GPLv3 — see [LICENSE](LICENSE).

## Edit → build → run → test

1. Edit `OrbitNX/**/*.cs`.
2. Press `F5` in VS Code — this runs the `build` task
   (`dotnet build ... Release`) then launches `OrbitNX.exe`
   automatically (`preLaunchTask` in `.vscode/launch.json`).
3. On first run (or to switch consoles), enter your Switch's IP in
   Settings — this is saved to `config.json` next to the exe. **It must
   match the IP in the `SkyNX` repo's `switch-ip.txt`** for a test session
   to actually connect; the two repos don't share this setting
   automatically.
4. Test against the console (see `SkyNX/OrbitNX/DEVELOPMENT.md` for the
   Switch-side build/deploy loop).
5. Logs: `OrbitNX.log`, written next to the exe
   (`bin/{Debug,Release}/net10.0-windows8.0/`).

No separate publish/installer step is part of this loop — `installer.iss`
and `publish/win-x64` are release packaging, not dev iteration.

## Building the installer

VS Code task `build-installer`, or `OrbitNX/scripts/build-installer.ps1`
directly. Runs `dotnet publish` (win-x64, self-contained) then compiles
`installer.iss` via Inno Setup's `ISCC.exe` (auto-detected on PATH or the
default install location). Produces `OrbitNX/Setup/OrbitNX-Setup.exe`,
bundling `LICENSE` and `THIRD-PARTY-NOTICES.md` alongside the app. Requires
[Inno Setup 6](https://jrsoftware.org/isdl.php) installed.

## Before touching interop-heavy code

Read the `orbitnx-pc-architect` skill first, especially
`references/switch-protocol.md` (known cross-repo packet-format
discrepancy — don't change ports or packet layout without cross-checking
the `SkyNX` repo), `references/steam-bigpicture.md`, and
`references/ffmpeg-pipeline.md` — each documents already-tried-and-rejected
approaches so you don't repeat a dead end.
