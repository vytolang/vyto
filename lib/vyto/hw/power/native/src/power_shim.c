/* vyto/hw/power native backing — soft sysfs reader for power/battery/thermal status.

   Battery, AC, and thermal state live in world-readable sysfs
   (/sys/class/power_supply/*, /sys/class/thermal/*). Attributes vary by supply type
   (a mains adapter has `online` but no `capacity`; a battery the reverse), so a missing
   file reads as "" rather than panicking like readfile — soft, matching vyto/hw. Same
   read_attr shape as usb_shim. Linux; other platforms read nothing. */

#include <stdio.h>
#include <string.h>

/* Read a sysfs file into a static buffer, trailing whitespace stripped. "" if absent.
   Single-threaded; wrap each call in str() before the next. */
const char *sysfs_read(const char *path) {
    static char buf[1024];
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
