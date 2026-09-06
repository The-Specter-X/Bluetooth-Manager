# Validation

## Automated checks

`meson test -C build --print-errorlogs` runs C tests for:

- PIN/passkey bounds, malformed input and error-message redaction.
- BlueZ enumeration, reported battery, pairing without implicit trust, connecting,
  disconnecting, property updates and removal.
- Service disappearance/restart with an operation pending; stale completions.
- Pair cancellation and rejection of overlapping operations.
- A stop request while discovery is still starting.
- Device removal/reappearance, discoverability timeout ordering and failed connection retry.
- PIN/passkey requests, confirmation, authorization and service authorization.
- Display-passkey progress, cancellation, locking during a prompt and spoofed bus callers.

The fake BlueZ service runs on a private test bus and never uses a real adapter.
CI builds on Debian trixie and runs AddressSanitizer/UndefinedBehaviorSanitizer.
The Wayland job uses Weston without XWayland and exercises the real executable,
desktop identity, device/service changes and single-instance quit. Weston headless
has no input seat or native XApp host, so the harness excludes only GTK 3's
corresponding, exact `gdk_seat_get_keyboard` and fallback-icon scale diagnostics;
every other critical remains a failure. Native XApp host behavior is intentionally
left to the Cinnamon target checklist.
See the GitHub Actions results for the status of each commit.

The editing environment cannot open Unix sockets, so local D-Bus and compositor
execution is unavailable there. This limitation must not be mistaken for passing
integration tests; CI results and target QA are the evidence.

## Required target acceptance before shipping

Record the distro image/date, kernel, BlueZ, GTK, libxapp, Cinnamon and Muffin versions.
Do not infer native tray menu support from libxapp's compile-time minimum alone.

- [ ] Start on the real Cinnamon Wayland session with XWayland unavailable.
- [ ] Correct launcher icon and taskbar identity; no duplicate application instances.
- [ ] Native XApp icon is visible with the window open and after it closes.
- [ ] Left-click opens the manager. Right-click menu is correctly positioned on
      every panel edge and on multiple monitors with different scaling.
- [ ] Panel host removal/restart does not leave an inaccessible application.
- [ ] Notifications work; login startup is opt-in and survives the next login.
- [ ] Built-in and USB adapters: power, naming, temporary visibility and radio blocks.
- [ ] Keyboard PIN display, passkey progress, numeric comparison, incorrect PIN,
      rejected request, timeout and user cancellation.
- [ ] Mouse/controller pairing and reconnection after resume.
- [ ] Headset/speaker pairing, reconnect and playback through PipeWire/WirePlumber;
      headset microphone behavior tested through the existing sound settings.
- [ ] Battery reporting checked on supported devices; missing data stays unknown.
- [ ] Unplug adapter during scanning, pairing and connecting; reconnect it.
- [ ] Restart bluetooth.service during operations; no stale state or duplicate agent.
- [ ] Lock/unlock, suspend/resume and Cinnamon screensaver restart during a prompt.
- [ ] Trust/untrust, block/unblock, rename and forget apply only to the selected device.
- [ ] Unicode/long names remain readable and are never interpreted as markup.
- [ ] Close/quit during a pairing operation releases prompts and scanning without
      powering off the adapter or disconnecting already-connected devices.
- [ ] Log and configuration permissions work as an ordinary user.

## Scope limits to verify during review

This implementation relies on the target's Cinnamon screensaver API; unknown lock
status intentionally prevents pairing. BlueZ handles device persistence, protocol
support and reconnect policy. rfkill unblocking requires the session's existing
device permissions; no privileged helper or new policy is installed. System-bus
connection loss requires reopening Mytooth; bluetooth.service restarts are handled
within the running process.

No physical Bluetooth pairing, Cinnamon tray placement, suspend/resume hardware
behavior or target image acceptance is claimed solely from CI.
