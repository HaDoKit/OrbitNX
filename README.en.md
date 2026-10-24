[Русский](README.md) | **English**

# OrbitNX

Stream games from a Windows PC to a Nintendo Switch over your local network: video and audio go
from the PC to the console, and input from the Joy-Con, Pro Controller and touch screen goes back to
the PC.

The project has two parts:

- **OrbitNX**: the Windows app (`OrbitNX/`). It launches the game, captures video and audio, sends
  them to the Switch, and turns button presses into a virtual gamepad.
- **OrbitNX for Switch**: a homebrew app (`Client/`). It shows the stream and sends input.

![OrbitNX on the Switch](docs/images/client/menu.jpg)

## Features

- **Steam Big Picture**: all of your Steam library, driven by a gamepad.
- **shadPS4**: the PlayStation 4 emulator, with gyro through a virtual DualShock 4.
- **Your own game library**: regular games (`.exe`), Steam games, emulators and custom launch
  scripts, with covers in the Switch menu.
- Up to 4 gamepads; the Switch touch screen works as touch input on the PC.
- Hardware encoding with NVIDIA NVENC and hardware decoding on the Switch.
- Steam button on the Switch: **− + +**. Back to the menu: hold **− for 3 seconds**.

> The PC app's interface is in Russian. The English docs quote its labels in Russian, with a
> translation in parentheses.

## Documentation

- [Features](docs/features.en.md): what the system looks like and everything OrbitNX can do, with screenshots
- [Installation](docs/installation.en.md): what you need on the PC and on the Switch
- [Usage](docs/usage.en.md): connecting, modes, controls, settings
- [Game library](docs/library.en.md): your own games, covers, window capture
- [Troubleshooting](docs/troubleshooting.en.md): black screen, artifacts, no connection, logs
- [How it works](docs/architecture.en.md): components, network ports and protocol

For developers, building from source: [DEVELOPMENT.en.md](DEVELOPMENT.en.md).

## Development

This project was developed together with [Claude Code](https://claude.com/claude-code), Anthropic's
AI coding assistant.

## License

[GPL-3.0](LICENSE). Third-party components and their licenses are listed in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
