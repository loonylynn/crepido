# Installing Crepido

These notes are provided as-is for people who want to build and try it themselves. The main target is Debian 13 with MATE/Marco on X11.

## Build and run without installing

Install the build dependencies:

```sh
sudo apt update
sudo apt install build-essential meson ninja-build pkg-config \
  libgtk-3-dev libx11-dev libxrandr-dev xvfb xauth
```

From the repository root:

```sh
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
./build/crepido
```

This runs Crepido from the checkout. It does not add an autostart entry to your MATE session.

## Install for your own desktop

A system-wide Meson installation puts the executable under `/usr/bin`. By default, the install also adds a MATE-only autostart entry under `/etc/xdg/autostart/`, so Crepido can start when you log into MATE.

If you have not configured a `build/` directory yet, use:

```sh
meson setup build --prefix=/usr --sysconfdir=/etc
```

If you already built Crepido using the quick-start instructions, configure that existing build instead:

```sh
meson configure build -Dprefix=/usr -Dsysconfdir=/etc
```

Then build, test, and install:

```sh
meson compile -C build
meson test -C build --print-errorlogs
sudo meson install -C build
```

To install without adding the MATE autostart entry, add `-Dinstall_mate_autostart=false` to the fresh `meson setup` command, or run this for an existing build:

```sh
meson configure build -Dinstall_mate_autostart=false
```

Then compile and install as above.

## Testing

The test suite can be run at any time with:

```sh
meson test -C build --print-errorlogs
```

The X11 integration test uses Xvfb when `xvfb-run` is installed, which helps keep tests away from your active desktop session. Automated tests can't verify every desktop-specific detail, so check the real MATE session, autostart behavior, and monitor placement yourself.

## Notes

- Crepido is intended to complement MATE Panel, not replace it.
- Wayland is not supported.
- Your personal settings are stored under `~/.config/crepido/`.
- You'll need to build and install it yourself; there is no one-click installer or formal support schedule.
