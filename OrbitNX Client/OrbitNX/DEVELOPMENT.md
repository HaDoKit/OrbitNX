# Dev loop (Switch client)

One-time setup: run `setup.bat` (installs the devkitPro pacman packages this
project needs). Requires `DEVKITPRO` already set in your environment.

`scripts/build.ps1` works around two devkitPro-on-Windows gotchas automatically -
no action needed, just documenting why the script isn't a plain `make` call:
a backslash-style `DEVKITPRO` (`C:\devkitPro`) gets mangled to `C:devkitPro` when
the Makefile shells out to tools like `sdl2-config`, and devkitPro's recursive
`make -C build -f $(CURDIR)/Makefile` breaks if any part of the path contains a
space (true even via devkitPro's own bundled msys2, not just Git Bash) - the
script normalizes slashes and, if needed, builds through a temporary space-free
`subst` drive.

## Edit → build → deploy → test

1. Edit `source/*.c`.
2. Build: `Ctrl+Shift+B` in VS Code (task `build`), or `scripts/build.ps1`
   directly. Produces `OrbitNX.nro` in this folder.
3. Deploy: task `deploy` (or `build+deploy` to do both in one step), or
   `scripts/deploy.ps1` directly. Sends the `.nro` to your console via
   `nxlink`.
   - Target IP comes from `switch-ip.txt` (first line, gitignored — this is
     per-machine, not committed). Override per-run with
     `$env:ORBITNX_SWITCH_IP`.
   - **The PC-side `OrbitNX` app's Settings must point at this same IP**
     for the two sides to actually connect during a test.
4. Run the forwarder/app on the console, then start `OrbitNX` on the PC
   and confirm the session connects.
5. Logs: this app writes to `sdmc:/switch/OrbitNX/orbitnx.log` on the SD card —
   pull it manually (SD card reader) if you need to inspect it; there's no
   network log-pull path in this project.

`build.bat`/`SendToSwitch.bat` still work for manual double-click use (they
have their own retry-loop UX) — the VS Code tasks above are a
non-interactive equivalent for the same underlying `make`/`nxlink` commands,
nothing more.

## NSP forwarder (optional, home-screen tile)

Not part of the normal dev loop (hbmenu launch of `OrbitNX.nro` needs none of
this) - only relevant if you want a home-screen tile instead of launching via
hbmenu each time. Requires `prod.keys` (your own, dumped from your own console -
never commit or share this file; already gitignored) at `C:\Users\<you>\.switch\prod.keys`
or the current working directory.

```
pip install nton
nton build "<path>\OrbitNX.nro" --sdmc "/switch/OrbitNX/OrbitNX.nro" \
    --name "OrbitNX" --publisher "HaDoK" --version "1.0.0" \
    --icon "<path>\icon.jpg" --id 01E7E36C3EA40000
```

The `--id` is pinned to `01E7E36C3EA40000` (no `0x` prefix - nton rejects that)
so rebuilding after an icon/version change updates the same installed title
instead of installing a duplicate. Install the resulting `.nsp` via DBI (or
similar) on the console - nton/this script only builds the file, it doesn't
install it.

## Before touching the network protocol

Read the `orbitnx-switch-client` skill's `references/network-protocol.md`
first — there's a known, unresolved discrepancy between this repo's `JoyPkg`
struct and what the PC-side code assumes. Don't change packet layout or
ports without cross-checking both repos field by field.
