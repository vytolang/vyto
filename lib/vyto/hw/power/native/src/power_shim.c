/* vyto/hw/power native backing — battery, AC adapter and thermal status.

   Soft everywhere: a missing attribute reads as "" rather than panicking, so a
   mains adapter (no `capacity`) and a battery (no `online`) share one API.

   Linux: world-readable sysfs, /sys/class/power_supply/<name>/<attr> and
   /sys/class/thermal. Windows: GetSystemPowerStatus, presented as the same
   supplies and attributes Linux has: "AC" (type Mains, online) when Windows
   knows the adapter state, and "BAT0" (type Battery, capacity, status) when the
   machine has a battery. Windows reports all batteries as one, so a laptop
   with two shows one BAT0 at their combined level. No voltage, and no thermal
   zones: Windows has no API for either that works without administrator
   rights. */

#include <stdio.h>
#include <string.h>

#if defined(VT_NO_LIBC)
const char *sysfs_read(const char *path) { (void)path; return ""; }
const char *pwr_list(void) { return ""; }
const char *pwr_attr(const char *name, const char *attr) { (void)name; (void)attr; return ""; }
int pwr_has_thermal(void) { return 0; }
#else

/* Thermal zones come from sysfs, so only Linux has them. Asked here rather
   than by probing the path from Vyto: under Wine "/sys/class/thermal" is a
   real directory on drive Z:, and on Windows any such path is just a folder
   that happens to exist or not. */
int pwr_has_thermal(void) {
#ifdef __linux__
    return 1;
#else
    return 0;
#endif
}

static char g_buf[1024];

/* Read a sysfs file into a static buffer, trailing whitespace stripped. "" if
   absent. Single-threaded; wrap each call in str() before the next. */
const char *sysfs_read(const char *path) {
    g_buf[0] = 0;
#ifndef _WIN32
    FILE *f = fopen(path, "r");
    if (!f) return g_buf;
    size_t got = fread(g_buf, 1, sizeof g_buf - 1, f);
    fclose(f);
    g_buf[got] = 0;
    while (got > 0 && (g_buf[got - 1] == '\n' || g_buf[got - 1] == '\r' || g_buf[got - 1] == ' '))
        g_buf[--got] = 0;
#else
    (void)path;
#endif
    return g_buf;
}

#ifndef _WIN32
#include <dirent.h>

/* The supply names, newline-separated, sorted. "" when there is no sysfs. */
const char *pwr_list(void) {
    static char out[2048];
    char names[64][64];
    int n = 0;
    out[0] = 0;
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return out;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < 64) {
        if (e->d_name[0] == '.') continue;
        snprintf(names[n++], 64, "%s", e->d_name);
    }
    closedir(d);
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) {
            char t[64];
            memcpy(t, names[j], 64);
            memcpy(names[j], names[j - 1], 64);
            memcpy(names[j - 1], t, 64);
        }
    size_t len = 0;
    for (int i = 0; i < n; i++) {
        size_t k = strlen(names[i]);
        if (len + k + 2 >= sizeof out) break;
        if (len) out[len++] = '\n';
        memcpy(out + len, names[i], k);
        len += k;
        out[len] = 0;
    }
    return out;
}

const char *pwr_attr(const char *name, const char *attr) {
    char path[512];
    if (!name || !attr || strchr(name, '/') || strchr(attr, '/')) { g_buf[0] = 0; return g_buf; }
    snprintf(path, sizeof path, "/sys/class/power_supply/%s/%s", name, attr);
    return sysfs_read(path);
}

#else
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>

const char *pwr_list(void) {
    SYSTEM_POWER_STATUS st;
    static char out[16];
    out[0] = 0;
    if (!GetSystemPowerStatus(&st)) return out;
    int ac = st.ACLineStatus != 255;
    int bat = st.BatteryFlag != 128 && st.BatteryFlag != 255;
    snprintf(out, sizeof out, "%s%s%s", ac ? "AC" : "", ac && bat ? "\n" : "", bat ? "BAT0" : "");
    return out;
}

const char *pwr_attr(const char *name, const char *attr) {
    SYSTEM_POWER_STATUS st;
    g_buf[0] = 0;
    if (!name || !attr || !GetSystemPowerStatus(&st)) return g_buf;
    if (strcmp(name, "AC") == 0 && st.ACLineStatus != 255) {
        if (strcmp(attr, "type") == 0) snprintf(g_buf, sizeof g_buf, "Mains");
        else if (strcmp(attr, "online") == 0) snprintf(g_buf, sizeof g_buf, "%d", st.ACLineStatus == 1);
    } else if (strcmp(name, "BAT0") == 0 && st.BatteryFlag != 128 && st.BatteryFlag != 255) {
        int pct = st.BatteryLifePercent;
        if (strcmp(attr, "type") == 0) {
            snprintf(g_buf, sizeof g_buf, "Battery");
        } else if (strcmp(attr, "capacity") == 0) {
            if (pct <= 100) snprintf(g_buf, sizeof g_buf, "%d", pct);
        } else if (strcmp(attr, "status") == 0) {
            const char *s = "Unknown";
            if (st.BatteryFlag & 8) s = "Charging";
            else if (st.ACLineStatus == 0) s = "Discharging";
            else if (st.ACLineStatus == 1) s = pct == 100 ? "Full" : "Not charging";
            snprintf(g_buf, sizeof g_buf, "%s", s);
        }
    }
    return g_buf;
}
#endif /* _WIN32 */
#endif /* hosted */
