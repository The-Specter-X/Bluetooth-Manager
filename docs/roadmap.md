# Mytooth roadmap

Steps 1–5 are implemented as the initial development release. Their target-system
acceptance gates remain open until tested on the distro and physical hardware.

1. **Platform proof:** C/GTK window, XApp tray/menu, icon and desktop entry. Native
   Wayland startup checked in CI; Cinnamon/Muffin tray behavior requires target QA.
2. **Foundation:** Meson build, one instance, small C modules, preferences, tests,
   diagnostic logging and CI.
3. **Bluetooth model:** live adapters/devices, power, naming, visibility, scans,
   hotplug and daemon restarts.
4. **Pairing/connections:** Agent1 methods, validation, cancellation, pairing,
   connection actions, trust/block and removal.
5. **Daily use:** device UI, details, search, reported battery, tray quick actions,
   notifications, startup option, lock/suspend handling.
6. **Next:** OBEX file sending/receiving with approval, cancellation and safe paths.
7. **Later:** audio-profile selection and PAN tethering through existing services.
8. **Release integration:** Debian packaging, translations, upgrade handling,
   distro defaults and conflict-free replacement of Blueman startup.

Dedicated accessibility work is deferred by project decision. Keep normal GTK
behavior and required library dependencies. Legacy dial-up, network hosting,
serial-port tools, BLE service development tools and vendor-specific features
remain outside the initial scope.
