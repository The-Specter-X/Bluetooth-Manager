# Mytooth project review — 2026-10-04

Reviewed baseline: `512bcccd1a1b491f1d09430b78a71d3a7eae0efb`.
The review covers every C module, public header, existing tests, build/install
metadata, logging, documentation and upstream feature coverage. The changes in
this branch address the confirmed issues below. This is not a physical-hardware
certification or a claim that all bugs have been eliminated.

## Fit to project goals

The architecture fits the intended C17, GTK3/XApp, Cinnamon/LMDE Wayland target:
one unprivileged process, one event loop, asynchronous documented BlueZ APIs,
standard user-session journal logging, and small modules. GLib/GIO provide actual
benefit for D-Bus and lifecycle work; no extra GNOME desktop services, plugin
framework, local pairing database, custom Bluetooth daemon or privileged helper
are needed. GTK handles theme changes, including light/dark. Its ordinary X11
backend is retained for transition testing without adding X11-specific code.

## Comparison with established managers

This table compares documented feature scope, not performance or maturity scores.
In particular, Blueman has continued releases and development; describing it as
inactive or asserting Mytooth is already better would not be justified.

| Area | Established counterparts | Mytooth decision |
| --- | --- | --- |
| Pairing, connections, power, visibility | Core functionality in Blueman, BlueDevil and GNOME Bluetooth | Keep; covers Agent1 methods, explicit trust/block, cancellation, hotplug and daemon restart |
| Tray and device quick actions | Blueman applet and Plasma's BlueDevil integration | XApp native host; keep the window accessible when the host disappears |
| File sending/receiving | Blueman OBEX; BlueDevil file policy and storage configuration; GNOME file sharing | Explicit approval, peer address, protected Downloads/Bluetooth, progress/cancel and selected-adapter sending |
| Audio | Bluetooth connection plus integration with system audio stack | PulseAudio protocol profiles, compatible with PipeWire-Pulse; actual codecs and policy remain with audio services |
| Bluetooth internet access | Blueman includes network/DUN features | NetworkManager PAN client, with address/routes/DNS owned by NetworkManager |
| Incoming-file policy | BlueDevil also offers trusted-device or unconditional acceptance | Keep explicit approval; do not add silent acceptance merely to match a feature list |
| Legacy/protocol tooling | Blueman has a broader collection of network/service helpers | Do not add DUN, NAP hosting, RFCOMM tools, remote phonebook/message browsing or a plugin framework to the initial scope |
| Shipping maturity | Established projects have translations, packaging and substantial hardware exposure | Debian binary packaging now present; translations and physical Cinnamon/Bluetooth acceptance remain release gates |

Sources (upstream):

- [Blueman feature description](https://github.com/blueman-project/blueman)
- [Blueman releases](https://github.com/blueman-project/blueman/releases)
- [BlueDevil manual](https://docs.kde.org/stable_kf6/en/bluedevil/kcontrol/bluedevil/index.html)
- [GNOME Bluetooth help](https://help.gnome.org/gnome-help/bluetooth.html)
- [BlueZ OBEX API](https://bluez.readthedocs.io/en/latest/obex-api/)
- [BlueZ OBEX agent API](https://bluez.readthedocs.io/en/latest/obex-agent-api/)

## Confirmed issues and changes

| Finding in baseline | Change |
| --- | --- |
| OBEX CreateSession omitted Source, despite selectable adapters | Supply the device's adapter address using the documented Source option |
| File chooser response used the mutable current detail_path | Capture the target; destroy the chooser when its details dialog closes; validate the device and adapter again before sending |
| Cancel on incoming approval reset transfer state but left the D-Bus invocation pending | Clear/reject the invocation and prompt together, allowing the next request |
| Transfer UI could appear before incoming metadata/approval was ready | Distinguish awaiting approval from a display-ready prompt |
| Early Transfer1 completion could be ignored before SendFile returned its object path | One-shot GetAll after creation, guarded by generation, operation and transfer path; no polling loop |
| Registration conflict required a full restart; availability was not visible | Add serialized Retry file receiver and persistent receiver status; never steal another receiver's slot |
| Incoming prompt showed no peer identity | Query the OBEX session and display its Destination address, or Unknown sender on failure |
| OBEX debug log echoed arbitrary daemon error text | Log only the D-Bus error identifier, preserving the redaction policy |
| Screensaver unlock could undo the suspend guard | Keep sleeping as a separate state; delayed GetActive replies cannot undo newer signals |
| Lock/quit could leave Pair pending before any Agent1 prompt appeared | Explicitly cancel pending Pair calls on lock and client stop |
| Pairing registration retry could overlap itself | Serialize registration calls |
| Audio matching removed all non-hex characters and joined unrelated fields | Match complete address tokens in supported card/path formats; malformed or partial addresses do not match |
| rfkill EOF/permanent read error could leave a readable watch spinning | Remove the watch and invalidate the cache on terminal read conditions |
| Tray host disappearance could be reported repeatedly | Clear the remembered-host flag after reporting loss |
| Architecture/manual still said GTK was Wayland-restricted | Correct documentation; keep Wayland and add X11 transition smoke coverage |
| No installable target-test package | Add conventional Debian/debhelper binary packaging and a CI artifact; no applet/policy/autostart changes |
| Unused UI ordering/prompt fields | Remove dead state without changing stable row insertion behavior |

## Verification

Regression tests now cover early completion, a two-file batch, selected-adapter
Source, delayed session cancellation, cancellation during incoming approval,
registration conflict/retry, sender identity, suspend/unlock/query ordering,
shutdown pairing cancellation, exact audio addresses and filename controls/bounds.

The CI workflow is the verification gate: strict Debian build and C tests,
ASan/UBSan with leak detection, Debian binary package build, native Wayland
smoke test with no X server, and a separate visible X11/Xvfb smoke test. Check
the PR's latest CI run rather than inferring a pass from this report. The local
editing runtime blocked installing the development dependencies; no local full
build or physical Bluetooth result is claimed.

## Remaining requirements and deliberate deferrals

- Test real Cinnamon/Muffin/XApp panel menu placement, host restarts and scaling;
  Weston/Xvfb cannot certify this integration.
- Test physical adapters, firmware/rfkill policy, keyboards, mice/controllers,
  audio playback/microphones/profiles, phone PAN, large OBEX files and resume.
- Test package install/upgrade/remove in a clean LMDE VM. The package is an
  unsigned development build, not a Debian archive submission or a release.
- Add translations before broad distribution; accessibility-specific work stays
  deferred as requested, while ordinary GTK behavior remains intact.
- Keep Bluetooth reconnection, codec negotiation and network routing delegated
  to BlueZ, WirePlumber and NetworkManager. Do not introduce a competing policy loop.
- Evaluate persisted receive-folder preferences and file-manager Send To support
  later if target testing shows a real need. These are useful extensions, not
  prerequisites for pairing, file transfer, audio or PAN.
- The receive path is checked in a private owned directory, but `obexd` opens it
  later. This is not an atomic reservation against another process with the same
  user's privileges. Never claim a security boundary against same-UID malicious
  processes; do not auto-open received files.
- The application intentionally handles one transfer/batch and one pairing prompt
  at a time. Parallel transfer queues would add complexity without an established
  target-user requirement.

The result is a focused daily-use Bluetooth manager with stronger correctness and
target-testability, not feature parity with every legacy Blueman extension.
