[Русский](installation.md) | **English**

# Installation

## Requirements

### PC

- Windows 10 version 1903 (build 18362) or later, 64-bit.
- An NVIDIA graphics card with driver 610 or later, for hardware encoding (NVENC). Without one,
  OrbitNX encodes video on the CPU (libx264). That works, but puts a noticeable load on the CPU.
- The **ViGEmBus** virtual gamepad driver. The OrbitNX installer offers to install it for you.
- For Steam mode, Steam installed. For shadPS4 mode, [shadPS4](https://shadps4.net).

### Nintendo Switch

- A console with custom firmware (Atmosphère) and the Homebrew Menu.
- A connection to the same local network as the PC. 5 GHz Wi-Fi, or a PC wired to the router, works
  best: the picture quality depends directly on the network.

### Network

The PC and the Switch must be able to reach each other on the same network. The PC connects to the
Switch by its IP address, so you don't need to open any incoming ports on the PC. These ports are
used (details in [How it works](architecture.en.md)):

| Port | Protocol | Purpose |
|---|---|---|
| 2222 | UDP | video (PC → Switch) |
| 2223 | TCP | input (Switch → PC) |
| 2224 | UDP | audio (PC → Switch) |
| 2225 | TCP | game library (PC → Switch) |

## Installing on the PC

1. Download `OrbitNX-Setup.exe` from the [releases](https://github.com/HaDoKit/OrbitNX/releases) and run it.
2. At the drivers step, select **ViGEmBus** if it isn't installed yet.
3. After installation, start OrbitNX. Closing the window minimizes it to the tray; to quit completely,
   use the tray icon menu → «Выход» (Exit).

You can reinstall the driver later: «Настройки» (Settings) → «Драйверы» (Drivers) →
«Переустановить драйвер геймпада» (Reinstall gamepad driver).

## Installing on the Switch

1. Download `OrbitNX-Switch.nro` from the [releases](https://github.com/HaDoKit/OrbitNX/releases).
2. Copy it to the Switch SD card into `/switch/OrbitNX/`, so the path is
   `/switch/OrbitNX/OrbitNX-Switch.nro`.
3. Start OrbitNX from the Homebrew Menu.

### HOME menu tile (optional)

To start OrbitNX from the HOME menu instead of the Homebrew Menu, you can install a forwarder: a small
title that launches the `.nro` from the SD card. You build it on the PC for your own console, using
keys dumped from that console. Instructions are in
[DEVELOPMENT.en.md → HOME menu tile](../DEVELOPMENT.en.md#home-menu-tile-forwarder). Install the
resulting `OrbitNX-Forwarder.nsp` with DBI.

> Install `OrbitNX-Forwarder.nsp` specifically. The installer rejects other files with
> "Content meta not found".

The forwarder only launches the `.nro`, so to update OrbitNX you just replace the `.nro` on the SD
card. There's no need to reinstall the tile.
