/* vyto/desktop/menu — the OS's own menu bar. See ../../README.md.
 *
 * Two arms, one set of entry points:
 *
 *   Windows   CreateMenu / AppendMenuW / SetMenu, and SetMenuItemInfoW for
 *             live state. Every function is in the audited Windows 7 export
 *             list (lib/vyto/win/win7_exports.txt).
 *   stub      everything else — X11 has no menu bar of its own, and linking
 *             GTK or Qt for one is exactly what vyto/desktop exists to avoid.
 *             vdmenu_supported() answers 0 and the app's MenuBar draws itself.
 *             Includes nothing past <stdint.h>, so a --freestanding build of a
 *             program that imports this package still compiles.
 *
 * The menu is built and owned here only as OS handles; the model lives in
 * Vyto (vyto/ui/menu). A choice comes back through the surface: vsurf's
 * wndproc turns WM_COMMAND into VS_EV_COMMAND with the item's id, and the
 * Window hands that to MenuBar.on_command. Nothing here sees the event loop.
 *
 * A window destroys the menu attached to it, so there is no free on the
 * normal path; vdmenu_destroy is for a menu that never got attached. */
#include <stdint.h>

/* Item flags, shared with menu.vt. */
#define VDM_SEPARATOR 1
#define VDM_DISABLED  2
#define VDM_CHECKED   4

#if defined(_WIN32) && !defined(VT_NO_LIBC)

#include <windows.h>
#include "vyto_win.h"

int32_t vdmenu_supported(void) { return 1; }
int32_t vdmenu_is_windows(void) { return 1; }

void *vdmenu_create(int32_t is_bar) {
    return is_bar ? (void *)CreateMenu() : (void *)CreatePopupMenu();
}

void vdmenu_destroy(void *m) {
    if (m) DestroyMenu((HMENU)m);
}

/* "label" + TAB + "shortcut" as UTF-16. Windows right-aligns whatever follows
   the tab, which is how every native menu shows its shortcuts. The & access
   key markup is already in label (menu.vt escapes a literal & as &&). */
static int item_text(VtWBuf *w, const char *label, const char *shortcut) {
    size_t a = label ? strlen(label) : 0;
    size_t b = shortcut ? strlen(shortcut) : 0;
    if (b == 0) return vt_wbuf_from(w, label ? label : "");
    char *t = malloc(a + b + 2);
    if (!t) { vt_wbuf_from(w, ""); return 0; }
    memcpy(t, label, a);
    t[a] = '\t';
    memcpy(t + a + 1, shortcut, b);
    t[a + b + 1] = 0;
    int ok = vt_wbuf_from(w, t);
    free(t);
    return ok;
}

static UINT state_flags(int32_t flags) {
    UINT f = 0;
    if (flags & VDM_DISABLED) f |= MF_GRAYED;
    if (flags & VDM_CHECKED) f |= MF_CHECKED;
    return f;
}

int32_t vdmenu_append(void *m, int32_t id, const char *label, const char *shortcut,
                      int32_t flags) {
    if (!m) return 0;
    if (flags & VDM_SEPARATOR) return AppendMenuW((HMENU)m, MF_SEPARATOR, 0, NULL) ? 1 : 0;
    VtWBuf w;
    item_text(&w, label, shortcut);
    BOOL ok = AppendMenuW((HMENU)m, MF_STRING | state_flags(flags), (UINT_PTR)(uint32_t)id, w.p);
    vt_wbuf_free(&w);
    return ok ? 1 : 0;
}

int32_t vdmenu_append_sub(void *m, void *sub, const char *label, int32_t flags) {
    if (!m || !sub) return 0;
    VtWBuf w;
    vt_wbuf_from(&w, label ? label : "");
    BOOL ok = AppendMenuW((HMENU)m, MF_STRING | MF_POPUP | state_flags(flags),
                          (UINT_PTR)(HMENU)sub, w.p);
    vt_wbuf_free(&w);
    return ok ? 1 : 0;
}

/* Install `bar` on the window. SetMenu takes the menu's height out of the
   client area; the window grows by that much so the app keeps the size it
   asked for — unless it is maximized, where growing would push it off-screen. */
int32_t vdmenu_attach(unsigned long hwnd_id, void *bar) {
    HWND h = (HWND)(uintptr_t)hwnd_id;
    if (!h || !bar || !IsWindow(h)) return 0;
    RECT before, after;
    GetClientRect(h, &before);
    if (!SetMenu(h, (HMENU)bar)) return 0;
    GetClientRect(h, &after);
    int dh = (int)(before.bottom - before.top) - (int)(after.bottom - after.top);
    if (dh > 0 && !IsZoomed(h) && !IsIconic(h)) {
        RECT wr;
        GetWindowRect(h, &wr);
        SetWindowPos(h, NULL, 0, 0, wr.right - wr.left, wr.bottom - wr.top + dh,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    DrawMenuBar(h);
    return 1;
}

/* Label, enabled and checked of the command item `id`, anywhere under `bar`
   (by-command lookup searches submenus). */
int32_t vdmenu_update(void *bar, int32_t id, const char *label, const char *shortcut,
                      int32_t flags) {
    if (!bar || id <= 0) return 0;
    VtWBuf w;
    item_text(&w, label, shortcut);
    MENUITEMINFOW mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    mi.fMask = MIIM_STATE | MIIM_STRING;
    mi.fState = ((flags & VDM_DISABLED) ? MFS_DISABLED : MFS_ENABLED)
              | ((flags & VDM_CHECKED) ? MFS_CHECKED : MFS_UNCHECKED);
    mi.dwTypeData = w.p;
    BOOL ok = SetMenuItemInfoW((HMENU)bar, (UINT)id, FALSE, &mi);
    vt_wbuf_free(&w);
    return ok ? 1 : 0;
}

#else /* X11, Android, freestanding: no menu bar of the OS's own */

int32_t vdmenu_supported(void) { return 0; }
int32_t vdmenu_is_windows(void) {
#ifdef _WIN32
    return 1;
#else
    return 0;
#endif
}
void *vdmenu_create(int32_t is_bar) { (void)is_bar; return 0; }
void vdmenu_destroy(void *m) { (void)m; }
int32_t vdmenu_append(void *m, int32_t id, const char *label, const char *shortcut,
                      int32_t flags) {
    (void)m; (void)id; (void)label; (void)shortcut; (void)flags;
    return 0;
}
int32_t vdmenu_append_sub(void *m, void *sub, const char *label, int32_t flags) {
    (void)m; (void)sub; (void)label; (void)flags;
    return 0;
}
int32_t vdmenu_attach(unsigned long hwnd_id, void *bar) {
    (void)hwnd_id; (void)bar;
    return 0;
}
int32_t vdmenu_update(void *bar, int32_t id, const char *label, const char *shortcut,
                      int32_t flags) {
    (void)bar; (void)id; (void)label; (void)shortcut; (void)flags;
    return 0;
}

#endif
