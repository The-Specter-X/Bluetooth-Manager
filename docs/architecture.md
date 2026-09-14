# Architecture

Mytooth is one C process with one main event loop. There is no plugin framework,
custom daemon, shell-command Bluetooth backend, database or root helper.

| Module | Responsibility |
| --- | --- |
| `main.c` | Application lifecycle, actions, notifications, logging, connecting modules |
| `bluetooth.c` | BlueZ object manager, asynchronous operations and owned discovery |
| `agent.c` | BlueZ Agent1 implementation and pending pairing prompt |
| `obex.c` | BlueZ OBEX client/agent, transfer state and safe receive paths |
| `audio.c` | PulseAudio-compatible Bluetooth card/profile cache and switching |
| `window.c` | Standard GTK widgets, stable device rows, dialogs and error presentation |
| `tray.c` | XApp status icon and quick actions |
| `session.c` | Cinnamon lock state and logind suspend signals |
| `rfkill.c` | Kernel rfkill events; unblocking only when current session has permission |
| `settings.c` | Small atomic preference and autostart files using GLib's key-file API |
| `util.c` | Typed property access, input validation and safe error messages |

## Ownership and asynchronous work

The application owns the window/tray/session and holds references to the Bluetooth
client and pairing agent. These two objects use GObject references because their
asynchronous callbacks must outlive UI actions. Plain C structs are used elsewhere.
Public header comments describe borrowed and owned returns.

GDBusObjectManagerClient is the authoritative device cache. UI rows retain their
position during updates, are keyed by object path, and are destroyed when their
device disappears. Newly created rows initially place connected devices first.
There is no polling timer for device state.

Operations are addressed to BlueZ's unique bus owner, carry a generation number,
and ignore completions from an earlier service generation. One foreground operation
per object prevents repeated clicks from launching overlapping operations.
CancelPairing is allowed while Pair is pending. Calls have finite timeouts; Pair
uses 120 seconds and ordinary operations use 30 seconds.

Scanning records both requested and acquired ownership. If the user stops while
StartDiscovery is pending, a successful start is followed by StopDiscovery. The
30-second scan timer only releases Mytooth's own session. The private system-bus
connection is closed on exit so BlueZ can release remaining client resources.

The pairing agent holds at most one prompt. It validates the caller against the
current BlueZ owner, checks that the device exists and is not blocked, and rejects
requests when the session is locked/unknown. Dialog responses complete the D-Bus
invocation exactly once. Display updates retain the same prompt and deadline.
No pairing secrets are written to the log.

OBEX runs on the session bus used by `obexd`. Mytooth registers one OBEX Agent1,
validates every call against the current unique service owner, and allows one
outgoing batch or incoming transfer at a time. Every incoming file needs explicit
approval. The private receive directory is owned by the user and mode `0700`;
remote names are reduced to a safe basename and never overwrite an existing path.
Transfer progress comes from Transfer1 property changes, with Cancel and
RemoveSession used for cleanup.

Audio profiles come from the default PulseAudio protocol server. On the target this
is normally PipeWire-Pulse, so Mytooth does not bypass WirePlumber policy or invoke
`pactl`. Mytooth detects NAP support and link state through BlueZ Network1, while
NetworkManager activates a volatile `panu` connection and owns DHCP, routes and DNS.
The temporary profile disappears after disconnection. Mytooth does not host a NAP,
create a bridge, change firewall rules or install a root helper.

## Deliberate dependencies

GTK requires GLib/GObject/GIO; their async D-Bus APIs avoid implementing message
marshalling, object lifetime and bus tracking ourselves. GtkApplication supplies
single-instance behavior and desktop identity. GNotification uses the desktop
notification service. GKeyFile handles the few preferences without a settings schema.
XApp supplies the Mint tray integration. libpulse supplies a small stable async
client API that works with PulseAudio and PipeWire-Pulse. No GNOME Shell, libadwaita, libhandy,
GNOME Bluetooth or GNOME Settings Daemon dependency is introduced. NetworkManager
is used over its standard D-Bus API without adding a second client-side object model.

XApp is Mint's tray protocol; it is not a Wayland core protocol. BlueZ D-Bus,
freedesktop desktop entries/notifications and kernel rfkill are used as documented.
GDK is restricted to its Wayland backend; Mytooth does not contain an X11 fallback.

## Upstream references

- [BlueZ Adapter API](https://bluez.readthedocs.io/en/latest/adapter-api/)
- [BlueZ Device API](https://bluez.readthedocs.io/en/latest/device-api/)
- [BlueZ Agent API](https://bluez.readthedocs.io/en/latest/agent-api/)
- [BlueZ OBEX API](https://bluez.readthedocs.io/en/latest/obex-api/)
- [BlueZ OBEX Agent API](https://bluez.readthedocs.io/en/latest/obex-agent-api/)
- [BlueZ Network API](https://bluez.readthedocs.io/en/latest/network-api/)
- [NetworkManager D-Bus API](https://networkmanager.dev/docs/api/latest/gdbus-org.freedesktop.NetworkManager.html)
- [NetworkManager Bluetooth settings](https://networkmanager.dev/docs/api/latest/settings-bluetooth.html)
- [PulseAudio introspection API](https://freedesktop.org/software/pulseaudio/doxygen/introspect_8h.html)
- [GDBusObjectManagerClient](https://docs.gtk.org/gio/class.DBusObjectManagerClient.html)
- [XApp status icon source](https://github.com/linuxmint/xapp/blob/master/libxapp/xapp-status-icon.c)
- [Cinnamon screensaver interface](https://github.com/linuxmint/cinnamon-screensaver/blob/master/libcscreensaver/org.cinnamon.ScreenSaver.xml)
- [Kernel rfkill API](https://docs.kernel.org/driver-api/rfkill.html)
- [GLib journal writer](https://docs.gtk.org/glib/func.log_writer_journald.html)
