#!/usr/bin/env python3
"""SPDX-License-Identifier: MIT"""

import os
import shutil
import sys
import time
import unittest

from gi.repository import Gio, GLib

from servicereplacementtest import IsolatedBackendTest


INTERFACE = "org.freedesktop.impl.portal.Settings"
NESTED_GROUP = "org.kde.kdeglobals.PortalSettingsTest"


class SettingsPortalTest(IsolatedBackendTest):
    def setUp(self):
        super().setUp()
        # Seed a real nested configuration group without using the user's files.
        self.environment["XDG_CONFIG_DIRS"] = self.environment["XDG_CONFIG_HOME"]
        with open(os.path.join(self.environment["XDG_CONFIG_HOME"], "kdeglobals"), "w", encoding="utf-8") as config:
            config.write("[PortalSettingsTest]\nGreeting=nested-read-ok\n")

    def call(self, service, method, parameters, signature):
        return self.bus.call_sync(
            service, "/org/freedesktop/portal/desktop", INTERFACE, method,
            parameters, GLib.VariantType.new(signature), Gio.DBusCallFlags.NONE, 3000, None,
        )

    def read_all(self, service, patterns):
        return self.call(service, "ReadAll", GLib.Variant("(as)", (patterns,)), "(a{sa{sv}})").unpack()[0]

    def read(self, service, group, key):
        reply = self.call(service, "Read", GLib.Variant("(ss)", (group, key)), "(v)")
        return reply.get_child_value(0).get_variant().unpack()

    def start_portal(self, executable, service):
        process = self.launch(executable)
        self.wait_for_owner(service, process)
        # The name is acquired before the object is exported; wait for the interface too.
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            self.assertIsNone(process.poll(), "Backend exited during startup")
            try:
                self.read_all(service, ["org.freedesktop.appearance"])
                return
            except GLib.Error as error:
                if not any(name in str(error) for name in ("UnknownObject", "UnknownInterface", "UnknownMethod")):
                    raise
            time.sleep(0.02)
        self.fail("Backend did not export the settings interface")

    def assert_unknown_property(self, service, group, key):
        with self.assertRaises(GLib.Error) as caught:
            self.read(service, group, key)
        self.assertIn("org.freedesktop.DBus.Error.UnknownProperty", str(caught.exception))

    def test_namespace_matching_and_nested_reads(self):
        services = ("org.freedesktop.impl.portal.desktop.sonicde", "org.freedesktop.impl.portal.desktop.kde")
        for executable, service in zip(EXECUTABLES, services):
            with self.subTest(service=service):
                self.start_portal(executable, service)
                all_settings = self.read_all(service, [])
                self.assertIn("org.freedesktop.appearance", all_settings)
                self.assertIn("org.kde.VirtualKeyboard", all_settings)
                self.assertIn("org.kde.TabletMode", all_settings)
                self.assertEqual(all_settings[NESTED_GROUP]["Greeting"], "nested-read-ok")
                self.assertEqual(all_settings, self.read_all(service, ["*"]))
                self.assertEqual(all_settings, self.read_all(service, [""]))

                appearance = self.read_all(service, ["org.freedesktop.appearance"])
                self.assertEqual(set(appearance), {"org.freedesktop.appearance"})
                self.assertEqual(self.read(service, "org.freedesktop.appearance", "color-scheme"),
                                 appearance["org.freedesktop.appearance"]["color-scheme"])
                self.assertEqual(self.read_all(service, [NESTED_GROUP]), {NESTED_GROUP: {"Greeting": "nested-read-ok"}})
                kde_settings = self.read_all(service, ["org.kde.kdeglobals.*"])
                self.assertIn(NESTED_GROUP, kde_settings)
                self.assertTrue(all(group.startswith("org.kde.kdeglobals.") for group in kde_settings))
                self.assertEqual(self.read_all(service, ["org.sonicde.nonexistent.*"]), {})
                self.assertEqual(self.read(service, NESTED_GROUP, "Greeting"), "nested-read-ok")

                for group, key in (
                    ("", "Greeting"),
                    ("org.sonicde.nonexistent", "Greeting"),
                    (NESTED_GROUP, "MissingKey"),
                    ("org.kde.kdeglobals.MissingGroup", "Greeting"),
                    ("org.kde.kdeglobalsUnexpected.PortalSettingsTest", "Greeting"),
                    ("org.kde.VirtualKeyboardUnexpected", "available"),
                    ("org.kde.VirtualKeyboard.MissingGroup", "available"),
                    ("org.kde.TabletMode.MissingGroup", "available"),
                    ("org.freedesktop.appearance", "MissingKey"),
                ):
                    with self.subTest(group=group, key=key):
                        self.assert_unknown_property(service, group, key)


if __name__ == "__main__":
    if not shutil.which("Xvfb") or not shutil.which("dbus-daemon"):
        print("SKIP: Xvfb and dbus-daemon are required for the isolated settings test")
        sys.exit(77)
    EXECUTABLES = sys.argv[1:3]
    if len(EXECUTABLES) != 2:
        sys.exit("Expected SonicDE and KDE backend executable paths")
    del sys.argv[1:3]
    unittest.main()
