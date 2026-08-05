/* vyto/mmap native backing — file and anonymous memory mappings.

   Opaque handle + explicit free, the file_shim.c convention: vmm_open/vmm_anon
   are the constructors and return NULL on failure, vmm_close is the single
   free, every other call returns 0 on success and negative on failure. The
   handle carries the base, the length and (on Windows) the two HANDLEs, so the
   Vyto side never learns which platform it is on.

   vytoc compiles every native/src/*.c for every target with no per-file filter
   (CLAUDE.md), so the whole file is bracketed three ways: a freestanding stub
   arm, then Win32, then POSIX.

   The base/map_base split is not decoration. POSIX mmap needs a page-multiple
   offset; Win32 MapViewOfFile needs a dwAllocationGranularity-multiple offset,
   and that is 64 KiB, not 4 KiB. The shim rounds the request down to whatever
   the platform demands, maps from there, and hands back base = map_base +
   delta. Leaving this to the caller produces a bug that only appears on
   Windows. */

/* Advice values. Defined here so one numbering serves every platform; the Vyto
   side re-exports them as ADVICE_*. */
#define VMM_NORMAL     0
#define VMM_RANDOM     1
#define VMM_SEQUENTIAL 2
#define VMM_WILLNEED   3
#define VMM_DONTNEED   4

/* ------------------------------------------------------------------------ */
#ifdef VT_NO_LIBC
/* Freestanding: there is no OS to map from and no libc to reach it with.
   --freestanding splices -DVT_NO_LIBC into the package shim's compile line, so
   an unguarded <sys/mman.h> would break the build of any program that merely
   imports vyto/mmap. Keep every symbol so such a program still links, and make
   each one fail exactly as a permission error would — constructors NULL, ops
   negative. vyto/mmap therefore degrades to "unsupported on this target" with
   no #ifdef anywhere in mmap.vt. An embedder that does have an MMU replaces
   this file.

   Deliberately no #include at all, not even <stddef.h>, so this arm cannot
   regress into depending on a hosted header. */

void      *vmm_open(const char *p, int w, long long o, long long n)
                                        { (void)p; (void)w; (void)o; (void)n; return 0; }
void      *vmm_anon(long long n)        { (void)n; return 0; }
void      *vmm_base(void *h)            { (void)h; return 0; }
void      *vmm_offset(void *h, long long o) { (void)h; (void)o; return 0; }
long long  vmm_len(void *h)             { (void)h; return 0; }
int        vmm_writable(void *h)        { (void)h; return 0; }
int        vmm_flush(void *h, int s)    { (void)h; (void)s; return -1; }
int        vmm_advise(void *h, int a)   { (void)h; (void)a; return -1; }
long long  vmm_page_size(void)          { return 4096; }
long long  vmm_granularity(void)        { return 4096; }
long long  vmm_file_size(const char *p) { (void)p; return -1; }
void       vmm_close(void *h)           { (void)h; }

/* ------------------------------------------------------------------------ */
#else

#ifdef _WIN32
/* Before any include: mingw's _mingw.h (reached via <stdio.h>) defaults
   _WIN32_WINNT to the XP-era value. 0x0600 is Vista, this port's floor —
   deliberately NOT 0x0602, because raising it lets PrefetchVirtualMemory bind
   at link time and the binary then refuses to load on Windows 7. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#include <stdlib.h>
#include <string.h>

typedef struct {
    void      *base;     /* first byte the caller sees */
    long long  len;      /* bytes the caller sees */
    void      *map_base; /* what the OS returned; base = map_base + delta */
    long long  map_len;  /* what must be unmapped */
    int        writable;
    int        anon;
#ifdef _WIN32
    void      *hfile;    /* HANDLE, INVALID_HANDLE_VALUE for anonymous */
    void      *hmap;     /* HANDLE from CreateFileMapping */
#endif
} VmmMap;

long long vmm_len(void *h)      { VmmMap *m = h; return m ? m->len : 0; }
int       vmm_writable(void *h) { VmmMap *m = h; return m ? m->writable : 0; }
void     *vmm_base(void *h)     { VmmMap *m = h; return m ? m->base : 0; }

void *vmm_offset(void *h, long long o) {
    VmmMap *m = h;
    if (!m || o < 0 || o > m->len) return 0;
    return (char *)m->base + o;
}

/* ---------------------------- Windows ---------------------------------- */
#ifdef _WIN32

long long vmm_page_size(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (long long)si.dwPageSize;
}

/* Not the page size: MapViewOfFile rounds offsets to the *allocation*
   granularity, which is 64 KiB on every Windows to date. */
long long vmm_granularity(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (long long)si.dwAllocationGranularity;
}

long long vmm_file_size(const char *path) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return -1;
    return ((long long)fad.nFileSizeHigh << 32) | (long long)fad.nFileSizeLow;
}

/* PrefetchVirtualMemory is Windows 8+. Resolve it by name at first use rather
   than importing it, so the binary still loads on Win7 (where advise() simply
   becomes a no-op). WIN32_MEMORY_RANGE_ENTRY only exists in Win8+ SDK headers
   and mingw-w64 versions vary, so declare the shape locally. */
typedef struct { PVOID VirtualAddress; SIZE_T NumberOfBytes; } VmmRange;
typedef BOOL (WINAPI *VmmPfnPrefetch)(HANDLE, ULONG_PTR, VmmRange *, ULONG);

static VmmPfnPrefetch vmm_prefetch_fn(void) {
    static VmmPfnPrefetch fn;
    static int tried;
    if (!tried) {
        tried = 1;
        HMODULE k = GetModuleHandleA("kernel32.dll");
        if (k) fn = (VmmPfnPrefetch)(void *)GetProcAddress(k, "PrefetchVirtualMemory");
    }
    return fn;
}

void *vmm_open(const char *path, int writable, long long off, long long len) {
    if (!path || off < 0 || len < 0) return 0;
    long long fsz = vmm_file_size(path);
    if (fsz < 0) return 0;
    if (off > fsz) return 0;
    if (len == 0) len = fsz - off;          /* 0 means "to end of file" */
    if (off + len > fsz) return 0;

    HANDLE hf = CreateFileA(path,
                            writable ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ,
                            FILE_SHARE_READ | (writable ? 0 : FILE_SHARE_WRITE),
                            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return 0;

    VmmMap *m = calloc(1, sizeof *m);
    if (!m) { CloseHandle(hf); return 0; }
    m->hfile = hf;
    m->writable = writable != 0;
    m->len = len;

    if (len == 0) {   /* an empty file maps to nothing; still a valid handle */
        m->hmap = NULL;
        return m;
    }

    HANDLE hm = CreateFileMappingA(hf, NULL, writable ? PAGE_READWRITE : PAGE_READONLY,
                                   0, 0, NULL);
    if (!hm) { CloseHandle(hf); free(m); return 0; }
    m->hmap = hm;

    long long gran  = vmm_granularity();
    long long start = off - (off % gran);   /* round the offset down */
    long long delta = off - start;
    m->map_len = len + delta;

    void *p = MapViewOfFile(hm, writable ? FILE_MAP_WRITE : FILE_MAP_READ,
                            (DWORD)(start >> 32), (DWORD)(start & 0xFFFFFFFF),
                            (SIZE_T)m->map_len);
    if (!p) { CloseHandle(hm); CloseHandle(hf); free(m); return 0; }
    m->map_base = p;
    m->base = (char *)p + delta;
    return m;
}

void *vmm_anon(long long len) {
    if (len <= 0) return 0;
    VmmMap *m = calloc(1, sizeof *m);
    if (!m) return 0;
    void *p = VirtualAlloc(NULL, (SIZE_T)len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!p) { free(m); return 0; }
    m->base = m->map_base = p;
    m->len = m->map_len = len;
    m->writable = 1;
    m->anon = 1;
    m->hfile = INVALID_HANDLE_VALUE;
    m->hmap = NULL;
    return m;
}

/* FlushViewOfFile only reaches the filesystem cache. Durability — what
   msync(MS_SYNC) implies — needs FlushFileBuffers as a second call. */
int vmm_flush(void *h, int sync) {
    VmmMap *m = h;
    if (!m || !m->base || m->len == 0) return 0;
    if (m->anon) return 0;
    if (!FlushViewOfFile(m->base, (SIZE_T)m->len)) return -1;
    if (sync && m->hfile != INVALID_HANDLE_VALUE && !FlushFileBuffers(m->hfile)) return -1;
    return 0;
}

int vmm_advise(void *h, int advice) {
    VmmMap *m = h;
    if (!m || !m->base || m->len == 0) return 0;
    if (advice == VMM_WILLNEED) {
        VmmPfnPrefetch fn = vmm_prefetch_fn();
        if (fn) {
            VmmRange r;
            r.VirtualAddress = m->base;
            r.NumberOfBytes = (SIZE_T)m->len;
            fn(GetCurrentProcess(), 1, &r, 0);
        }
    }
    /* Everything else has no Win32 equivalent. Advice is advisory, so
       "ignored" is success — returning -1 would make every Windows caller
       look broken. */
    return 0;
}

void vmm_close(void *h) {
    VmmMap *m = h;
    if (!m) return;
    if (m->anon) {
        if (m->map_base) VirtualFree(m->map_base, 0, MEM_RELEASE);
    } else {
        if (m->map_base) UnmapViewOfFile(m->map_base);
        if (m->hmap) CloseHandle(m->hmap);
        if (m->hfile && m->hfile != INVALID_HANDLE_VALUE) CloseHandle(m->hfile);
    }
    free(m);
}

/* ----------------------------- POSIX ------------------------------------ */
#else

long long vmm_page_size(void)    { return (long long)sysconf(_SC_PAGESIZE); }
long long vmm_granularity(void)  { return vmm_page_size(); }

long long vmm_file_size(const char *path) {
    struct stat st;
    if (!path || stat(path, &st) != 0) return -1;
    return (long long)st.st_size;
}

void *vmm_open(const char *path, int writable, long long off, long long len) {
    if (!path || off < 0 || len < 0) return 0;
    long long fsz = vmm_file_size(path);
    if (fsz < 0 || off > fsz) return 0;
    if (len == 0) len = fsz - off;          /* 0 means "to end of file" */
    if (off + len > fsz) return 0;

    int fd = open(path, writable ? O_RDWR : O_RDONLY);
    if (fd < 0) return 0;

    VmmMap *m = calloc(1, sizeof *m);
    if (!m) { close(fd); return 0; }
    m->writable = writable != 0;
    m->len = len;

    if (len == 0) {   /* mmap(len == 0) is EINVAL; an empty file is still a
                         valid, empty mapping. */
        close(fd);
        return m;
    }

    long long pg    = vmm_page_size();
    long long start = off - (off % pg);
    long long delta = off - start;
    m->map_len = len + delta;

    void *p = mmap(NULL, (size_t)m->map_len,
                   writable ? (PROT_READ | PROT_WRITE) : PROT_READ,
                   writable ? MAP_SHARED : MAP_PRIVATE, fd, (off_t)start);
    close(fd);   /* the mapping keeps its own reference to the file */
    if (p == MAP_FAILED) { free(m); return 0; }
    m->map_base = p;
    m->base = (char *)p + delta;
    return m;
}

void *vmm_anon(long long len) {
    if (len <= 0) return 0;
    VmmMap *m = calloc(1, sizeof *m);
    if (!m) return 0;
    void *p = mmap(NULL, (size_t)len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { free(m); return 0; }
    m->base = m->map_base = p;
    m->len = m->map_len = len;
    m->writable = 1;
    m->anon = 1;
    return m;
}

int vmm_flush(void *h, int sync) {
    VmmMap *m = h;
    if (!m || !m->map_base || m->map_len == 0) return 0;
    if (m->anon) return 0;
    return msync(m->map_base, (size_t)m->map_len, sync ? MS_SYNC : MS_ASYNC) == 0 ? 0 : -1;
}

int vmm_advise(void *h, int advice) {
    VmmMap *m = h;
    if (!m || !m->map_base || m->map_len == 0) return 0;
    int a;
    switch (advice) {
    case VMM_RANDOM:     a = MADV_RANDOM;     break;
    case VMM_SEQUENTIAL: a = MADV_SEQUENTIAL; break;
    case VMM_WILLNEED:   a = MADV_WILLNEED;   break;
    case VMM_DONTNEED:   a = MADV_DONTNEED;   break;
    default:             a = MADV_NORMAL;     break;
    }
    return madvise(m->map_base, (size_t)m->map_len, a) == 0 ? 0 : -1;
}

void vmm_close(void *h) {
    VmmMap *m = h;
    if (!m) return;
    if (m->map_base && m->map_len > 0) munmap(m->map_base, (size_t)m->map_len);
    free(m);
}

#endif /* _WIN32 */
#endif /* VT_NO_LIBC */
