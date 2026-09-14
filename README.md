# Mytooth

A Bluetooth manager written in C for an LMDE-based distribution running
**Cinnamon on Wayland**. GTK 3 provides the widgets; Mint's libxapp provides the
tray integration. BlueZ remains responsible for Bluetooth protocols and pairing
records. There is no X11 backend in Mytooth.

This is the initial implementation of roadmap steps 1–5, **not a hardware-certified
release**. See [validation](docs/validation.md) for the automated checks and the
remaining target-system tests. Native Cinnamon tray menu behavior depends on the
versions of XApp and Muffin shipped by the distribution.

## Implemented

- One application instance, window, launcher entry and persistent XApp tray icon.
- Live adapter/device discovery through BlueZ D-Bus, power control, adapter
  selection and naming, visibility with a two-minute daemon-side timeout.
- Classic/BLE discovery through BlueZ's default automatic transport; each scan
  releases Mytooth's own discovery session and stops after 30 seconds.
- Pair, cancel, connect, disconnect, trust/untrust, block/unblock, rename and forget.
- Complete Agent1 interaction methods: PIN/passkey entry and display, numeric
  confirmation, authorization, cancellation and release.
- Saved/nearby device views, search, device details, reported battery and service UUIDs.
- Notifications, optional login startup, close-to-tray, and explicit quit.
- BlueZ restart/hotplug handling, cancellation during session lock/suspend,
  software/hardware rfkill reporting when `/dev/rfkill` is readable.
- Unprivileged logging to the journal or terminal; debug mode is opt-in.

Pairing and connecting are separate actions. Pairing does **not** automatically
trust a device. Reconnection policy remains with BlueZ and the installed audio/input
stack; Mytooth does not run a reconnect loop.

File transfers, audio-profile selection, Bluetooth PAN networking and Debian
packaging belong to later roadmap steps. Audio devices can already be paired and
connected; playback depends on your existing PipeWire/WirePlumber setup.

## Build

On the target LMDE/Debian installation:

```sh
sudo apt install build-essential meson ninja-build pkg-config libgtk-3-dev libxapp-dev libglib2.0-dev
meson setup build --werror
meson compile -C build
meson test -C build --print-errorlogs
./build/mytooth
```

Minimum compile-time APIs: C17, GLib/GIO 2.66, GTK 3.24, libxapp 2.0, Meson 0.61.
These are API minimums, **not a guarantee that an old XApp release supports native
Wayland tray menus**. The target distro must supply a compatible XApp status applet,
libxapp Wayland menu implementation, and Muffin compositor. Record the tested package
versions in the target validation checklist.

BlueZ must be installed and its service running. Cinnamon's screensaver service
must report an unlocked session for new pairing prompts. Standard Bluetooth
devices still require appropriate kernel drivers, firmware and audio/input support.

Install the executable, icon, launcher and manual:

```sh
sudo meson install -C build
```

This does not enable startup, remove Blueman, change system policy or modify
Bluetooth configuration. Stop the competing Blueman applet before testing Mytooth
as the default pairing agent. Distribution replacement and packaging are a later step.

## Running

```sh
mytooth                # open the existing window or start Mytooth
mytooth --background   # tray startup; reopens the window if no native host appears
mytooth --debug        # enable diagnostics for this process
mytooth --quit         # end Mytooth without disconnecting devices
mytooth --version
```

Use the window menu for notifications and **Start at login**. Startup is disabled
until explicitly enabled. Closing the main window keeps the agent and tray alive
when a native XApp host is present. Without a host, closing the window exits.
If a previously present host disappears, Mytooth reopens its window.

## Logs and preferences

Mytooth uses normal user-session logging:

```sh
journalctl -t mytooth -b
journalctl -t mytooth -b -f
```

When launched with a terminal on stderr, logs go to that terminal. Otherwise
they go to journald, falling back to stderr if journald is unavailable. Journald
chooses persistence, rotation and retention (`/var/log/journal` or `/run/log/journal`
according to system policy). Mytooth never creates its own `/var/log` file and never
runs as root. Debug logging records operation names and error identifiers, not
PINs, passkeys, device addresses or device names. Dependency diagnostics may have
their own content; review logs before sharing them.

Preferences: `$XDG_CONFIG_HOME/mytooth/settings.ini`, normally
`~/.config/mytooth/settings.ini`. Login entry:
`~/.config/autostart/io.github.the_specter_x.Mytooth.desktop`. Pairing records and
device properties are owned by BlueZ; Mytooth does not duplicate them.

## Development

See [architecture](docs/architecture.md), [validation](docs/validation.md), and
[the roadmap](docs/roadmap.md). Dedicated accessibility features and testing are
deferred; standard GTK behavior and required dependencies are retained.

```sh
meson setup build-sanitize -Db_sanitize=address,undefined -Db_lundef=false --werror
meson compile -C build-sanitize
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 meson test -C build-sanitize --print-errorlogs
```

License: GPL-3.0-or-later. See [COPYING](COPYING).
