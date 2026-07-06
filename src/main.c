#define _POSIX_C_SOURCE 200809L

#include "check.h"
#include "emit.h"
#include "parse.h"

#include <libgen.h>
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

static void resolve_imports(Module *m, const char *basedir) {
    for (int i = 0; i < m->ndecls; i++) {
        Decl *d = m->decls[i];
        if (d->kind != D_IMPORT) continue;
        const char *path = arena_printf(&g_arena, "%s/%s.vt", basedir, d->import_path);
        d->import_module = load_module(path);
    }
}

static Module *load_module(const char *path) {
    const char *ipath = intern(path);
    for (Module *m = g_modules; m; m = m->next)
        if (m->path == ipath) return m;
    char *src = read_file(path, NULL);
    if (!src) fatal("cannot open %s", path);
    Module *m = parse_module(path, sanitize(stem_of(path)), src);
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

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

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

static void usage(void) {
    fprintf(stderr,
            "voltc — the Volt compiler\n"
            "usage:\n"
            "  voltc build <file.vt> [-o out] [--release] [--verbose]\n"
            "  voltc run   <file.vt> [--release] [--verbose] [-- args...]\n");
    exit(2);
}

int main(int argc, char **argv) {
    if (argc < 3) usage();
    const char *cmd = argv[1];
    const char *input = argv[2];
    bool release = false, verbose = false;
    const char *outpath = NULL;
    int prog_argc = 0;
    char **prog_argv = NULL;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--release") == 0) release = true;
        else if (strcmp(argv[i], "--verbose") == 0) verbose = true;
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) outpath = argv[++i];
        else if (strcmp(argv[i], "--") == 0) { prog_argc = argc - i - 1; prog_argv = argv + i + 1; break; }
        else usage();
    }
    bool do_run = strcmp(cmd, "run") == 0;
    if (!do_run && strcmp(cmd, "build") != 0) usage();

    /* ---- front end ---- */
    Module *entry = load_module(input);
    check_all(g_modules, entry);

    /* ---- emit C into the cache dir ---- */
    const char *cache = arena_printf(&g_arena, "%s/.volt-cache", dir_of(input));
    mkdir(cache, 0755);

    const char *rtdir = find_runtime_dir();
    const char *cc = have_tcc() && !release ? "tcc" : "cc";
    const char *opt = release ? "-O2" : (strcmp(cc, "tcc") == 0 ? "" : "-O0");

    SBuf objs;
    sb_init(&objs);
    bool relink = false;

    /* runtime object */
    const char *rt_o = arena_printf(&g_arena, "%s/volt_rt_%s%s.o", cache, cc, release ? "_rel" : "");
    if (!file_exists(rt_o)) {
        char *c = arena_printf(&g_arena, "%s %s -w -c -o %s %s/volt_rt.c", cc, opt, rt_o, rtdir);
        if (run_cmd(c, verbose) != 0) fatal("runtime compilation failed");
        relink = true;
    }
    sb_printf(&objs, " %s", rt_o);

    for (Module *m = g_modules; m; m = m->next) {
        SBuf h, c;
        sb_init(&h);
        sb_init(&c);
        emit_module(m, m == entry, &h, &c);
        const char *hpath = arena_printf(&g_arena, "%s/mod_%s.h", cache, m->name);
        const char *cpath = arena_printf(&g_arena, "%s/mod_%s.c", cache, m->name);
        bool hchanged = false, cchanged = false;
        if (!write_if_changed(hpath, h.data, h.len, &hchanged) ||
            !write_if_changed(cpath, c.data, c.len, &cchanged))
            fatal("cannot write %s", cpath);
        sb_free(&h);
        sb_free(&c);
        const char *opath = arena_printf(&g_arena, "%s/mod_%s%s.o", cache, m->name,
                                         release ? "_rel" : "");
        /* headers are cheap to over-invalidate: recompile when any header changed */
        m->src_hash = hchanged || cchanged; /* reuse field as "dirty" for this build */
        sb_printf(&objs, " %s", opath);
        (void)opath;
    }
    /* if any header changed, all modules recompile (imports are rare and builds are fast) */
    bool any_h_changed = false;
    for (Module *m = g_modules; m; m = m->next)
        if (m->src_hash) any_h_changed = true;

    for (Module *m = g_modules; m; m = m->next) {
        const char *cpath = arena_printf(&g_arena, "%s/mod_%s.c", cache, m->name);
        const char *opath = arena_printf(&g_arena, "%s/mod_%s%s.o", cache, m->name,
                                         release ? "_rel" : "");
        if (m->src_hash || any_h_changed || !file_exists(opath)) {
            char *cmdline = arena_printf(&g_arena, "%s %s -w -I%s -I%s -c -o %s %s", cc, opt,
                                         rtdir, cache, opath, cpath);
            if (run_cmd(cmdline, verbose) != 0) fatal("C compilation of module '%s' failed", m->name);
            relink = true;
        }
    }

    /* ---- link ---- */
    const char *exe = outpath ? outpath
                              : arena_printf(&g_arena, "%s/%s", cache, stem_of(input));
    SBuf libs;
    sb_init(&libs);
    for (Module *m = g_modules; m; m = m->next)
        for (int i = 0; i < m->ndecls; i++)
            if (m->decls[i]->kind == D_LINK)
                sb_printf(&libs, " -l%s", m->decls[i]->link_lib);
    if (relink || !file_exists(exe)) {
        char *cmdline = arena_printf(&g_arena, "%s -o %s%s%s -lm", cc, exe, objs.data, libs.data);
        if (run_cmd(cmdline, verbose) != 0) fatal("link failed");
    }
    sb_free(&objs);
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
