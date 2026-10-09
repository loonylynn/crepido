# Crepido

Crepido is a little GTK 3 dock for Linux. It's a **passion project made for fun**, not a commercial product or a polished, officially supported desktop component. It's shared as-is; things may be rough around the edges.

It's designed to sit alongside **MATE Panel**, mainly on Debian 13 with MATE/Marco and X11. It isn't a replacement for the panel, and it doesn't support Wayland. Window Maker was an inspiration for some of its behavior, but isn't needed to run it.

## What it can do

<img width="800" height="600" alt="example" src="https://github.com/user-attachments/assets/7eedfd50-1d24-4d51-a045-80e25324912a" />

- Keep application launchers handy.
- Group launchers in expandable Drawers.
- Show and restore minimized windows.
- Place the Dock on a selected monitor and screen edge.
- Customize icon size, opacity, and bitmap backgrounds.

## Try it

These quick-start steps are for Debian 13 with MATE/Marco on X11. You'll build Crepido from source yourself.

Install the build tools and libraries:

```sh
sudo apt install build-essential meson ninja-build pkg-config \
  libgtk-3-dev libx11-dev libxrandr-dev xvfb xauth
```

Build it, run the tests, and launch it:

```sh
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
./build/crepido
```

Running it this way starts Crepido for that session; it doesn't install an autostart entry.

## A few things to know

- The main target is MATE/Marco on X11. Other setups may or may not work.
- Crepido is experimental hobby software. Back up your configuration and use your own judgment before installing it system-wide.
- Settings are stored in `~/.config/crepido/`.
- To install Crepido beyond the build directory, see [the install notes](docs/INSTALLING.md).

## Feedback

Found a bug or have an idea? Open an issue on GitHub. This is a hobby project, so please don't expect a guaranteed response or support schedule.

## License

Crepido is free software under the GNU GPL, version 2 or (at your option) any later version. See [LICENSE](LICENSE).
