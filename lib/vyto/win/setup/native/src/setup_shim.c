/* vyto/win/setup — the Windows pieces of the installer and uninstaller that
 * Vyto cannot reach from its stdlib: the embedded payload resource, known
 * folders, shortcuts, the Add/Remove Programs entry, elevation, processes.
 * See ../../setup.vt.
 *
 * Every function used is in the audited Windows 7 export list. Off Windows
 * (and under --freestanding) every entry point reports failure: the package
 * still compiles there, which is what lets its pure-Vyto parts be tested on
 * Linux.
 *
 * Strings cross as UTF-8 and become UTF-16 here (vyto_win.h); returned paths
 * come from a small ring of static buffers, valid until four more calls. */
#include <stdint.h>

#if defined(_WIN32) && !defined(VT_NO_LIBC)

#define COBJMACROS
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <knownfolders.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vyto_win.h"

static char g_ring[4][4096];
static int g_next;
static const char *ring(const char *s) {
    char *b = g_ring[g_next++ & 3];
    snprintf(b, sizeof g_ring[0], "%s", s ? s : "");
    return b;
}
static const char *ring_w(const wchar_t *w) {
    char *u = vt_utf8_from_w(w);
    const char *r = ring(u ? u : "");
    free(u);
    return r;
}

/* RCDATA 100 is the payload vytoc embedded (--win-rcdata). Written to `out`
   so the Vyto side can read it with the file API; 0 when there is none
   (that is the uninstaller). */
int32_t vset_payload_to_file(const char *out) {
    HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(100), MAKEINTRESOURCEW(10)); /* 10 = RT_RCDATA */
    if (!r) return 0;
    HGLOBAL g = LoadResource(NULL, r);
    DWORD n = SizeofResource(NULL, r);
    const void *p = g ? LockResource(g) : NULL;
    if (!p || n == 0) return 0;
    VtWBuf w;
    vt_wbuf_from(&w, out);
    FILE *f = _wfopen(w.p, L"wb");
    vt_wbuf_free(&w);
    if (!f) return -1;
    size_t ok = fwrite(p, 1, n, f);
    fclose(f);
    return ok == n ? 1 : -1;
}

/* 0 Program Files, 1 per-user Programs (%LOCALAPPDATA%\Programs),
   2 all users' Start menu Programs, 3 this user's Start menu Programs,
   4 all users' Desktop, 5 this user's Desktop, 6 the temp dir. */
const char *vset_folder(int32_t which) {
    if (which == 6) {
        wchar_t t[MAX_PATH + 1];
        DWORD n = GetTempPathW(MAX_PATH + 1, t);
        if (n == 0) return "";
        if (n > 0 && t[n - 1] == L'\\') t[n - 1] = 0;
        return ring_w(t);
    }
    const KNOWNFOLDERID *ids[6] = { &FOLDERID_ProgramFiles, &FOLDERID_UserProgramFiles,
                                    &FOLDERID_CommonPrograms, &FOLDERID_Programs,
                                    &FOLDERID_PublicDesktop, &FOLDERID_Desktop };
    if (which < 0 || which > 5) return "";
    PWSTR p = NULL;
    /* KF_FLAG_CREATE: %LOCALAPPDATA%\Programs need not exist yet */
    if (FAILED(SHGetKnownFolderPath(ids[which], 0x00008000, NULL, &p)) || !p) {
        /* FOLDERID_UserProgramFiles is %LOCALAPPDATA%\Programs by definition;
           build it from LocalAppData where the id is unknown (Wine 6). */
        if (which != 1) return "";
        if (FAILED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &p)) || !p) return "";
        wchar_t buf[1100];
        _snwprintf(buf, 1099, L"%ls\\Programs", p);
        buf[1099] = 0;
        CoTaskMemFree(p);
        CreateDirectoryW(buf, NULL);
        return ring_w(buf);
    }
    const char *r = ring_w(p);
    CoTaskMemFree(p);
    return r;
}

/* A .lnk at `lnk` pointing at `target`. */
int32_t vset_shortcut(const char *lnk, const char *target, const char *args, const char *workdir,
                      const char *desc) {
    HRESULT ci = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    (void)ci;
    IShellLinkW *sl = NULL;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW,
                                (void **)&sl)) || !sl)
        return 0;
    VtWBuf t, a, w, d, l;
    vt_wbuf_from(&t, target);
    vt_wbuf_from(&a, args ? args : "");
    vt_wbuf_from(&w, workdir ? workdir : "");
    vt_wbuf_from(&d, desc ? desc : "");
    vt_wbuf_from(&l, lnk);
    IShellLinkW_SetPath(sl, t.p);
    if (args && *args) IShellLinkW_SetArguments(sl, a.p);
    if (workdir && *workdir) IShellLinkW_SetWorkingDirectory(sl, w.p);
    if (desc && *desc) IShellLinkW_SetDescription(sl, d.p);
    IShellLinkW_SetIconLocation(sl, t.p, 0);
    IPersistFile *pf = NULL;
    int ok = 0;
    if (SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void **)&pf)) && pf) {
        ok = SUCCEEDED(IPersistFile_Save(pf, l.p, TRUE));
        IPersistFile_Release(pf);
    }
    IShellLinkW_Release(sl);
    vt_wbuf_free(&t); vt_wbuf_free(&a); vt_wbuf_free(&w); vt_wbuf_free(&d); vt_wbuf_free(&l);
    return ok;
}

static HKEY hive(int32_t machine) { return machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER; }

static const wchar_t *UNINST = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\";

static void set_sz(HKEY k, const wchar_t *name, const char *val) {
    if (!val || !*val) return;
    VtWBuf w;
    vt_wbuf_from(&w, val);
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)w.p, (DWORD)((wcslen(w.p) + 1) * sizeof(wchar_t)));
    vt_wbuf_free(&w);
}

/* The Add/Remove Programs entry. */
int32_t vset_reg_uninstall(int32_t machine, const char *key, const char *name, const char *version,
                           const char *publisher, const char *location, const char *icon,
                           const char *uninstall, const char *quiet, int32_t size_kb) {
    wchar_t path[512];
    VtWBuf wk;
    vt_wbuf_from(&wk, key);
    _snwprintf(path, 511, L"%ls%ls", UNINST, wk.p);
    path[511] = 0;
    vt_wbuf_free(&wk);
    HKEY k;
    if (RegCreateKeyExW(hive(machine), path, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS)
        return 0;
    set_sz(k, L"DisplayName", name);
    set_sz(k, L"DisplayVersion", version);
    set_sz(k, L"Publisher", publisher);
    set_sz(k, L"InstallLocation", location);
    set_sz(k, L"DisplayIcon", icon);
    set_sz(k, L"UninstallString", uninstall);
    set_sz(k, L"QuietUninstallString", quiet);
    DWORD one = 1, kb = (DWORD)size_kb;
    RegSetValueExW(k, L"NoModify", 0, REG_DWORD, (const BYTE *)&one, sizeof one);
    RegSetValueExW(k, L"NoRepair", 0, REG_DWORD, (const BYTE *)&one, sizeof one);
    RegSetValueExW(k, L"EstimatedSize", 0, REG_DWORD, (const BYTE *)&kb, sizeof kb);
    RegCloseKey(k);
    return 1;
}

/* A value from an existing entry ("" when there is none): an upgrade reads
   DisplayVersion and InstallLocation from here. */
const char *vset_reg_get(int32_t machine, const char *key, const char *value) {
    wchar_t path[512];
    VtWBuf wk, wv;
    vt_wbuf_from(&wk, key);
    vt_wbuf_from(&wv, value);
    _snwprintf(path, 511, L"%ls%ls", UNINST, wk.p);
    path[511] = 0;
    HKEY k;
    const char *out = "";
    if (RegOpenKeyExW(hive(machine), path, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        wchar_t buf[2048];
        DWORD type = 0, sz = sizeof buf - sizeof(wchar_t);
        if (RegQueryValueExW(k, wv.p, NULL, &type, (BYTE *)buf, &sz) == ERROR_SUCCESS && type == REG_SZ) {
            buf[sz / sizeof(wchar_t)] = 0;
            out = ring_w(buf);
        }
        RegCloseKey(k);
    }
    vt_wbuf_free(&wk);
    vt_wbuf_free(&wv);
    return out;
}

int32_t vset_reg_remove(int32_t machine, const char *key) {
    wchar_t path[512];
    VtWBuf wk;
    vt_wbuf_from(&wk, key);
    _snwprintf(path, 511, L"%ls%ls", UNINST, wk.p);
    path[511] = 0;
    vt_wbuf_free(&wk);
    LONG r = RegDeleteTreeW(hive(machine), path);
    return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
}

/* Running elevated (or as an administrator with UAC off)? */
int32_t vset_is_admin(void) {
    HANDLE t;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return 0;
    TOKEN_ELEVATION e;
    DWORD n = 0;
    int r = GetTokenInformation(t, TokenElevation, &e, sizeof e, &n) && e.TokenIsElevated;
    CloseHandle(t);
    return r;
}

/* 1 when another process has `path` open (a running app's own exe): the
   copy would fail, so the installer asks for the app to be closed. */
int32_t vset_in_use(const char *path) {
    VtWBuf w;
    vt_wbuf_from(&w, path);
    HANDLE h = CreateFileW(w.p, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD e = GetLastError();
    vt_wbuf_free(&w);
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return 0; }
    return e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION;
}

/* Start `exe` with a command line; wait for it when `wait`, returning its
   exit code (or 0 when not waiting); -1 when it could not be started. */
int32_t vset_spawn(const char *exe, const char *args, int32_t wait) {
    char cmd[8192];
    snprintf(cmd, sizeof cmd, "\"%s\"%s%s", exe, args && *args ? " " : "", args ? args : "");
    VtWBuf we, wc;
    vt_wbuf_from(&we, exe);
    vt_wbuf_from(&wc, cmd);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    BOOL ok = CreateProcessW(we.p, wc.p, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    vt_wbuf_free(&we);
    vt_wbuf_free(&wc);
    if (!ok) return -1;
    DWORD code = 0;
    if (wait) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int32_t)code;
}

int32_t vset_pid(void) { return (int32_t)GetCurrentProcessId(); }

/* Wait for process `pid` to exit (the uninstaller's first stage, so its exe
   can be deleted); 1 when it has, 0 on timeout. */
int32_t vset_wait_pid(int32_t pid, int32_t ms) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
    if (!h) return 1;   /* already gone */
    DWORD r = WaitForSingleObject(h, (DWORD)ms);
    CloseHandle(h);
    return r == WAIT_OBJECT_0;
}

int32_t vset_rmdir(const char *path) {
    VtWBuf w;
    vt_wbuf_from(&w, path);
    BOOL ok = RemoveDirectoryW(w.p);
    vt_wbuf_free(&w);
    return ok != 0;
}

/* Delete `path` at the next restart: how the uninstaller's temporary copy of
   itself goes away, since a running exe cannot delete itself. */
int32_t vset_delete_on_reboot(const char *path) {
    VtWBuf w;
    vt_wbuf_from(&w, path);
    BOOL ok = MoveFileExW(w.p, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    vt_wbuf_free(&w);
    return ok != 0;
}

/* Tell Explorer the Start menu / desktop changed. */
void vset_shell_changed(void) { SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL); }

int32_t vset_available(void) { return 1; }

#else /* not Windows: every entry point reports failure */

int32_t vset_payload_to_file(const char *out) { (void)out; return 0; }
const char *vset_folder(int32_t which) { (void)which; return ""; }
int32_t vset_shortcut(const char *lnk, const char *target, const char *args, const char *workdir,
                      const char *desc) {
    (void)lnk; (void)target; (void)args; (void)workdir; (void)desc;
    return 0;
}
int32_t vset_reg_uninstall(int32_t machine, const char *key, const char *name, const char *version,
                           const char *publisher, const char *location, const char *icon,
                           const char *uninstall, const char *quiet, int32_t size_kb) {
    (void)machine; (void)key; (void)name; (void)version; (void)publisher; (void)location;
    (void)icon; (void)uninstall; (void)quiet; (void)size_kb;
    return 0;
}
const char *vset_reg_get(int32_t machine, const char *key, const char *value) {
    (void)machine; (void)key; (void)value;
    return "";
}
int32_t vset_reg_remove(int32_t machine, const char *key) { (void)machine; (void)key; return 0; }
int32_t vset_is_admin(void) { return 0; }
int32_t vset_in_use(const char *path) { (void)path; return 0; }
int32_t vset_spawn(const char *exe, const char *args, int32_t wait) {
    (void)exe; (void)args; (void)wait;
    return -1;
}
int32_t vset_pid(void) { return 0; }
int32_t vset_wait_pid(int32_t pid, int32_t ms) { (void)pid; (void)ms; return 1; }
int32_t vset_rmdir(const char *path) { (void)path; return 0; }
int32_t vset_delete_on_reboot(const char *path) { (void)path; return 0; }
void vset_shell_changed(void) { }
int32_t vset_available(void) { return 0; }

#endif
