#!/usr/bin/env python3
"""
SPDX-License-Identifier: MIT

Run with paths to the SonicDE and KDE backend executables.
"""

import os
import select
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

from gi.repository import Gio, GLib


class IsolatedBackendTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="xdp-backend-test-")
        self.addCleanup(temporary.cleanup)
        self.environment = os.environ.copy()
        for variable in ("XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_DATA_HOME", "XDG_RUNTIME_DIR"):
            directory = os.path.join(temporary.name, variable)
            os.mkdir(directory, 0o700)
            self.environment[variable] = directory
        self.environment.update(QT_QPA_PLATFORM="xcb", XDG_CURRENT_DESKTOP="SonicDE:KDE", KCRASH_AUTO_RESTART="0")

        # No service directories: do not activate host portal helpers or other daemons.
        config_path = os.path.join(temporary.name, "bus.conf")
        with open(config_path, "w", encoding="utf-8") as config:
            config.write('''<busconfig>
                <type>session</type>
                <listen>unix:tmpdir=/tmp</listen>
                <auth>EXTERNAL</auth>
                <policy context="default">
                    <allow own="*"/>
                    <allow send_destination="*"/>
                    <allow receive_sender="*"/>
                </policy>
            </busconfig>''')
        daemon = subprocess.Popen(
            ["dbus-daemon", "--nofork", "--config-file=" + config_path, "--print-address=1"],
            stdout=subprocess.PIPE,
            text=True,
        )
        self.addCleanup(self.stop_process, daemon)
        self.addCleanup(daemon.stdout.close)
        self.assertTrue(select.select([daemon.stdout], [], [], 10)[0], "D-Bus did not become ready")
        address = daemon.stdout.readline().strip()
        self.assertTrue(address, "D-Bus did not provide an address")
        self.environment["DBUS_SESSION_BUS_ADDRESS"] = address
        self.bus = Gio.DBusConnection.new_for_address_sync(
            address, Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None,
        )
        self.addCleanup(self.bus.close_sync, None)

        display = subprocess.Popen(
            ["Xvfb", "-displayfd", "1", "-screen", "0", "1280x720x24", "-nolisten", "tcp"],
            stdout=subprocess.PIPE,
            text=True,
        )
        self.addCleanup(self.stop_process, display)
        self.addCleanup(display.stdout.close)
        self.assertTrue(select.select([display.stdout], [], [], 10)[0], "Xvfb did not become ready")
        display_number = display.stdout.readline().strip()
        self.assertTrue(display_number.isdigit(), "Xvfb did not provide a display number")
        self.environment["DISPLAY"] = ":" + display_number

    @staticmethod
    def stop_process(process):
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)

    def owner(self, service):
        try:
            reply = self.bus.call_sync(
                "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetNameOwner",
                GLib.Variant("(s)", (service,)), GLib.VariantType.new("(s)"), Gio.DBusCallFlags.NONE, 1000, None,
            )
        except GLib.Error as error:
            if "org.freedesktop.DBus.Error.NameHasNoOwner" in str(error):
                return None
            raise
        return reply.unpack()[0]

    def launch(self, executable, replace=False):
        process = subprocess.Popen([executable] + (["--replace"] if replace else []), env=self.environment)
        self.addCleanup(self.stop_process, process)
        return process

    def wait_for_owner(self, service, process, previous=None):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            self.assertIsNone(process.poll(), f"Backend exited before acquiring {service}")
            owner = self.owner(service)
            if owner is not None and owner != previous:
                return owner
            time.sleep(0.02)
        self.fail(f"Backend did not acquire {service}")

class ServiceReplacementTest(IsolatedBackendTest):
    def test_replacement_exits_only_the_old_owner(self):
        services = ["org.freedesktop.impl.portal.desktop.sonicde", "org.freedesktop.impl.portal.desktop.kde"]
        processes = [self.launch(executable) for executable in EXECUTABLES]
        owners = [self.wait_for_owner(service, process) for service, process in zip(services, processes)]

        for index, (service, executable) in enumerate(zip(services, EXECUTABLES)):
            # Without --replace, a second process must fail without disturbing the owner.
            duplicate = self.launch(executable)
            self.assertEqual(duplicate.wait(timeout=10), 1)
            self.assertEqual(self.owner(service), owners[index])
            self.assertIsNone(processes[index].poll())

            for _ in range(2):
                previous = processes[index]
                replacement = self.launch(executable, replace=True)
                owners[index] = self.wait_for_owner(service, replacement, owners[index])
                self.assertEqual(previous.wait(timeout=10), 0, "Replaced backend did not exit cleanly")
                self.assertIsNone(replacement.poll(), "Replacement backend exited unexpectedly")
                self.assertIsNone(processes[1 - index].poll(), "Backend with another service name was stopped")
                processes[index] = replacement


if __name__ == "__main__":
    if not shutil.which("Xvfb") or not shutil.which("dbus-daemon"):
        print("SKIP: Xvfb and dbus-daemon are required for the isolated backend replacement test")
        sys.exit(77)
    EXECUTABLES = sys.argv[1:3]
    if len(EXECUTABLES) != 2:
        sys.exit("Expected SonicDE and KDE backend executable paths")
    del sys.argv[1:3]
    unittest.main()
