#!/usr/bin/env python3
"""Run the real application on headless Wayland and an isolated fake BlueZ bus.

No X server or physical Bluetooth adapter is involved. This checks launch,
app identity, hotplug/restart and single-instance exit.
It does not establish Cinnamon panel menu placement or hardware compatibility.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

import dbus


def wait_for(predicate, description, seconds=15):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.05)
    raise AssertionError(f"Timed out: {description}")


def main():
    binary = str(Path(sys.argv[1]).resolve())
    logs = Path("build/smoke-logs")
    logs.mkdir(parents=True, exist_ok=True)
    processes = []
    files = []
    with tempfile.TemporaryDirectory(prefix="mytooth-smoke-") as temp:
        env = os.environ.copy()
        env.update(XDG_RUNTIME_DIR=temp, XDG_CONFIG_HOME=f"{temp}/config",
                   WAYLAND_DISPLAY="mytooth-test", GDK_BACKEND="wayland",
                   G_DEBUG="fatal-criticals", GSETTINGS_BACKEND="memory",
                   NO_AT_BRIDGE="1",
                   MYTOOTH_TEST_TRACE="1",
                   DBUS_SYSTEM_BUS_ADDRESS=os.environ["DBUS_SESSION_BUS_ADDRESS"])
        env.pop("DISPLAY", None)

        def launch(args, filename, extra=None):
            output = (logs / filename).open("w")
            files.append(output)
            process = subprocess.Popen(args, env=env | (extra or {}),
                                       stdout=output, stderr=subprocess.STDOUT)
            processes.append(process)
            return process

        bus = dbus.SessionBus()
        try:
            weston = launch(["weston", "--backend=headless-backend.so", "--renderer=pixman",
                             "--socket=mytooth-test", "--idle-time=0",
                             "--width=1024", "--height=768"], "weston.log")
            wait_for(lambda: Path(temp, "mytooth-test").exists(), "Wayland socket")
            mock = launch(["/usr/bin/python3", "-m", "dbusmock", "--session", "-t", "bluez5"], "bluez.log")
            wait_for(lambda: bus.name_has_owner("org.bluez"), "mock BlueZ")
            root = bus.get_object("org.bluez", "/")
            api = dbus.Interface(root, "org.bluez.Mock")
            api.AddAdapter("hci0", "Mytooth test adapter")
            api.AddDevice("hci0", "AA:BB:CC:DD:EE:FF", "<b>Headphones</b> 日本語")
            app = launch([binary, "--debug"], "mytooth.log", {"WAYLAND_DEBUG": "client"})
            wait_for(lambda: bus.name_has_owner("io.github.the_specter_x.Mytooth"), "Mytooth instance")
            time.sleep(0.5)
            if "startup: command line received" not in (logs / "mytooth.log").read_text():
                print("Mytooth wait channel:", Path(f"/proc/{app.pid}/wchan").read_text().strip(), flush=True)
            wait_for(lambda: 'set_app_id("io.github.the_specter_x.Mytooth")' in
                     (logs / "mytooth.log").read_text(), "visible native Wayland window")
            assert app.poll() is None, "Mytooth exited during startup"
            subprocess.run([binary], env=env, check=True, timeout=10)
            dbus.Interface(bus.get_object("org.bluez", "/org/bluez/hci0"),
                           "org.bluez.Adapter1").RemoveDevice("/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF")
            api.AddDevice("hci0", "AA:BB:CC:DD:EE:00", "Replacement device")
            mock.terminate()
            mock.wait(timeout=10)
            wait_for(lambda: not bus.name_has_owner("org.bluez"), "BlueZ disappearance")
            launch(["/usr/bin/python3", "-m", "dbusmock", "--session", "-t", "bluez5"], "bluez-restart.log")
            wait_for(lambda: bus.name_has_owner("org.bluez"), "BlueZ restart")
            api = dbus.Interface(bus.get_object("org.bluez", "/"), "org.bluez.Mock")
            api.AddAdapter("hci1", "Replacement adapter")
            time.sleep(0.5)
            assert app.poll() is None, "Mytooth failed during hotplug/restart"
            subprocess.run([binary, "--quit"], env=env, check=True, timeout=10)
            assert app.wait(timeout=10) == 0, "Mytooth did not exit cleanly"
            output = (logs / "mytooth.log").read_text()
            assert "CRITICAL" not in output, output
            assert "Gtk-WARNING" not in output, output
            assert weston.poll() is None
            print("PASS: native Wayland identity, hotplug, service restart, single instance and quit")
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
            for output in files:
                output.close()


if __name__ == "__main__":
    main()
