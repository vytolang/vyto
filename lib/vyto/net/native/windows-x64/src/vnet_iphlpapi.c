/* if_nametoindex for the vendored curl on windows-x64. First-party; a refresh
 * leaves it alone.
 *
 * curl_setup.h turns HAVE_IF_NAMETOINDEX on for every mingw build, and curl
 * uses it for one thing: an IPv6 zone written as a name ("fe80::1%eth0") in a
 * URL. The real function lives in iphlpapi.dll, which exists on Windows 7 but
 * is not in the audited Windows 7 export list (lib/vyto/win/win7_exports.txt),
 * so linking it would fail the import audit. Resolving it at run time keeps the
 * exe's import table to kernel32/msvcrt/ws2_32/bcrypt — the same reason
 * vsurf.c loads dwmapi this way. If the DLL or the export is missing the
 * answer is 0, which curl reports as an unknown interface; numeric zones
 * ("%3") never call this at all. */
#if defined(_WIN32) && !defined(VT_NO_LIBC)
#include <windows.h>

typedef ULONG (WINAPI *vnet_ifn_fn)(PCSTR);

ULONG WINAPI if_nametoindex(PCSTR name) {
    static vnet_ifn_fn fn;
    static volatile LONG state; /* 0 untried, 1 resolving, 2 done */
    if (InterlockedCompareExchange(&state, 1, 0) == 0) {
        HMODULE h = LoadLibraryW(L"iphlpapi.dll");
        if (h) fn = (vnet_ifn_fn)(void (*)(void))GetProcAddress(h, "if_nametoindex");
        InterlockedExchange(&state, 2);
    }
    while (state != 2) Sleep(0);
    return fn ? fn(name) : 0;
}
#endif
