/* memprobe.c — process memory figures for the painter bench. A diagnostic,
 * not a library: it lives inside apps/d2dbench and nothing else imports it.
 *
 * Windows reports what Task Manager's default column shows, the private
 * working set: resident pages not shared with any other process. It is
 * counted page by page with QueryWorkingSet (K32QueryWorkingSet in kernel32
 * on Windows 7), and
 * the breakdown attributes each private page to its owner: a DLL image, a
 * mapped file, or private memory (heaps, VirtualAlloc). The scan's own buffer
 * is private memory too, so the figures run a few tens of KB high.
 *
 * Linux reports VmRSS and RssAnon from /proc/self/status for comparison and
 * has no breakdown. Other targets report -1.
 */
#include <stdint.h>

#if defined(_WIN32) && !defined(VT_NO_LIBC)

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
/* Version 2 maps QueryWorkingSet and GetProcessMemoryInfo onto their K32
   forms in kernel32 (Windows 7+), so no psapi.dll is needed. */
#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MP_PAGE 4096

/* The working-set entries, or NULL. Grows the buffer until it fits. */
static PSAPI_WORKING_SET_INFORMATION *ws_entries(void) {
    size_t n = 8192;
    for (int tries = 0; tries < 8; tries++) {
        size_t bytes = sizeof(ULONG_PTR) + n * sizeof(PSAPI_WORKING_SET_BLOCK);
        PSAPI_WORKING_SET_INFORMATION *w = (PSAPI_WORKING_SET_INFORMATION *)malloc(bytes);
        if (!w) return NULL;
        if (QueryWorkingSet(GetCurrentProcess(), w, (DWORD)bytes)) return w;
        size_t need = (size_t)w->NumberOfEntries;
        free(w);
        if (GetLastError() != ERROR_BAD_LENGTH) return NULL;
        n = need + 1024;
    }
    return NULL;
}

static int is_private(const PSAPI_WORKING_SET_BLOCK *b) { return ((b->Flags >> 8) & 1) == 0; }

int64_t mp_private_ws_kb(void) {
    PSAPI_WORKING_SET_INFORMATION *w = ws_entries();
    if (!w) return -1;
    int64_t pages = 0;
    for (ULONG_PTR i = 0; i < w->NumberOfEntries; i++)
        if (is_private(&w->WorkingSetInfo[i])) pages++;
    free(w);
    return pages * (MP_PAGE / 1024);
}

int64_t mp_ws_kb(void) {
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc,
                                 sizeof pmc))
        return -1;
    return (int64_t)(pmc.WorkingSetSize / 1024);
}

int64_t mp_commit_kb(void) {
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc,
                                 sizeof pmc))
        return -1;
    return (int64_t)(pmc.PrivateUsage / 1024);
}

typedef struct {
    char name[64];
    int64_t pages;
} Bucket;

static int by_pages(const void *a, const void *b) {
    int64_t pa = ((const Bucket *)a)->pages, pb = ((const Bucket *)b)->pages;
    return pa < pb ? 1 : pa > pb ? -1 : 0;
}

/* Private working set by owner, largest first, as one line per owner. */
const char *mp_breakdown(void) {
    static char out[4096];
    out[0] = 0;
    PSAPI_WORKING_SET_INFORMATION *w = ws_entries();
    if (!w) return "breakdown unavailable";
    Bucket b[96];
    int nb = 0;
    for (ULONG_PTR i = 0; i < w->NumberOfEntries; i++) {
        const PSAPI_WORKING_SET_BLOCK *e = &w->WorkingSetInfo[i];
        if (!is_private(e)) continue;
        void *addr = (void *)((e->Flags >> 12) << 12);
        MEMORY_BASIC_INFORMATION mbi;
        char name[64] = "[unknown]";
        if (VirtualQuery(addr, &mbi, sizeof mbi)) {
            if (mbi.Type == MEM_IMAGE) {
                char path[MAX_PATH];
                if (GetModuleFileNameA((HMODULE)mbi.AllocationBase, path, sizeof path)) {
                    const char *base = strrchr(path, '\\');
                    snprintf(name, sizeof name, "%s", base ? base + 1 : path);
                } else {
                    snprintf(name, sizeof name, "[image]");
                }
            } else if (mbi.Type == MEM_MAPPED) {
                snprintf(name, sizeof name, "[mapped files]");
            } else {
                snprintf(name, sizeof name, "[heap / VirtualAlloc]");
            }
        }
        int k = 0;
        while (k < nb && strcmp(b[k].name, name) != 0) k++;
        if (k == nb) {
            if (nb == 96) k = 95;
            else {
                snprintf(b[nb].name, sizeof b[nb].name, "%s", name);
                b[nb].pages = 0;
                nb++;
            }
        }
        b[k].pages++;
    }
    free(w);
    qsort(b, (size_t)nb, sizeof b[0], by_pages);
    size_t len = 0;
    for (int k = 0; k < nb && len + 96 < sizeof out; k++)
        len += (size_t)snprintf(out + len, sizeof out - len, "  %8lldK  %s\n",
                                (long long)(b[k].pages * (MP_PAGE / 1024)), b[k].name);
    if (len > 0) out[len - 1] = 0;
    return out;
}

#elif defined(__linux__) && !defined(VT_NO_LIBC)

#include <stdio.h>
#include <string.h>

static int64_t status_kb(const char *key) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    int64_t v = -1;
    size_t kl = strlen(key);
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, kl) == 0) {
            long long x = -1;
            if (sscanf(line + kl, "%lld", &x) == 1) v = x;
            break;
        }
    }
    fclose(f);
    return v;
}

int64_t mp_private_ws_kb(void) { return status_kb("RssAnon:"); }
int64_t mp_ws_kb(void) { return status_kb("VmRSS:"); }
int64_t mp_commit_kb(void) { return status_kb("VmData:"); }
const char *mp_breakdown(void) { return "  (no breakdown on Linux)"; }

#else

int64_t mp_private_ws_kb(void) { return -1; }
int64_t mp_ws_kb(void) { return -1; }
int64_t mp_commit_kb(void) { return -1; }
const char *mp_breakdown(void) { return "  (no breakdown on this target)"; }

#endif
