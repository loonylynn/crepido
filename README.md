# Crepido

Crepido is a little GTK 3 dock for Linux. It's a **passion project made for fun**, not a commercial product or a polished, officially supported desktop component. It's shared as-is; things may be rough around the edges.

It's designed to sit alongside **MATE Panel**, mainly on Debian 13 with MATE/Marco and X11. It isn't a replacement for the panel, and it doesn't support Wayland. Window Maker was an inspiration for some of its behavior, but isn't needed to run it.

## What it can do?

<img width="799" height="599" alt="preview1" src="https://github.com/user-attachments/assets/66a350f8-b85f-4c5f-8644-724667e9108c" />


- Keep application launchers handy.
- Group launchers in expandable Drawers.
- Show and restore minimized windows.
- Place the Dock on a selected monitor and screen edge.
- Customize icon size, opacity, and bitmap backgrounds.

## Instructions

Crepido is designed for Debian 13 with MATE/Marco on X11.

### 1. Install dependencies

```sh
sudo apt update
sudo apt install build-essential meson ninja-build pkg-config \
  libgtk-3-dev libx11-dev libxrandr-dev xvfb xauth
```

### 2. Download and build

```sh
git clone https://github.com/loonylynn/crepido.git
cd crepido
meson setup build
meson compile -C build
```

Skip the first two commands and run the build
commands from your Crepido folder if this isn't your first time cloning the repo. To rebuild later, use `meson compile -C build`.

### 3. Running Crepido

```sh
./build/crepido
```

This runs Crepido for your current session without installing it system-wide.
For installation and optional autostart, see [the install guide](docs/INSTALLING.md).

## DISCLAIMER

- The main target is MATE/Marco on X11. Other setups may or may not work.
- Crepido is experimental hobby software. Back up your configuration and use your own judgment before installing it system-wide.
- Settings are stored in `~/.config/crepido/`.
- To install Crepido beyond the build directory, see [the install notes](docs/INSTALLING.md).

## Feedback

Found a bug or have an idea? Open an issue on GitHub. This is a hobby project, so please don't expect a guaranteed response or support schedule.

## License

Crepido is free software under the GNU GPL, version 2 or (at your option) any later version. See [LICENSE](LICENSE).
