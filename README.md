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

## Build and run

These instructions are for **Debian 13 with MATE/Marco on X11**. Crepido does not support Wayland. You'll build it from source; these steps do not install it system-wide or add it to desktop startup.

### 1. Install the build dependencies

Open a terminal and run:

```sh
sudo apt update
sudo apt install build-essential meson ninja-build pkg-config \
  libgtk-3-dev libx11-dev libxrandr-dev xvfb xauth
```

### 2. Download the source code

Run this step **once**, the first time you set up Crepido:

```sh
git clone https://github.com/loonylynn/crepido.git
cd crepido
```

If you've already cloned the repository, don't clone it again. Just go to your existing checkout, for example:

```sh
cd ~/crepido
```

### 3. Compile Crepido

Make sure you're in the repository's top-level folder — the one that contains `meson.build` — before running these commands:

```sh
meson setup build
meson compile -C build
```

`meson setup build` configures the build directory and is normally needed only the first time. After changing or pulling source code later, rebuild with:

```sh
meson compile -C build
```

If Meson reports that the build directory belongs to an old source path (for example, after moving or renaming the project folder), recreate the generated build directory from the repository root:

```sh
meson setup --wipe build
meson compile -C build
```

This resets generated build files, not your source code.

### 4. Run the tests (recommended)

```sh
meson test -C build --print-errorlogs
```

### 5. Launch Crepido

```sh
./build/crepido
```

Crepido will run for your current desktop session. Closing it ends that run; it won't automatically start at login. For installation and optional MATE autostart setup, see [the install notes](docs/INSTALLING.md).

**Troubleshooting:** If Meson says it can't find `meson.build`, you're probably not in the repository folder. Run `cd ~/crepido` (or change to wherever you cloned it), then confirm that `ls meson.build` shows the file before trying again.

## A few things to know

- The main target is MATE/Marco on X11. Other setups may or may not work.
- Crepido is experimental hobby software. Back up your configuration and use your own judgment before installing it system-wide.
- Settings are stored in `~/.config/crepido/`.
- To install Crepido beyond the build directory, see [the install notes](docs/INSTALLING.md).

## Feedback

Found a bug or have an idea? Open an issue on GitHub. This is a hobby project, so please don't expect a guaranteed response or support schedule.

## License

Crepido is free software under the GNU GPL, version 2 or (at your option) any later version. See [LICENSE](LICENSE).
