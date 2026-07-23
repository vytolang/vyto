/* vyto/net/link native backing — soft sysfs reader for network-interface status.

   Interface state lives in world-readable sysfs (/sys/class/net/<iface>/*). readfile
   panics on a missing attribute, but attributes come and go per interface type (a
   wifi device has no fixed `speed`, a down link has no `carrier`), so this returns ""
   for an absent/unreadable file instead — soft, matching the rest of vyto/hw. Same
   read_attr shape as usb_shim. Linux; other platforms read nothing. */

#include <stdio.h>
#include <string.h>

/* Read a sysfs file into a static buffer, trailing whitespace stripped. "" if absent.
   Single-threaded (Vyto has no threads); wrap each call in str() before the next. */
const char *sysfs_read(const char *path) {
    static char buf[4096];
    buf[0] = 0;
#ifndef _WIN32
    FILE *f = fopen(path, "r");
    if (!f) return buf;
    size_t got = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[got] = 0;
    while (got > 0 && (buf[got - 1] == '\n' || buf[got - 1] == '\r' || buf[got - 1] == ' '))
        buf[--got] = 0;
#else
    (void)path;
#endif
    return buf;
}
