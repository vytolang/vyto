#define _POSIX_C_SOURCE 200809L

#include "check.h"
#include "emit.h"
#include "parse.h"

#include <dirent.h>
#include <libgen.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static Module *g_modules; /* all loaded modules, entry first */

static const char *dir_of(const char *path) {
    char *copy = arena_strdup(&g_arena, path);
    return arena_strdup(&g_arena, dirname(copy));
}

static const char *stem_of(const char *path) {
    char *copy = arena_strdup(&g_arena, path);
    char *base = basename(copy);
    char *dot = strrchr(base, '.');
    if (dot) *dot = 0;
    return arena_strdup(&g_arena, base);
}

static const char *sanitize(const char *s) {
    char *out = arena_strdup(&g_arena, s);
    for (char *p = out; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9')))
            *p = '_';
    if (out[0] >= '0' && out[0] <= '9') return arena_printf(&g_arena, "m%s", out);
    return out;
}

static Module *load_module(const char *path);

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* Locate the stdlib directory: $VOLT_HOME/lib, else lib/ next to the voltc
   executable (or one level up, matching the runtime search). Cached; NULL
   when no lib directory exists. Canonicalized for prefix checks. */
static const char *find_lib_dir(void) {
    static bool cached = false;
    static const char *libdir;
    if (cached) return libdir;
    cached = true;
    char real[PATH_MAX];
    const char *env = getenv("VOLT_HOME");
    if (env) {
        const char *p = arena_printf(&g_arena, "%s/lib", env);
        if (file_exists(p) && realpath(p, real)) return libdir = arena_strdup(&g_arena, real);
    }
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = 0;
        const char *d = dir_of(exe);
        const char *cands[] = {
            arena_printf(&g_arena, "%s/lib", d),
            arena_printf(&g_arena, "%s/../lib", d),
        };
        for (size_t i = 0; i < 2; i++)
            if (file_exists(cands[i]) && realpath(cands[i], real))
                return libdir = arena_strdup(&g_arena, real);
    }
    return libdir = NULL;
}

/* try <dir>/<imp>.vt then the package layout <dir>/<imp>/<leaf>.vt */
static const char *probe_import(const char *dir, const char *imp) {
    const char *p = arena_printf(&g_arena, "%s/%s.vt", dir, imp);
    if (file_exists(p)) return p;
    char *copy = arena_strdup(&g_arena, imp);
    const char *base = basename(copy);
    p = arena_printf(&g_arena, "%s/%s/%s.vt", dir, imp, base);
    if (file_exists(p)) return p;
    return NULL;
}

static void resolve_imports(Module *m, const char *basedir) {
    for (int i = 0; i < m->ndecls; i++) {
        Decl *d = m->decls[i];
        if (d->kind != D_IMPORT) continue;
        const char *path = probe_import(basedir, d->import_path);
        if (!path) {
            const char *lib = find_lib_dir();
            if (lib) path = probe_import(lib, d->import_path);
            if (!path)
                fatal_at(d->loc, "cannot resolve import \"%s\" (searched %s and %s)",
                         d->import_path, basedir,
                         lib ? lib : "the stdlib — none found; set VOLT_HOME");
        }
        d->import_module = load_module(path);
    }
}

/* Module name for a stdlib file: the lib-relative path, sanitized, with the
   package layout's duplicate leaf collapsed (volt/ui/ui.vt -> volt_ui). This
   keeps lib module names collision-proof against user module stems. */
static const char *lib_module_name(const char *libdir, const char *canon) {
    const char *rel = canon + strlen(libdir);
    while (*rel == '/') rel++;
    char *p = arena_strdup(&g_arena, rel);
    char *dot = strrchr(p, '.');
    if (dot) *dot = 0;
    char *slash = strrchr(p, '/');
    if (slash) {
        const char *leaf = slash + 1;
        char *prev = slash;
        while (prev > p && prev[-1] != '/') prev--;
        size_t dirlen = (size_t)(slash - prev);
        if (strlen(leaf) == dirlen && strncmp(prev, leaf, dirlen) == 0) *slash = 0;
    }
    return sanitize(p);
}

static Module *load_module(const char *path) {
    char real[PATH_MAX];
    if (realpath(path, real)) path = arena_strdup(&g_arena, real);
    const char *ipath = intern(path);
    for (Module *m = g_modules; m; m = m->next)
        if (m->path == ipath) return m;
    char *src = read_file(path, NULL);
    if (!src) fatal("cannot open %s", path);
    const char *lib = find_lib_dir();
    const char *mname;
    if (lib && strncmp(path, lib, strlen(lib)) == 0 && path[strlen(lib)] == '/')
        mname = lib_module_name(lib, path);
    else
        mname = sanitize(stem_of(path));
    Module *m = parse_module(path, mname, src);
    /* duplicate module names (different dirs, same stem) would collide in C */
    for (Module *o = g_modules; o; o = o->next)
        if (strcmp(o->name, m->name) == 0)
            fatal("two modules share the name '%s' (%s, %s)", m->name, o->path, m->path);
    m->next = g_modules;
    g_modules = m;
    resolve_imports(m, dir_of(path));
    return m;
}

/* ---- files & processes ---- */

static bool write_if_changed(const char *path, const char *data, size_t len, bool *changed) {
    size_t old_len = 0;
    char *old = read_file(path, &old_len);
    if (old && old_len == len && memcmp(old, data, len) == 0) {
        *changed = false;
        return true;
    }
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fwrite(data, 1, len, f);
    fclose(f);
    *changed = true;
    return true;
}

static int run_cmd(const char *cmd, bool verbose) {
    if (verbose) fprintf(stderr, "+ %s\n", cmd);
    int rc = system(cmd);
    if (rc == -1) return -1;
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : 128;
}

/* Locate the runtime directory relative to the voltc executable. */
static const char *find_runtime_dir(void) {
    const char *env = getenv("VOLT_HOME");
    if (env) {
        const char *p = arena_printf(&g_arena, "%s/runtime", env);
        if (file_exists(arena_printf(&g_arena, "%s/volt_rt.c", p))) return p;
    }
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = 0;
        const char *d = dir_of(exe);
        const char *cands[] = {
            arena_printf(&g_arena, "%s/runtime", d),
            arena_printf(&g_arena, "%s/../runtime", d),
        };
        for (size_t i = 0; i < 2; i++)
            if (file_exists(arena_printf(&g_arena, "%s/volt_rt.c", cands[i]))) return cands[i];
    }
    fatal("cannot locate the Volt runtime (set VOLT_HOME)");
    return NULL;
}

static bool have_tcc(void) {
    static int cached = -1;
    if (cached < 0) cached = run_cmd("tcc -v >/dev/null 2>&1", false) == 0 ? 1 : 0;
    return cached == 1;
}

/* Does this cc support -fsanitize=signed-integer-overflow and link libubsan?
   Probed once by compiling a trivial program; degrades to off if unavailable. */
static bool cc_supports_san(const char *cc) {
    static int cached = -1;
    if (cached < 0) {
        char *cmd = arena_printf(&g_arena,
            "printf 'int main(void){return 0;}' | %s -fsanitize=signed-integer-overflow "
            "-fno-sanitize-recover=all -x c - -o /dev/null 2>/dev/null", cc);
        cached = run_cmd(cmd, false) == 0 ? 1 : 0;
    }
    return cached == 1;
}

static const char *volt_triple(void) {
#if defined(_WIN32) && defined(_M_X64)
    return "windows-x64";
#elif defined(__APPLE__) && defined(__aarch64__)
    return "macos-arm64";
#elif defined(__APPLE__)
    return "macos-x64";
#elif defined(__linux__) && defined(__x86_64__)
    return "linux-x64";
#elif defined(__linux__) && defined(__aarch64__)
    return "linux-arm64";
#else
    return "unknown";
#endif
}

static long file_mtime(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_mtime : -1;
}

static bool has_suffix(const char *s, const char *suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    return ls >= lf && strcmp(s + ls - lf, suf) == 0;
}

static void usage(void) {
    fprintf(stderr,
            "voltc — the Volt compiler\n"
            "usage:\n"
            "  voltc build <file.vt> [-o out] [--release] [--bundle] [--verbose]\n"
            "              [--target <triple>] [--cc <compiler cmd>]\n"
            "              [--freestanding] [--no-float] [--no-fs]\n"
            "    --bundle       statically link prebuilt native libs + the C++ runtime\n"
            "                   into one self-contained exe (no shipped .so)\n"
            "    --freestanding no libc: route alloc/print/abort through vt_host_* hooks\n"
            "                   the embedder supplies; output is lib<name>.a, not an exe\n"
            "    --no-float     stub float-to-string (no soft-float formatter pulled in)\n"
            "    --no-fs        drop the filesystem layer (File ops become no-ops/panics)\n"
            "  voltc run   <file.vt> [--release] [--verbose] [-- args...]\n"
            "targets: linux-x64 linux-arm64 macos-x64 macos-arm64 windows-x64\n"
            "  --cc (or VOLT_CC) overrides the C compiler; put sysroots/flags inside it,\n"
            "  e.g. --cc 'zig cc -target aarch64-linux-gnu' or (freestanding)\n"
            "  --cc 'arm-none-eabi-gcc -mcpu=cortex-m4 -ffreestanding'\n");
    exit(2);
}

static const struct { const char *triple; const char *cc; } cross_cc_table[] = {
    {"linux-x64", "x86_64-linux-gnu-gcc"},
    {"linux-arm64", "aarch64-linux-gnu-gcc"},
    {"windows-x64", "x86_64-w64-mingw32-gcc"},
    {"macos-x64", NULL}, /* no conventional Linux-hosted default; require --cc */
    {"macos-arm64", NULL},
};

static bool known_triple(const char *t) {
    for (size_t i = 0; i < sizeof cross_cc_table / sizeof cross_cc_table[0]; i++)
        if (strcmp(cross_cc_table[i].triple, t) == 0) return true;
    return false;
}

static const char *default_cross_cc(const char *triple) {
    for (size_t i = 0; i < sizeof cross_cc_table / sizeof cross_cc_table[0]; i++)
        if (strcmp(cross_cc_table[i].triple, triple) == 0) return cross_cc_table[i].cc;
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 3) usage();
    const char *cmd = argv[1];
    const char *input = argv[2];
    bool release = false, verbose = false, bundle = false;
    bool freestanding = false, no_float = false, no_fs = false;
    const char *outpath = NULL, *target = NULL, *cc_override = NULL;
    int prog_argc = 0;
    char **prog_argv = NULL;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--release") == 0) release = true;
        else if (strcmp(argv[i], "--bundle") == 0) bundle = true;
        else if (strcmp(argv[i], "--verbose") == 0) verbose = true;
        else if (strcmp(argv[i], "--freestanding") == 0) freestanding = true;
        else if (strcmp(argv[i], "--no-float") == 0) no_float = true;
        else if (strcmp(argv[i], "--no-fs") == 0) no_fs = true;
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) outpath = argv[++i];
        else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) target = argv[++i];
        else if (strcmp(argv[i], "--cc") == 0 && i + 1 < argc) cc_override = argv[++i];
        else if (strcmp(argv[i], "--") == 0) { prog_argc = argc - i - 1; prog_argv = argv + i + 1; break; }
        else usage();
    }
    bool do_run = strcmp(cmd, "run") == 0;
    if (!do_run && strcmp(cmd, "build") != 0) usage();
    if (freestanding && do_run)
        fatal("freestanding builds produce a library, not a runnable executable — "
              "link libvolt.a into your firmware and flash it");
    if (!cc_override) cc_override = getenv("VOLT_CC");
    if (target && !known_triple(target))
        fatal("unknown target '%s' (see voltc --help for the list)", target);
    const char *host = volt_triple();
    const char *triple = target ? target : host;
    bool cross = strcmp(triple, host) != 0;
    bool win_target = strncmp(triple, "windows", 7) == 0;
    if (do_run && cross)
        fatal("built for %s but this host is %s — copy the binary to the target "
              "or run it under an emulator (qemu/wine)", triple, host);

    /* ---- front end ---- */
    Module *entry = load_module(input);
    check_all(g_modules, entry);

    /* ---- emit C into the cache dir (per-target subdir for explicit targets) ---- */
    const char *cache = arena_printf(&g_arena, "%s/.volt-cache", dir_of(input));
    mkdir(cache, 0755);
    if (target) {
        cache = arena_printf(&g_arena, "%s/%s", cache, target);
        mkdir(cache, 0755);
    }

    const char *rtdir = find_runtime_dir();
    if (freestanding && !cc_override)
        fatal("freestanding builds need a bare-metal toolchain — pass one with "
              "--cc or VOLT_CC (e.g. --cc 'arm-none-eabi-gcc -mcpu=cortex-m4')");
    const char *cc;
    if (cc_override) {
        cc = cc_override;
    } else if (cross || (target && win_target)) {
        cc = default_cross_cc(triple);
        if (!cc)
            fatal("no default cross compiler for %s — pass one with --cc or VOLT_CC", triple);
        char *probe = arena_printf(&g_arena, "command -v %s >/dev/null 2>&1", cc);
        if (run_cmd(probe, false) != 0)
            fatal("cross compiler '%s' not found — install it or pass --cc/VOLT_CC "
                  "(e.g. --cc 'zig cc -target ...')", cc);
    } else {
        cc = have_tcc() && !release ? "tcc" : "cc";
    }
    const char *opt = release ? "-O2" : (strcmp(cc, "tcc") == 0 ? "" : "-O0");
    /* Freestanding profile: no libc, no libc-recognized builtins (so our own
       mem/str/formatter routines can't be pattern-matched into self-calls),
       plus the opt-in sub-profiles. Injected into every runtime+module compile. */
    const char *fsflags = "";
    if (freestanding) fsflags = " -ffreestanding -DVT_NO_LIBC -fno-builtin";
    if (no_float) fsflags = arena_printf(&g_arena, "%s -DVT_NO_FLOAT", fsflags);
    if (no_fs) fsflags = arena_printf(&g_arena, "%s -DVT_NO_FS", fsflags);
    /* Debug builds also trap overflow at the C level (defense in depth beyond
       the emitter's checked arithmetic), catching overflow in native/FFI code.
       Native host compilers only; skipped for cross/tcc/freestanding (ubsan
       needs a hosted runtime) or if unsupported. */
    const char *san = "";
    if (!release && !bundle && !cross && !win_target && !freestanding &&
        strcmp(cc, "tcc") != 0 && cc_supports_san(cc))
        san = " -fsanitize=signed-integer-overflow -fno-sanitize-recover=all";
    /* Object-cache key for module .o files. A ubsan-instrumented debug object
       cannot be reused for a --bundle/release link (which omits the ubsan
       runtime), so debug+san and plain builds must not share the same path;
       the freestanding profile likewise needs its own objects. */
    const char *prof = arena_printf(&g_arena, "%s%s%s", freestanding ? "_fs" : "",
                                    no_float ? "_nf" : "", no_fs ? "_nofs" : "");
    const char *osuf = arena_printf(&g_arena, "%s%s", release ? "_rel" : (san[0] ? "_dbg" : ""),
                                    prof);

    SBuf objs;
    sb_init(&objs);
    bool relink = false;

    /* runtime object (cc name sanitized: --cc commands may contain spaces) */
    const char *rt_o = arena_printf(&g_arena, "%s/volt_rt_%s%s%s.o", cache, sanitize(cc),
                                    release ? "_rel" : "", prof);
    const char *rt_c = arena_printf(&g_arena, "%s/volt_rt.c", rtdir);
    /* builtin-method helpers are amalgamated into volt_rt.c via #include, so the
       compile unit is still volt_rt.c; track their mtimes so edits rebuild it. */
    static const char *rt_amalg[] = {"volt_rt_num.c", "volt_rt_str.c",
                                     "volt_rt_arr.c", "volt_rt_map.c"};
    long rt_newest = file_mtime(rt_c);
    long rt_h_mt = file_mtime(arena_printf(&g_arena, "%s/volt_rt.h", rtdir));
    if (rt_h_mt > rt_newest) rt_newest = rt_h_mt;
    for (size_t i = 0; i < sizeof rt_amalg / sizeof *rt_amalg; i++) {
        long t = file_mtime(arena_printf(&g_arena, "%s/%s", rtdir, rt_amalg[i]));
        if (t > rt_newest) rt_newest = t;
    }
    if (!file_exists(rt_o) || rt_newest > file_mtime(rt_o)) {
        char *c = arena_printf(&g_arena, "%s %s%s -w -c -o %s %s/volt_rt.c", cc, opt, fsflags, rt_o,
                               rtdir);
        if (run_cmd(c, verbose) != 0) fatal("runtime compilation failed");
        relink = true;
    }
    sb_printf(&objs, " %s", rt_o);

    /* ---- native packages: bundled C sources and prebuilt shared libraries ---- */
    SBuf shlibs;
    sb_init(&shlibs);
    SBuf bundle_deps; /* extra -l flags for statically bundled archives (--bundle) */
    sb_init(&bundle_deps);
    const char *copy_libs[64];
    int ncopy = 0;
    bool need_rpath = false;
    const char *seen_dirs[128];
    int nseen = 0;
    for (Module *m = g_modules; m; m = m->next) {
        const char *mdir = dir_of(m->path);
        /* several modules may live in one package dir: process it once */
        bool seen = false;
        for (int i = 0; i < nseen; i++)
            if (strcmp(seen_dirs[i], mdir) == 0) seen = true;
        if (seen) continue;
        if (nseen < 128) seen_dirs[nseen++] = mdir;

        const char *nsrc = arena_printf(&g_arena, "%s/native/src", mdir);
        DIR *dp = opendir(nsrc);
        if (dp) {
            struct dirent *de;
            while ((de = readdir(dp))) {
                if (!has_suffix(de->d_name, ".c")) continue;
                const char *csrc = arena_printf(&g_arena, "%s/%s", nsrc, de->d_name);
                const char *obj = arena_printf(&g_arena, "%s/native_%s_%s%s%s.o", cache, m->name,
                                               stem_of(de->d_name), release ? "_rel" : "", prof);
                if (!file_exists(obj) || file_mtime(csrc) > file_mtime(obj)) {
                    char *cl = arena_printf(&g_arena, "%s %s%s -w -I%s -c -o %s %s", cc,
                                            release ? "-O2" : opt, fsflags, nsrc, obj, csrc);
                    if (run_cmd(cl, verbose) != 0)
                        fatal("native source of package '%s' failed to compile", m->name);
                    relink = true;
                }
                sb_printf(&objs, " %s", obj);
            }
            closedir(dp);
        }

        const char *pdir = arena_printf(&g_arena, "%s/native/%s", mdir, triple);
        dp = opendir(pdir);
        if (dp) {
            struct dirent *de;
            bool any = false;
            while ((de = readdir(dp))) {
                bool shared = has_suffix(de->d_name, ".so") || has_suffix(de->d_name, ".dylib") ||
                              has_suffix(de->d_name, ".dll");
                bool static_lib = has_suffix(de->d_name, ".a");
                const char *lib = arena_printf(&g_arena, "%s/%s", pdir, de->d_name);
                if (bundle) {
                    /* --bundle: statically link the archive into the exe (no
                       shipped .so), and pull in any deps it declares in a
                       sidecar "<lib>.deps" file (one line of extra -l flags). */
                    if (!static_lib) continue;
                    sb_printf(&shlibs, " %s", lib);
                    char *deps = read_file(arena_printf(&g_arena, "%s.deps", lib), NULL);
                    if (deps) {
                        for (char *p = deps; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
                        sb_printf(&bundle_deps, " %s", deps);
                    }
                    any = true;
                } else {
                    if (!shared) continue;
                    sb_printf(&shlibs, " %s", lib);
                    if (ncopy < 64) copy_libs[ncopy++] = lib;
                    need_rpath = true;
                    any = true;
                }
            }
            closedir(dp);
            if (!any) fatal(bundle
                ? "package '%s' has no static library (.a) in native/%s for --bundle"
                : "package '%s' has native/%s/ but no shared library in it", m->name, triple);
        } else if (file_exists(arena_printf(&g_arena, "%s/native", mdir)) && !file_exists(nsrc)) {
            fatal("package '%s' ships native binaries but none for this platform (native/%s)",
                  m->name, triple);
        }
    }

    for (Module *m = g_modules; m; m = m->next) {
        SBuf h, c;
        sb_init(&h);
        sb_init(&c);
        emit_module(m, m == entry, !release, freestanding, &h, &c);
        const char *hpath = arena_printf(&g_arena, "%s/mod_%s.h", cache, m->name);
        const char *cpath = arena_printf(&g_arena, "%s/mod_%s.c", cache, m->name);
        bool hchanged = false, cchanged = false;
        if (!write_if_changed(hpath, h.data, h.len, &hchanged) ||
            !write_if_changed(cpath, c.data, c.len, &cchanged))
            fatal("cannot write %s", cpath);
        sb_free(&h);
        sb_free(&c);
        const char *opath = arena_printf(&g_arena, "%s/mod_%s%s.o", cache, m->name, osuf);
        /* headers are cheap to over-invalidate: recompile when any header changed */
        m->src_hash = hchanged || cchanged; /* reuse field as "dirty" for this build */
        sb_printf(&objs, " %s", opath);
        (void)opath;
    }
    /* if any header changed, all modules recompile (imports are rare and builds are fast) */
    bool any_h_changed = false;
    for (Module *m = g_modules; m; m = m->next)
        if (m->src_hash) any_h_changed = true;

    /* newest header on disk: an interrupted previous build may have left
       fresh .c/.h files with stale .o files and no "changed" flag this run */
    long latest_h = file_mtime(arena_printf(&g_arena, "%s/volt_rt.h", rtdir));
    for (Module *m = g_modules; m; m = m->next) {
        long t = file_mtime(arena_printf(&g_arena, "%s/mod_%s.h", cache, m->name));
        if (t > latest_h) latest_h = t;
    }

    for (Module *m = g_modules; m; m = m->next) {
        const char *cpath = arena_printf(&g_arena, "%s/mod_%s.c", cache, m->name);
        const char *opath = arena_printf(&g_arena, "%s/mod_%s%s.o", cache, m->name, osuf);
        if (m->src_hash || any_h_changed || !file_exists(opath) ||
            file_mtime(cpath) > file_mtime(opath) || latest_h > file_mtime(opath)) {
            char *cmdline = arena_printf(&g_arena, "%s %s%s%s -w -I%s -I%s -c -o %s %s", cc, opt,
                                         san, fsflags, rtdir, cache, opath, cpath);
            if (run_cmd(cmdline, verbose) != 0) fatal("C compilation of module '%s' failed", m->name);
            relink = true;
        }
    }

    /* ---- freestanding: archive, don't link ----
       Bare-metal has no hosted crt0/main and often no linker at this stage —
       the embedder links libvolt.a with their vt_host_* hooks, startup, and
       linker script. ar just bundles objects, so the host ar handles cross
       objects fine. No -lm, no rpath, no exe. */
    if (freestanding) {
        const char *lib = outpath ? outpath
                                  : arena_printf(&g_arena, "%s/lib%s.a", cache, stem_of(input));
        if (relink || !file_exists(lib)) {
            char *cmdline = arena_printf(&g_arena, "rm -f %s && ar rcs %s%s", lib, lib, objs.data);
            if (run_cmd(cmdline, verbose) != 0) fatal("archive failed");
        }
        if (verbose) fprintf(stderr, "wrote %s\n", lib);
        sb_free(&objs);
        sb_free(&shlibs);
        sb_free(&bundle_deps);
        return 0;
    }

    /* ---- link ---- */
    const char *exe = outpath ? outpath
                              : arena_printf(&g_arena, "%s/%s%s", cache, stem_of(input),
                                             win_target ? ".exe" : "");
    const char *rpath_flag = "";
    if (need_rpath) {
        /* $ORIGIN / @loader_path must reach the linker unexpanded (single quotes) */
        if (strncmp(triple, "linux", 5) == 0) rpath_flag = " '-Wl,-rpath,$ORIGIN'";
        else if (strncmp(triple, "macos", 5) == 0) rpath_flag = " '-Wl,-rpath,@loader_path'";
        /* windows: none — the exe directory is searched by default */
    }
    SBuf libs;
    sb_init(&libs);
    for (Module *m = g_modules; m; m = m->next)
        for (int i = 0; i < m->ndecls; i++) {
            Decl *d = m->decls[i];
            if (d->kind != D_LINK) continue;
            if (d->link_cond && strncmp(triple, d->link_cond, strlen(d->link_cond)) != 0)
                continue; /* library is for another platform */
            sb_printf(&libs, " -l%s", d->link_lib);
        }
    for (int i = 0; i < ncopy; i++)
        if (!file_exists(exe) || file_mtime(copy_libs[i]) > file_mtime(exe)) relink = true;
    if (relink || !file_exists(exe)) {
        /* --bundle: bake the static archives + their deps and the C++/GCC
           runtimes into the exe (no shipped .so). System libs (libc, libX11)
           stay dynamic — fully static X11/glibc needs archives that usually
           aren't installed and is fragile. */
        const char *bundle_flags = bundle ? " -static-libstdc++ -static-libgcc" : "";
        const char *bundle_link = bundle
            ? arena_printf(&g_arena, "%s -lstdc++", bundle_deps.data) : "";
        char *cmdline = arena_printf(&g_arena, "%s%s%s -o %s%s%s%s%s%s -lm", cc, san, bundle_flags,
                                     exe, objs.data, shlibs.data, libs.data, bundle_link, rpath_flag);
        if (run_cmd(cmdline, verbose) != 0) fatal("link failed");
    }
    /* ship prebuilt libraries next to the executable */
    const char *exedir = dir_of(exe);
    for (int i = 0; i < ncopy; i++) {
        if (strcmp(dir_of(copy_libs[i]), exedir) == 0) continue;
        char *cl = arena_printf(&g_arena, "cp -p %s %s/", copy_libs[i], exedir);
        if (run_cmd(cl, verbose) != 0) fatal("cannot copy %s next to the executable", copy_libs[i]);
    }
    sb_free(&objs);
    sb_free(&shlibs);
    sb_free(&libs);

    if (!do_run) {
        printf("%s\n", exe);
        return 0;
    }

    /* ---- run ---- */
    char **eargv = arena_alloc(&g_arena, sizeof(char *) * (size_t)(prog_argc + 2));
    eargv[0] = arena_strdup(&g_arena, exe);
    for (int i = 0; i < prog_argc; i++) eargv[i + 1] = prog_argv[i];
    eargv[prog_argc + 1] = NULL;
    execv(exe, eargv);
    /* exe may be relative without ./ */
    if (!strchr(exe, '/')) execv(arena_printf(&g_arena, "./%s", exe), eargv);
    fatal("cannot exec %s", exe);
    return 1;
}
