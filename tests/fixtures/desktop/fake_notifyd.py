#!/usr/bin/env python3
"""A stand-in org.freedesktop.Notifications server for vyto/desktop/notify.

Runs on a private session bus (dbus-run-session) and logs every call with its
typed arguments, one line each, to $FAKE_DIR/notify.log, so the test compares
exactly what crossed D-Bus. FAKE_MARKUP=1 advertises body-markup, which is what
makes the shim escape & < >. FAKE_DELAY_MS holds every Notify reply back that
long, the way GNOME 42 does (2.5-6 s), to prove no caller waits. Touches $FAKE_DIR/ready once it owns the name.
"""
import os
from gi.repository import Gio, GLib

XML = """<node><interface name="org.freedesktop.Notifications">
<method name="Notify"><arg type="s" direction="in"/><arg type="u" direction="in"/>
<arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="s" direction="in"/>
<arg type="as" direction="in"/><arg type="a{sv}" direction="in"/><arg type="i" direction="in"/>
<arg type="u" direction="out"/></method>
<method name="CloseNotification"><arg type="u" direction="in"/></method>
<method name="GetCapabilities"><arg type="as" direction="out"/></method>
<method name="GetServerInformation"><arg type="s" direction="out"/><arg type="s" direction="out"/>
<arg type="s" direction="out"/><arg type="s" direction="out"/></method>
</interface></node>"""

d = os.environ["FAKE_DIR"]
log = open(os.path.join(d, "notify.log"), "a", buffering=1, encoding="utf-8")
markup = os.environ.get("FAKE_MARKUP") == "1"
delay = int(os.environ.get("FAKE_DELAY_MS", "0"))
next_id = [41]


def on_call(conn, sender, path, iface, method, params, inv):
    if method == "GetServerInformation":
        inv.return_value(GLib.Variant("(ssss)", ("fake-notifyd", "vyto", "1", "1.2")))
    elif method == "GetCapabilities":
        caps = ["body", "body-markup"] if markup else ["body"]
        inv.return_value(GLib.Variant("(as)", (caps,)))
    elif method == "Notify":
        app, rep, icon, summary, body, actions, hints, tmo = params.unpack()
        nid = rep if rep else next_id[0] + 1
        if not rep:
            next_id[0] += 1
        h = " ".join(f"{k}={hints[k]!r}" for k in sorted(hints))
        log.write(f"Notify app={app!r} replaces={rep} icon={icon} summary={summary!r} "
                  f"body={body!r} actions={actions} hints[{h}] timeout={tmo} -> {nid}\n")
        reply = GLib.Variant("(u)", (nid,))
        if delay:
            GLib.timeout_add(delay, lambda: inv.return_value(reply) or False)
        else:
            inv.return_value(reply)
    elif method == "CloseNotification":
        log.write(f"Close {params.unpack()[0]}\n")
        inv.return_value(None)


def on_bus(conn, name):
    node = Gio.DBusNodeInfo.new_for_xml(XML)
    conn.register_object("/org/freedesktop/Notifications", node.interfaces[0], on_call)


def on_name(conn, name):
    open(os.path.join(d, "ready"), "w").close()


Gio.bus_own_name(Gio.BusType.SESSION, "org.freedesktop.Notifications",
                 Gio.BusNameOwnerFlags.NONE, on_bus, on_name, None)
GLib.MainLoop().run()
