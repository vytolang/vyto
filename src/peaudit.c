/* peaudit.c — the Windows 7 import audit, `vytoc audit` and `--audit`.
 *
 * Reads each binary's import tables (the regular one and the delay-load one)
 * straight from the PE file and checks every imported function against
 * lib/vyto/win/win7_exports.txt: the exports of a real Windows 7 SP1 install,
 * dumped there by tests/win/dump_exports.c. A function missing from that list
 * is one Windows 7 does not have, and an exe importing it refuses to start
 * with "entry point not found" — at load time, before main, with nothing at
 * build time to say so. This is what makes "runs on Windows 7 and later"
 * something checked rather than hoped.
 *
 * Also fails on:
 *   - any api-ms-win-crt-* import: the Universal CRT, absent from Windows 7
 *     unless KB2999226 is installed (a mingw UCRT toolchain or a third-party
 *     DLL can pull it in)
 *   - a DLL that is neither a Windows 7 system DLL nor shipped beside the
 *     binary
 *   - an import by ordinal, which cannot be checked by name
 * A DLL shipped beside the binary (libblend2d.dll) is the app's own and is not
 * checked through its importer; audit it as a binary of its own.
 *
 * In C inside vytoc rather than a script over objdump because it has to work
 * for an installed vytoc (tests/ is not shipped) and with any --cc, including
 * toolchains that bring no mingw objdump. It replaced tests/win/pe_audit.sh
 * and keeps that script's rules and output lines exactly.
 *
 * The input is an arbitrary file, so every offset and RVA is bounds-checked
 * before it is followed; a malformed binary is a FAIL with a reason, never a
 * crash. */
#include "peaudit.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

/* ---- a set of strings (open addressing, FNV-1a) ---- */

typedef struct { char **slot; size_t cap, n; } StrSet;

static uint64_t fnv(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}

static int set_has(const StrSet *s, const char *k) {
    if (!s->cap) return 0;
    for (size_t i = fnv(k) & (s->cap - 1);; i = (i + 1) & (s->cap - 1)) {
        if (!s->slot[i]) return 0;
        if (strcmp(s->slot[i], k) == 0) return 1;
    }
}

static int set_add(StrSet *s, const char *k) {  /* 1 if newly added */
    if (set_has(s, k)) return 0;
    if ((s->n + 1) * 2 > s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 1024;
        char **ns = calloc(nc, sizeof *ns);
        if (!ns) { fprintf(stderr, "vytoc: out of memory\n"); exit(1); }
        for (size_t i = 0; i < s->cap; i++) {
            if (!s->slot[i]) continue;
            size_t j = fnv(s->slot[i]) & (nc - 1);
            while (ns[j]) j = (j + 1) & (nc - 1);
            ns[j] = s->slot[i];
        }
        free(s->slot);
        s->slot = ns;
        s->cap = nc;
    }
    size_t i = fnv(k) & (s->cap - 1);
    while (s->slot[i]) i = (i + 1) & (s->cap - 1);
    size_t kl = strlen(k) + 1;   /* no strdup: this is C99 */
    s->slot[i] = malloc(kl);
    if (!s->slot[i]) { fprintf(stderr, "vytoc: out of memory\n"); exit(1); }
    memcpy(s->slot[i], k, kl);
    s->n++;
    return 1;
}

static void set_free(StrSet *s) {
    for (size_t i = 0; i < s->cap; i++) free(s->slot[i]);
    free(s->slot);
    memset(s, 0, sizeof *s);
}

static void lower(char *s) { for (; *s; s++) *s = (char)tolower((unsigned char)*s); }

/* ---- the allowlist ---- */

static int load_exports(const char *path, StrSet *fns, StrSet *dlls) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;  /* a dump copied off Windows is CRLF */
        if (!line[0] || line[0] == '#') continue;
        char *colon = strchr(line, ':');
        if (!colon || colon == line || !colon[1]) continue;
        *colon = 0;
        lower(line);                       /* DLL names compare case-blind */
        set_add(dlls, line);
        *colon = ':';
        set_add(fns, line);
    }
    fclose(f);
    return 1;
}

/* ---- PE reading ---- */

typedef struct {
    const unsigned char *p;
    size_t len;
    const unsigned char *sec;  /* section table */
    unsigned nsec;
    int pe64;
} Pe;

static int rd16(const Pe *pe, size_t off, unsigned *v) {
    if (off > pe->len || pe->len - off < 2) return 0;
    *v = pe->p[off] | (unsigned)pe->p[off + 1] << 8;
    return 1;
}
static int rd32(const Pe *pe, size_t off, uint32_t *v) {
    if (off > pe->len || pe->len - off < 4) return 0;
    *v = (uint32_t)pe->p[off] | (uint32_t)pe->p[off + 1] << 8 |
         (uint32_t)pe->p[off + 2] << 16 | (uint32_t)pe->p[off + 3] << 24;
    return 1;
}
static int rd64(const Pe *pe, size_t off, uint64_t *v) {
    uint32_t lo, hi;
    if (!rd32(pe, off, &lo) || !rd32(pe, off + 4, &hi)) return 0;
    *v = (uint64_t)hi << 32 | lo;
    return 1;
}

/* RVA -> file offset through the section table. */
static int rva_off(const Pe *pe, uint32_t rva, size_t *off) {
    for (unsigned i = 0; i < pe->nsec; i++) {
        const unsigned char *s = pe->sec + i * 40;
        uint32_t vsz = (uint32_t)s[8] | (uint32_t)s[9] << 8 | (uint32_t)s[10] << 16 | (uint32_t)s[11] << 24;
        uint32_t va = (uint32_t)s[12] | (uint32_t)s[13] << 8 | (uint32_t)s[14] << 16 | (uint32_t)s[15] << 24;
        uint32_t rsz = (uint32_t)s[16] | (uint32_t)s[17] << 8 | (uint32_t)s[18] << 16 | (uint32_t)s[19] << 24;
        uint32_t raw = (uint32_t)s[20] | (uint32_t)s[21] << 8 | (uint32_t)s[22] << 16 | (uint32_t)s[23] << 24;
        uint32_t span = vsz > rsz ? vsz : rsz;
        if (rva >= va && rva - va < span) {
            if (rva - va >= rsz) return 0;   /* uninitialised data: nothing on disk */
            *off = (size_t)raw + (rva - va);
            return *off < pe->len;
        }
    }
    return 0;
}

/* A NUL-terminated string at an RVA, copied out (bounded). */
static int rva_str(const Pe *pe, uint32_t rva, char *out, size_t cap) {
    size_t off;
    if (!rva_off(pe, rva, &off)) return 0;
    size_t i = 0;
    while (off + i < pe->len && pe->p[off + i] && i + 1 < cap) { out[i] = (char)pe->p[off + i]; i++; }
    if (off + i >= pe->len || (pe->p[off + i] && i + 1 >= cap)) return 0;
    out[i] = 0;
    return i > 0;
}

/* Walk one thunk array (the name table), adding "dll:fn" or
   "dll:<ordinal N>" for each entry. */
static int walk_thunks(const Pe *pe, const char *dll, uint32_t rva, StrSet *imports) {
    size_t off;
    if (!rva || !rva_off(pe, rva, &off)) return 0;
    size_t step = pe->pe64 ? 8 : 4;
    for (unsigned n = 0; n < 65536; n++, off += step) {
        uint64_t t;
        if (pe->pe64) { if (!rd64(pe, off, &t)) return 0; }
        else { uint32_t t32; if (!rd32(pe, off, &t32)) return 0; t = t32; }
        if (t == 0) return 1;
        char key[600];
        uint64_t ord_flag = pe->pe64 ? 0x8000000000000000ULL : 0x80000000ULL;
        if (t & ord_flag) {
            snprintf(key, sizeof key, "%s:<ordinal %u>", dll, (unsigned)(t & 0xffff));
        } else {
            char name[512];
            if (!rva_str(pe, (uint32_t)(t & 0x7fffffff) + 2, name, sizeof name)) return 0;
            snprintf(key, sizeof key, "%s:%s", dll, name);
        }
        set_add(imports, key);
    }
    return 0;
}

/* Reads every import into `imports`. Returns NULL on success, or why the file
   could not be read as a PE image. */
static const char *read_imports(const unsigned char *p, size_t len, StrSet *imports) {
    Pe pe = { p, len, NULL, 0, 0 };
    uint32_t lfanew, sig;
    if (len < 64 || p[0] != 'M' || p[1] != 'Z') return "not a PE file (no MZ header)";
    if (!rd32(&pe, 0x3c, &lfanew) || !rd32(&pe, lfanew, &sig) || sig != 0x00004550)
        return "not a PE file (no PE signature)";
    size_t coff = (size_t)lfanew + 4;
    unsigned nsec, optsz, magic;
    if (!rd16(&pe, coff + 2, &nsec) || !rd16(&pe, coff + 16, &optsz)) return "truncated COFF header";
    size_t opt = coff + 20;
    if (!rd16(&pe, opt, &magic)) return "truncated optional header";
    if (magic == 0x20b) pe.pe64 = 1;
    else if (magic != 0x10b) return "unknown optional-header magic";
    size_t sect = opt + optsz;
    if (sect > len || (len - sect) / 40 < nsec) return "truncated section table";
    pe.sec = p + sect;
    pe.nsec = nsec;
    uint32_t ndirs;
    size_t dirs = opt + (pe.pe64 ? 112 : 96);
    if (!rd32(&pe, opt + (pe.pe64 ? 108 : 92), &ndirs)) return "truncated optional header";

    /* data directory 1: the import table, 20-byte descriptors */
    uint32_t irva = 0, dlrva = 0;
    if (ndirs > 1 && !rd32(&pe, dirs + 1 * 8, &irva)) return "truncated data directories";
    if (ndirs > 13 && !rd32(&pe, dirs + 13 * 8, &dlrva)) return "truncated data directories";
    if (irva) {
        size_t off;
        if (!rva_off(&pe, irva, &off)) return "import table outside the file";
        for (unsigned i = 0; i < 4096; i++, off += 20) {
            uint32_t oft, name, ft;
            if (!rd32(&pe, off, &oft) || !rd32(&pe, off + 12, &name) || !rd32(&pe, off + 16, &ft))
                return "truncated import table";
            if (!oft && !name && !ft) break;
            char dll[260];
            if (!rva_str(&pe, name, dll, sizeof dll)) return "unreadable import DLL name";
            lower(dll);
            if (!walk_thunks(&pe, dll, oft ? oft : ft, imports)) return "unreadable import thunks";
        }
    }
    /* data directory 13: delay-load imports, 32-byte descriptors (RVA form) */
    if (dlrva) {
        size_t off;
        if (!rva_off(&pe, dlrva, &off)) return "delay-import table outside the file";
        for (unsigned i = 0; i < 4096; i++, off += 32) {
            uint32_t attr, name, int_rva;
            if (!rd32(&pe, off, &attr) || !rd32(&pe, off + 4, &name) || !rd32(&pe, off + 16, &int_rva))
                return "truncated delay-import table";
            if (!name) break;
            if (!(attr & 1)) return "delay-import table uses the obsolete VA form";
            char dll[260];
            if (!rva_str(&pe, name, dll, sizeof dll)) return "unreadable delay-import DLL name";
            lower(dll);
            if (!walk_thunks(&pe, dll, int_rva, imports)) return "unreadable delay-import thunks";
        }
    }
    return NULL;
}

static unsigned char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 20, n = 0;
    unsigned char *b = malloc(cap);
    while (b) {
        size_t r = fread(b + n, 1, cap - n, f);
        n += r;
        if (r == 0) break;
        if (n == cap) {
            unsigned char *nb = realloc(b, cap *= 2);
            if (!nb) { free(b); b = NULL; break; }
            b = nb;
        }
    }
    fclose(f);
    *len = n;
    return b;
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* DLLs shipped beside the binary, lowercased. */
static void shipped_dlls(const char *bin, StrSet *out) {
    char dir[4096];
    const char *s = strrchr(bin, '/');
    if (s) {
        size_t n = (size_t)(s - bin);
        if (n >= sizeof dir) return;
        memcpy(dir, bin, n);
        dir[n] = 0;
        if (!n) strcpy(dir, "/");
    } else {
        strcpy(dir, ".");
    }
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        char nm[512];
        snprintf(nm, sizeof nm, "%s", de->d_name);
        lower(nm);
        size_t l = strlen(nm);
        if (l > 4 && strcmp(nm + l - 4, ".dll") == 0) set_add(out, nm);
    }
    closedir(d);
}

int pe_audit(const char *exports, int nfiles, const char **files) {
    StrSet fns = {0}, dlls = {0};
    if (!load_exports(exports, &fns, &dlls) || !fns.n) {
        fprintf(stderr, "vytoc: cannot read the Windows 7 export list %s\n", exports);
        return 2;
    }
    int status = 0;
    for (int fi = 0; fi < nfiles; fi++) {
        const char *bin = files[fi];
        const char *name = base_name(bin);
        size_t len = 0;
        unsigned char *data = slurp(bin, &len);
        if (!data) {
            printf("FAIL pe_audit %s (no such file)\n", bin);
            status = 1;
            continue;
        }
        StrSet imports = {0};
        const char *bad = read_imports(data, len, &imports);
        free(data);
        if (bad) {
            printf("  %s: %s\n", name, bad);
            printf("FAIL pe_audit %s\n", name);
            set_free(&imports);
            status = 1;
            continue;
        }
        /* sorted, so the problem lines come out in a stable order */
        char **list = malloc(sizeof *list * (imports.n ? imports.n : 1));
        size_t n = 0;
        for (size_t i = 0; i < imports.cap; i++)
            if (imports.slot[i]) list[n++] = imports.slot[i];
        qsort(list, n, sizeof *list, cmp_str);

        StrSet shipped = {0}, reported = {0};
        shipped_dlls(bin, &shipped);
        int problems = 0;
        for (size_t i = 0; i < n; i++) {
            char dll[600];
            snprintf(dll, sizeof dll, "%s", list[i]);
            char *colon = strchr(dll, ':');
            if (!colon) continue;
            *colon = 0;
            const char *fn = colon + 1;
            if (set_has(&shipped, dll) || set_has(&reported, dll)) continue;
            if (strncmp(dll, "api-ms-win-crt-", 15) == 0) {
                printf("  %s: imports the Universal CRT (%s), which Windows 7 lacks without KB2999226\n",
                       name, dll);
                set_add(&reported, dll);
                problems = 1;
                continue;
            }
            if (!set_has(&dlls, dll)) {
                printf("  %s: %s is not a Windows 7 system DLL and is not shipped beside the binary\n",
                       name, dll);
                set_add(&reported, dll);
                problems = 1;
                continue;
            }
            if (strncmp(fn, "<ordinal", 8) == 0) {
                printf("  %s: %s imported by ordinal (%s), cannot be checked by name\n", name, dll, fn);
                problems = 1;
                continue;
            }
            if (!set_has(&fns, list[i])) {
                printf("  %s: %s!%s does not exist on Windows 7\n", name, dll, fn);
                problems = 1;
            }
        }
        if (!problems) printf("PASS pe_audit %s (%zu imports)\n", name, n);
        else { printf("FAIL pe_audit %s\n", name); status = 1; }
        free(list);
        set_free(&imports);
        set_free(&shipped);
        set_free(&reported);
    }
    set_free(&fns);
    set_free(&dlls);
    fflush(stdout);
    return status;
}
