/* dump_exports.c — list what a Windows install's system DLLs export.
 *
 * Built by tests/run_tests_win.sh with mingw and run by run.ps1 on the
 * Windows box. Its output, committed as lib/vyto/win/win7_exports.txt from a
 * Windows 7 run, is the allowlist `vytoc audit` (src/peaudit.c) checks every exe's
 * imports against: an import missing from it would fail to load on Windows 7
 * with "entry point not found", the kind of break nothing else catches until
 * a user hits it.
 *
 *   dump_exports.exe               the DLLs Vyto programs import
 *   dump_exports.exe a.dll b.dll   just these
 *
 * Output: a "# windows <major>.<minor>.<build>" line, then "dll:function"
 * lines, DLL names lowercased. Each DLL is mapped as an image without running
 * its DllMain (DONT_RESOLVE_DLL_REFERENCES), so the export table's RVAs can be
 * read straight off the module base.
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* Every system DLL a Vyto exe can import: mingw's implicit set (kernel32,
   user32, advapi32, shell32, msvcrt), what the stdlib names with
   #link ... if "windows", and what vsurf and the CRT pull in. */
static const char *k_default[] = {
    "kernel32.dll", "user32.dll", "gdi32.dll",   "advapi32.dll", "shell32.dll",
    "msvcrt.dll",   "ole32.dll",  "oleaut32.dll", "ws2_32.dll",   "bcrypt.dll",
    "d2d1.dll",     "dwrite.dll", "windowscodecs.dll", "psapi.dll", "comdlg32.dll",
    "imm32.dll",    "shcore.dll",
    /* vyto/hw: device listing (SetupAPI, CfgMgr), HID, WinUSB */
    "setupapi.dll", "cfgmgr32.dll", "hid.dll", "winusb.dll",
};

typedef LONG(WINAPI *RtlGetVersionFn)(OSVERSIONINFOW *);

static void print_version(void) {
    OSVERSIONINFOW v;
    memset(&v, 0, sizeof v);
    v.dwOSVersionInfoSize = sizeof v;
    /* RtlGetVersion tells the truth; GetVersionEx answers 6.2 to any process
       without a supportedOS manifest entry for 8.1 and later. */
    RtlGetVersionFn f =
        (RtlGetVersionFn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
    if (f && f(&v) == 0)
        printf("# windows %lu.%lu.%lu\n", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    else
        printf("# windows unknown\n");
}

static int dump(const char *dll) {
    HMODULE m = LoadLibraryExA(dll, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!m) {
        /* Absent on this Windows (shcore.dll on 7): its absence is the fact
           worth recording, so nothing from it can pass the audit. */
        printf("# missing %s\n", dll);
        return 0;
    }
    const BYTE *base = (const BYTE *)m;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    char lower[MAX_PATH];
    size_t n = strlen(dll);
    if (n >= sizeof lower) n = sizeof lower - 1;
    for (size_t i = 0; i < n; i++) lower[i] = (char)tolower((unsigned char)dll[i]);
    lower[n] = 0;
    int count = 0;
    if (dd->VirtualAddress && dd->Size) {
        const IMAGE_EXPORT_DIRECTORY *ed = (const IMAGE_EXPORT_DIRECTORY *)(base + dd->VirtualAddress);
        const DWORD *names = (const DWORD *)(base + ed->AddressOfNames);
        for (DWORD i = 0; i < ed->NumberOfNames; i++) {
            printf("%s:%s\n", lower, (const char *)(base + names[i]));
            count++;
        }
    }
    FreeLibrary(m);
    return count;
}

int main(int argc, char **argv) {
    print_version();
    if (argc > 1) {
        for (int i = 1; i < argc; i++) dump(argv[i]);
    } else {
        for (size_t i = 0; i < sizeof k_default / sizeof k_default[0]; i++) dump(k_default[i]);
    }
    return 0;
}
