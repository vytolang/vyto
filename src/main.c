#define _POSIX_C_SOURCE 200809L

#include "check.h"
#include "emit.h"
#include "parse.h"
#include "peaudit.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <strings.h>
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

static bool dir_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool dir_writable(const char *path) {
    return access(path, W_OK | X_OK) == 0;
}

static int run_cmd(const char *cmd, bool echo);   /* defined below */

/* mkdir -p. Returns 0 when the directory exists afterwards. Walks the path
   creating each component, ignoring EEXIST so a concurrent build racing us is
   not an error. */
static int mkdir_p(const char *path) {
    if (dir_exists(path)) return 0;
    char *tmp = arena_strdup(&g_arena, path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) { *p = '/'; return -1; }
        *p = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return dir_exists(path) ? 0 : -1;
}

/* Locate the stdlib directory: $VYTO_HOME/lib, else lib/ next to the vytoc
   executable (or one level up, matching the runtime search). Cached; NULL
   when no lib directory exists. Canonicalized for prefix checks. */
static const char *find_lib_dir(void) {
    static bool cached = false;
    static const char *libdir;
    if (cached) return libdir;
    cached = true;
    char real[PATH_MAX];
    const char *env = getenv("VYTO_HOME");
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

/* ---- module search roots ------------------------------------------------
 *
 * Roots probed for an import, in order, after the importing module's own
 * directory. Registration order is search order:
 *
 *   1. --modpath <dir>, repeatable, in command-line order
 *   2. $VYTO_PATH, ':'-separated  (skipped entirely when any --modpath was
 *      given -- flag overrides env, as VYTO_CC does in main())
 *   3. vyto_modules/ found by walking up from the entry file's directory
 *   4. $VYTO_HOME/lib -- the stdlib, LAST, so nothing can shadow it
 *
 * A root is a directory that CONTAINS packages, exactly like $VYTO_HOME/lib
 * holds vyto/. That one rule for every source is what makes a vendored package
 * and a --modpath one produce identical module names.
 *
 * Every root is canonicalized at registration, because load_module compares a
 * realpath'd module path against it by string prefix -- a non-canonical root
 * would silently fall through to bare-stem naming. Roots are de-duplicated by
 * canonical path too, so one directory reached two ways (a --modpath that is
 * also the discovered vyto_modules/, a symlink) cannot name one file twice.
 *
 * An unusable root is dropped, never fatal: a stale $VYTO_PATH entry must not
 * break an otherwise fine build. This deliberately does NOT copy
 * find_obj_cache_dir's "explicit request we cannot honour" rule -- a path list
 * is a list of candidates, not one demand. */
#define MAX_ROOTS 64

typedef struct { const char *path; const char *why; } Root;

static Root g_roots[MAX_ROOTS];
static int g_nroots;

/* Canonicalize and append. False when the directory is unusable or already
   registered. `why` labels the root in diagnostics only. */
static bool add_root(const char *dir, const char *why) {
    if (!dir || !*dir || g_nroots >= MAX_ROOTS) return false;
    char real[PATH_MAX];
    if (!dir_exists(dir) || !realpath(dir, real)) return false;
    const char *canon = arena_strdup(&g_arena, real);
    for (int i = 0; i < g_nroots; i++)
        if (strcmp(g_roots[i].path, canon) == 0) return false;
    g_roots[g_nroots].path = canon;
    g_roots[g_nroots].why = why;
    g_nroots++;
    return true;
}

/* Register every vyto_modules/ from `start_dir` up to /, nearest first, so a
   subproject's own packages win over the enclosing project's.

   Done ONCE, from the entry file's directory, not per importing module: a
   module name must be a function of the file, not of who imported it first.
   Per-module roots would let a package deep in vyto_modules/ see its own
   nested vyto_modules/ and name a file against a root the entry never had,
   making the name depend on import order.

   The walk is over the canonical start dir, so ".." and symlinks in argv[2]
   cannot send it somewhere surprising. It stops at "/" with no marker file: a
   marker would be a second invisible rule that a bare directory of .vt files
   could not satisfy. */
static void add_vyto_modules_roots(const char *start_dir) {
    char real[PATH_MAX];
    if (!realpath(start_dir, real)) return;
    char *d = arena_strdup(&g_arena, real);
    for (;;) {
        add_root(arena_printf(&g_arena, "%s/vyto_modules", d), "vyto_modules");
        if (d[0] == '/' && d[1] == 0) break;
        char *slash = strrchr(d, '/');
        if (!slash) break;
        if (slash == d) d[1] = 0; else *slash = 0;
    }
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
        /* the importing file's own directory first, so a local module always
           wins; then every registered root, the stdlib among them and last */
        const char *path = probe_import(basedir, d->import_path);
        for (int r = 0; !path && r < g_nroots; r++)
            path = probe_import(g_roots[r].path, d->import_path);
        if (!path) {
            /* one root per line: with four sources of roots a comma list is
               unreadable. The colon ends line 1 because tests/run_tests.sh
               compares only the first diagnostic line. */
            SBuf s;
            sb_init(&s);
            sb_printf(&s, "\n  %s (the importing file's directory)", basedir);
            for (int r = 0; r < g_nroots; r++)
                sb_printf(&s, "\n  %s (%s)", g_roots[r].path, g_roots[r].why);
            if (!find_lib_dir())
                sb_printf(&s, "\n  (no stdlib found — set VYTO_HOME)");
            fatal_at(d->loc, "cannot resolve import \"%s\"; searched:%s",
                     d->import_path, s.data);
        }
        d->import_module = load_module(path);
    }
}

/* Module name for a file under a search root: the root-relative path,
   sanitized, with the package layout's duplicate leaf collapsed
   (lib/vyto/ui/ui.vt -> vyto_ui, vyto_modules/vytoweb/router/router.vt ->
   vytoweb_router). Because the package directory is always a component, two
   packages may each carry a config.vt and both still load.

   The collapse only fires when the leaf equals its parent directory, so
   vyto-kv/src/store.vt is vyto_kv_src_store, not vyto_kv_store. Stripping a
   package's source root is a manifest concept ("modules": "src") and belongs
   with a manifest reader, not as a directory name hardcoded here.

   CONTRACT: `canon` must be under `root` -- the caller verifies the prefix,
   and `canon + strlen(root)` is unchecked. */
static const char *root_module_name(const char *root, const char *canon) {
    const char *rel = canon + strlen(root);
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

/* ---- shared object cache ------------------------------------------------
 *
 * Objects compiled from files that do not depend on the program being built --
 * the runtime, and every native package's C -- are content-addressed and shared
 * across every entry-file directory on the machine. Without this, each
 * directory that runs a program gets its own .vyto-cache and recompiles the
 * same translation units: vyto/regex vendors 30 of them, so examples/ and
 * tests/fixtures/ were each holding a byte-identical copy.
 *
 * Emitted Vyto C (mod_*.c) is deliberately NOT shared. A generic instantiation
 * is attached to the module that declared the generic (check.c, instantiate_fn),
 * so mod_vyto_util_sort.c contains sort_by__int or sort_by__MyType depending on
 * what the *program* asked for. Those objects stay in the local cache.
 */

/* Where shared objects live. In order: $VYTO_OBJ_CACHE (or "off" to disable),
 * the repo root beside the stdlib, then the user cache dir. Never fatal --
 * a build that cannot find anywhere to share simply keeps its objects local. */
static const char *find_obj_cache_dir(void) {
    static bool cached = false;
    static const char *dir;
    if (cached) return dir;
    cached = true;

    const char *env = getenv("VYTO_OBJ_CACHE");
    if (env && strcmp(env, "off") == 0) return dir = NULL;
    if (env && *env) {
        if (mkdir_p(env) == 0 && dir_writable(env)) return dir = arena_strdup(&g_arena, env);
        return dir = NULL;              /* explicit request we cannot honour */
    }

    /* find_lib_dir() canonicalizes to <root>/lib, so its parent is the repo
       root in a checkout -- no separate root discovery, no git dependency. */
    const char *lib = find_lib_dir();
    if (lib) {
        const char *p = arena_printf(&g_arena, "%s/.vyto-cache/obj", dir_of(lib));
        if (mkdir_p(p) == 0 && dir_writable(p)) return dir = p;
    }

    /* Installed vytoc: the stdlib sits somewhere unwritable like /usr/local. */
    const char *xdg = getenv("XDG_CACHE_HOME");
    const char *home = getenv("HOME");
    const char *p = NULL;
    if (xdg && *xdg) p = arena_printf(&g_arena, "%s/vyto/obj", xdg);
    else if (home && *home) p = arena_printf(&g_arena, "%s/.cache/vyto/obj", home);
    if (p && mkdir_p(p) == 0 && dir_writable(p)) return dir = p;

    return dir = NULL;
}

/* Digest every file directly in `d` (name and bytes, in readdir order made
 * deterministic by sorting) into `h`.
 *
 * Hashing the whole directory rather than just the one translation unit is the
 * point: an object depends on the headers it includes, so keying on
 * vregex_compile.c alone would serve a stale object after an edit to
 * pcre2_internal.h. This is the content-addressed replacement for the
 * newest-mtime scan that used to guard these objects.
 *
 * It does not cover headers from outside the directory (system headers, a
 * per-triple include dir). Neither did the mtime rule, so this is not a
 * regression -- but it is why the -I flags stay part of the key. */
/* Bits reported by dir_digest: which baked-in directory macros the package's
   sources actually mention. */
#define USES_LIBDIR 1u
#define USES_APPDIR 2u

static uint64_t dir_digest_at(const char *d, int depth, unsigned *uses) {
    if (depth > 8) return 0;               /* guard against a symlink cycle */
    char *names[1024];
    int n = 0;
    DIR *dp = opendir(d);
    if (!dp) return 0;
    struct dirent *de;
    while ((de = readdir(dp)) && n < 1024) {
        if (de->d_name[0] == '.') continue;
        names[n++] = arena_strdup(&g_arena, de->d_name);
    }
    closedir(dp);
    for (int i = 1; i < n; i++) {          /* insertion sort: n is small */
        char *k = names[i];
        int j = i - 1;
        while (j >= 0 && strcmp(names[j], k) > 0) { names[j + 1] = names[j]; j--; }
        names[j + 1] = k;
    }
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < n; i++) {
        const char *p = arena_printf(&g_arena, "%s/%s", d, names[i]);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        h ^= fnv1a(names[i], strlen(names[i]));
        h *= 1099511628211ULL;
        if (S_ISDIR(st.st_mode)) {
            /* Recurse. The flat glob does not compile subdirectories, but their
               contents are still inputs: vyto/regex keeps the whole vendored
               PCRE2 tree under native/src/pcre2/ and includes into it, so an
               edit there must invalidate the objects. The mtime rule this
               replaces missed that -- a directory's mtime does not change when
               a file inside it is edited. */
            h ^= dir_digest_at(p, depth + 1, uses);
            h *= 1099511628211ULL;
            continue;
        }
        /* Regular files only. fopen() on anything else (a fifo, a device)
           either blocks or reports a nonsense size to read_file. */
        if (!S_ISREG(st.st_mode)) continue;
        size_t len = 0;
        char *src = read_file(p, &len);
        if (!src) continue;
        h ^= fnv1a(src, len);
        h *= 1099511628211ULL;
        /* read_file NUL-terminates, so strstr is safe on source text. */
        if (uses) {
            if (strstr(src, "VYTO_LIBDIR")) *uses |= USES_LIBDIR;
            if (strstr(src, "VYTO_APPDIR")) *uses |= USES_APPDIR;
        }
    }
    return h;
}

static uint64_t dir_digest(const char *d, unsigned *uses) {
    if (uses) *uses = 0;
    return dir_digest_at(d, 0, uses);
}

/* Compile `csrc` to a shared object and return its path, or NULL when there is
 * no shared cache and the caller should fall back to its local one.
 *
 * `cmd_head` is everything on the compile line before `-c -o <out> <src>`: the
 * compiler, the optimisation level, the sanitizer and freestanding flags, every
 * -D (including -DVYTO_APPDIR, which is why vyto/os's shim correctly gets one
 * entry per app while every other package shares one) and every -I. Hashing it
 * therefore keys the object by target triple and build profile as well, and
 * nothing else needs adding.
 *
 * It is taken as a plain string rather than a printf format on purpose: a path
 * containing a '%' would otherwise be re-interpreted as a conversion.
 *
 * Writes via a pid-suffixed temporary and rename(), which is atomic within a
 * directory: a concurrent build never observes a half-written object, and two
 * builds racing on the same key simply both compile and one wins. */
static const char *cached_object(const char *csrc, uint64_t inputs, const char *cmd_head,
                                 const char *what, bool verbose, bool *compiled) {
    const char *root = find_obj_cache_dir();
    if (!root) return NULL;            /* NULL means "no shared cache", nothing else */

    uint64_t h = inputs;
    h ^= fnv1a(cmd_head, strlen(cmd_head));
    h *= 1099511628211ULL;
    h ^= fnv1a(csrc, strlen(csrc));
    h *= 1099511628211ULL;

    const char *shard = arena_printf(&g_arena, "%s/%02x", root, (unsigned)(h >> 56) & 0xff);
    mkdir(shard, 0755);
    const char *obj = arena_printf(&g_arena, "%s/%016llx.o", shard, (unsigned long long)h);
    if (file_exists(obj)) return obj;

    const char *tmp = arena_printf(&g_arena, "%s/%016llx.o.tmp.%d", shard,
                                   (unsigned long long)h, (int)getpid());
    char *cmd = arena_printf(&g_arena, "%s -c -o %s %s", cmd_head, tmp, csrc);
    if (run_cmd(cmd, verbose) != 0) {
        unlink(tmp);
        fatal("%s failed to compile", what);
    }
    /* Same directory, so this is atomic. A failure here is the filesystem
       (full, or permissions changed mid-build), not the compile -- say so
       rather than blaming the source. */
    if (rename(tmp, obj) != 0) {
        unlink(tmp);
        fatal("cannot write to the object cache at %s", root);
    }

    /* Sidecar so the cache can be read by a human: which source, which command.
       `grep -rl pcre2 .vyto-cache/obj` answers "what is this object". */
    const char *side = arena_printf(&g_arena, "%s/%016llx.txt", shard, (unsigned long long)h);
    FILE *f = fopen(side, "w");
    if (f) {
        fprintf(f, "%s\n%s -c -o %s %s\n", csrc, cmd_head, obj, csrc);
        fclose(f);
    }
    if (compiled) *compiled = true;
    return obj;
}

/* ---- parallel native compiles ----
 *
 * Package shims used to compile one at a time through system(). That was fine
 * for a handful of files, and wrong once a package vendored a library: vyto/net
 * builds curl + mbedTLS (~270 TUs) for windows-x64, and serially a cold build
 * took about two minutes. Shim objects are therefore queued and compiled
 * VYTO_JOBS at a time (default: the online CPU count) just before the link.
 *
 * A queued job compiles to a private temporary and is renamed into place only
 * after it succeeded, so the cache never holds a half-written object — the same
 * guarantee cached_object() gives inline. */
typedef struct {
    const char *cmd;   /* full compile command, writing to tmp */
    const char *tmp;   /* where the compiler writes */
    const char *obj;   /* where the object ends up */
    const char *side;  /* cache sidecar to write on success, or NULL */
    const char *src;   /* for the sidecar and the error message */
    const char *what;  /* "native source of package 'x'" */
    pid_t pid;
    int status;
} CJob;
static CJob *g_cjobs;
static int g_ncjobs, g_capcjobs;

static bool cjob_queued(const char *obj) {
    for (int i = 0; i < g_ncjobs; i++)
        if (strcmp(g_cjobs[i].obj, obj) == 0) return true;
    return false;
}

static void cjob_add(const char *cmd_head, const char *src, const char *obj, const char *side,
                     const char *what) {
    if (cjob_queued(obj)) return;   /* identical source and flags in two packages */
    if (g_ncjobs == g_capcjobs) {
        g_capcjobs = g_capcjobs ? g_capcjobs * 2 : 64;
        g_cjobs = realloc(g_cjobs, sizeof *g_cjobs * (size_t)g_capcjobs);
        if (!g_cjobs) fatal("out of memory");
    }
    CJob *j = &g_cjobs[g_ncjobs];
    j->tmp = arena_printf(&g_arena, "%s.tmp.%d.%d", obj, (int)getpid(), g_ncjobs);
    j->cmd = arena_printf(&g_arena, "%s -c -o %s %s", cmd_head, j->tmp, src);
    j->obj = obj;
    j->side = side;
    j->src = src;
    j->what = what;
    j->pid = 0;
    j->status = -1;
    g_ncjobs++;
}

static int compile_jobs_limit(void) {
    const char *e = getenv("VYTO_JOBS");
    if (e && *e) {
        int n = atoi(e);
        if (n >= 1) return n;
    }
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n >= 1 ? (int)n : 1;
}

/* Run every queued compile, at most VYTO_JOBS at once. Returns only when all
   have finished; a failure is reported after the rest complete, so a broken
   file does not leave sibling compilers running behind a fatal(). */
static void run_compile_jobs(bool verbose) {
    if (g_ncjobs == 0) return;
    int limit = compile_jobs_limit();
    int next = 0, running = 0, failed = -1;
    while (next < g_ncjobs || running > 0) {
        while (running < limit && next < g_ncjobs && failed < 0) {
            CJob *j = &g_cjobs[next++];
            if (verbose) fprintf(stderr, "+ %s\n", j->cmd);
            fflush(NULL);
            pid_t pid = fork();
            if (pid < 0) fatal("cannot fork a compiler: %s", strerror(errno));
            if (pid == 0) {
                execl("/bin/sh", "sh", "-c", j->cmd, (char *)NULL);
                _exit(127);
            }
            j->pid = pid;
            running++;
        }
        if (running == 0) break;   /* a failure stopped new starts and all are done */
        int st;
        pid_t pid = wait(&st);
        if (pid < 0) {
            if (errno == EINTR) continue;
            fatal("lost track of a compiler: %s", strerror(errno));
        }
        for (int i = 0; i < next; i++) {
            if (g_cjobs[i].pid != pid) continue;
            g_cjobs[i].pid = 0;
            g_cjobs[i].status = WIFEXITED(st) ? WEXITSTATUS(st) : 128;
            if (g_cjobs[i].status != 0 && failed < 0) failed = i;
            running--;
            break;
        }
    }
    for (int i = 0; i < next; i++) {
        CJob *j = &g_cjobs[i];
        if (j->status != 0) { unlink(j->tmp); continue; }
        if (failed >= 0) { unlink(j->tmp); continue; }
        /* Same directory, so this is atomic. */
        if (rename(j->tmp, j->obj) != 0) {
            unlink(j->tmp);
            fatal("cannot write %s", j->obj);
        }
        if (j->side) {
            FILE *f = fopen(j->side, "w");
            if (f) {
                fprintf(f, "%s\n%s\n", j->src, j->cmd);
                fclose(f);
            }
        }
    }
    if (failed >= 0) fatal("%s failed to compile", g_cjobs[failed].what);
    g_ncjobs = 0;
}

/* cached_object(), but queued: returns the object's final path at once and
   adds a compile job when it is not in the cache yet. NULL means "no shared
   cache", exactly as there. */
static const char *cached_object_queued(const char *csrc, uint64_t inputs, const char *cmd_head,
                                        const char *what, bool *compiled) {
    const char *root = find_obj_cache_dir();
    if (!root) return NULL;
    uint64_t h = inputs;
    h ^= fnv1a(cmd_head, strlen(cmd_head));
    h *= 1099511628211ULL;
    h ^= fnv1a(csrc, strlen(csrc));
    h *= 1099511628211ULL;
    const char *shard = arena_printf(&g_arena, "%s/%02x", root, (unsigned)(h >> 56) & 0xff);
    mkdir(shard, 0755);
    const char *obj = arena_printf(&g_arena, "%s/%016llx.o", shard, (unsigned long long)h);
    if (file_exists(obj)) return obj;
    const char *side = arena_printf(&g_arena, "%s/%016llx.txt", shard, (unsigned long long)h);
    cjob_add(cmd_head, csrc, obj, side, what);
    if (compiled) *compiled = true;
    return obj;
}

static Module *load_module(const char *path) {
    char real[PATH_MAX];
    if (realpath(path, real)) path = arena_strdup(&g_arena, real);
    const char *ipath = intern(path);
    for (Module *m = g_modules; m; m = m->next)
        if (m->path == ipath) return m;
    char *src = read_file(path, NULL);
    if (!src) fatal("cannot open %s", path);
    /* A file under a search root is named by its root-relative path; one that
       is under none keeps its bare stem. First match wins, so an explicitly
       given --modpath nested inside another root takes precedence -- it
       registered earlier. `path` is canonical (realpath above) and so is every
       root, which is what makes the prefix test sound. */
    const char *mname = NULL;
    for (int r = 0; r < g_nroots; r++) {
        size_t rlen = strlen(g_roots[r].path);
        if (strncmp(path, g_roots[r].path, rlen) == 0 && path[rlen] == '/') {
            mname = root_module_name(g_roots[r].path, path);
            break;
        }
    }
    if (!mname) mname = sanitize(stem_of(path));
    Module *m = parse_module(path, mname, src);
    /* A module name is global: it prefixes every emitted C symbol, the header
       guard, and the cache filenames. A collision is unbuildable, not just
       untidy. */
    for (Module *o = g_modules; o; o = o->next)
        if (strcmp(o->name, m->name) == 0)
            fatal("two modules both compile to the name '%s':\n  %s\n  %s\n"
                  "a module name is global (it prefixes every emitted C symbol), so "
                  "these cannot be built together. A file under a package root is "
                  "named by its root-relative path, one outside every root by its "
                  "bare stem — move one under a root, or rename it.",
                  m->name, o->path, m->path);
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

/* ---- --with-assets: embed an app's data dirs into the binary ---- */

/* Escape a string for a C double-quoted literal (backslash + quote). */
static const char *c_str_escape(const char *s) {
    size_t n = strlen(s), j = 0;
    char *out = arena_alloc(&g_arena, n * 2 + 1);
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\\' || s[i] == '"') out[j++] = '\\';
        out[j++] = s[i];
    }
    out[j] = 0;
    return out;
}

/* Deepest asset tree we will walk. A real assets/ directory is a handful of
   levels; anything past this is a loop we failed to detect or a tree nobody
   meant to embed. */
#define ASSET_MAX_DEPTH 32

/* Recursively walk `abs` (an absolute dir), emitting for every regular file an
   .incbin asm blob + extern decls into `decls`, and a vt_vfs_register() call
   into `regs` keyed by the file's path relative to the app root (`logical`).
   Uses .incbin so embedding a multi-MB asset costs no giant C array. Hidden
   entries (leading '.') are skipped. *n is the running asset counter.

   SYMLINKS ARE NOT FOLLOWED. Directory detection used to be "opendir()
   succeeded", which is also true of a symlink TO a directory -- so a tree
   containing one that points at an ancestor recursed until the build died.
   pnpm and yarn workspaces build node_modules out of symlinks, so this is a
   normal directory to drop beside an app, not an exotic one. lstat gives us
   the link itself rather than its target; the depth cap is the backstop for
   any loop lstat cannot see (a bind mount, a hardlinked directory). */
static void emit_asset_dir(const char *abs, const char *logical,
                           SBuf *decls, SBuf *regs, int *n, int depth) {
    if (depth > ASSET_MAX_DEPTH) {
        fprintf(stderr, "vytoc: asset tree deeper than %d levels at %s "
                        "-- not descending further\n", ASSET_MAX_DEPTH, abs);
        return;
    }
    DIR *dp = opendir(abs);
    if (!dp) return;
    struct dirent *de;
    while ((de = readdir(dp))) {
        if (de->d_name[0] == '.') continue; /* skip ., .., dotfiles */
        const char *full = arena_printf(&g_arena, "%s/%s", abs, de->d_name);
        const char *log  = logical[0] ? arena_printf(&g_arena, "%s/%s", logical, de->d_name)
                                      : de->d_name;
        struct stat lst;
        if (lstat(full, &lst) != 0) continue;
        if (S_ISLNK(lst.st_mode)) continue;       /* never follow, see above */
        if (S_ISDIR(lst.st_mode)) {
            emit_asset_dir(full, log, decls, regs, n, depth + 1);
            continue;
        }
        if (!S_ISREG(lst.st_mode)) continue;      /* fifo, device, socket */
        int id = (*n)++;
        sb_printf(decls,
            "__asm__(\n"
            "  \".section .rodata\\n\"\n"
            "  \".balign 1\\n\"\n"
            "  \".globl vt_asset_%d_start\\n\"\n"
            "  \"vt_asset_%d_start:\\n\"\n"
            "  \".incbin \\\"%s\\\"\\n\"\n"
            "  \"vt_asset_%d_end:\\n\"\n"
            /* .text, not .previous: .previous (and .pushsection/.popsection)
               are ELF-only pseudo-ops that mingw's COFF assembler rejects
               outright. This block is at file scope and gcc emits its own
               .section directive ahead of whatever follows, so unconditionally
               returning to .text is equivalent on both formats. */
            "  \".text\\n\"\n"
            ");\n"
            "extern const unsigned char vt_asset_%d_start[];\n"
            "extern const unsigned char vt_asset_%d_end[];\n",
            id, id, c_str_escape(full), id, id, id);
        sb_printf(regs,
            "    vt_vfs_register(\"%s\", vt_asset_%d_start,\n"
            "                    (long)(vt_asset_%d_end - vt_asset_%d_start));\n",
            c_str_escape(log), id, id, id);
    }
    closedir(dp);
}

/* Generate the C source of the embedded-asset table for `app_dir`'s data dirs,
   or return NULL when there are no assets to embed. */
static const char *gen_asset_source(const char *app_dir) {
    static const char *data_dirs[] = {"assets", "conf", "storage"};
    SBuf decls, regs;
    sb_init(&decls);
    sb_init(&regs);
    int n = 0;
    for (size_t i = 0; i < sizeof data_dirs / sizeof *data_dirs; i++) {
        const char *d = arena_printf(&g_arena, "%s/%s", app_dir, data_dirs[i]);
        emit_asset_dir(d, data_dirs[i], &decls, &regs, &n, 0);
    }
    if (n == 0) { sb_free(&decls); sb_free(&regs); return NULL; }
    SBuf out;
    sb_init(&out);
    sb_printf(&out,
        "/* generated by vytoc --with-assets; do not edit */\n"
        "#include \"vyto_rt.h\"\n\n%s\n"
        "__attribute__((constructor))\n"
        "static void vt_assets_init(void) {\n%s}\n",
        decls.data, regs.data);
    const char *src = arena_strndup(&g_arena, out.data, out.len);
    sb_free(&decls);
    sb_free(&regs);
    sb_free(&out);
    return src;
}

/* ---- Windows resources: app manifest, icon, version info ----------------
   Every Windows exe gets an application manifest, and an exe built with
   --manifest also gets its icon and version info. The manifest program
   (lib/vyto/win/manifest.vt) runs on the build host and prints a small line
   format; vytoc owns turning that into the .manifest XML, the .ico and the
   .rc that windres compiles into a COFF object for the link. */

/* POSIX single-quote a path for a shell command line. */
static const char *sh_quote(const char *s) {
    SBuf b;
    sb_init(&b);
    sb_puts(&b, "'");
    for (const char *c = s; *c; c++) {
        if (*c == '\'') sb_puts(&b, "'\\''");
        else sb_printf(&b, "%c", *c);
    }
    sb_puts(&b, "'");
    const char *out = arena_strdup(&g_arena, b.data);
    sb_free(&b);
    return out;
}

/* An RC string literal's body: a quote is doubled and a backslash escaped. */
static const char *rc_escape(const char *s) {
    SBuf b;
    sb_init(&b);
    for (const char *c = s; *c; c++) {
        if (*c == '"') sb_puts(&b, "\"\"");
        else if (*c == '\\') sb_puts(&b, "\\\\");
        else sb_printf(&b, "%c", *c);
    }
    const char *out = arena_strdup(&g_arena, b.data);
    sb_free(&b);
    return out;
}

/* XML attribute/text escaping for the manifest. */
static const char *xml_escape(const char *s) {
    SBuf b;
    sb_init(&b);
    for (const char *c = s; *c; c++) {
        switch (*c) {
        case '&': sb_puts(&b, "&amp;"); break;
        case '<': sb_puts(&b, "&lt;"); break;
        case '>': sb_puts(&b, "&gt;"); break;
        case '"': sb_puts(&b, "&quot;"); break;
        default: sb_printf(&b, "%c", *c);
        }
    }
    const char *out = arena_strdup(&g_arena, b.data);
    sb_free(&b);
    return out;
}

typedef struct {
    const char *name, *version, *icon, *company, *description, *copyright;
    bool dpi_unaware;
    /* WinManifest.service(): the installer registers the exe as a service */
    const char *svc_name, *svc_display, *svc_desc, *svc_start, *svc_deps;
    /* WinManifest.program()/.serviceFor(): more programs of the suite, built
       and installed beside the main exe, and services for them (raw "svc="
       values, passed through to the setup program) */
    int nprog;
    const char *prog_id[16], *prog_path[16];
    int nsvc;
    const char *svc_line[16];
    /* internal, for the installer's own builds (--win-elevate, --win-rcdata):
       require administrator rights; embed a payload as RCDATA 100 */
    bool elevate;
    const char *rcdata;
} WinApp;

/* Run the --manifest program on this host and parse what it prints. Its
   stderr is not captured, so a failed validation's panic lands on the
   terminal next to vytoc's own error. */
static WinApp read_win_manifest(const char *manifest_vt, bool verbose) {
    WinApp app = {0};
    char self[4096];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0) fatal("cannot locate vytoc itself to run %s", manifest_vt);
    self[n] = 0;
    const char *cmd = arena_printf(&g_arena, "%s run %s", sh_quote(self), sh_quote(manifest_vt));
    if (verbose) fprintf(stderr, "%s\n", cmd);
    FILE *f = popen(cmd, "r");
    if (!f) fatal("cannot run %s", manifest_vt);
    SBuf out;
    sb_init(&out);
    char buf[4096];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) sb_printf(&out, "%.*s", (int)got, buf);
    int status = pclose(f);
    if (status != 0) fatal("the --manifest program %s failed (its error is above)", manifest_vt);

    const char *base = dir_of(manifest_vt);
    char *text = arena_strdup(&g_arena, out.data);
    sb_free(&out);
    char *line = text;
    int lineno = 0;
    bool saw_name = false;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        lineno++;
        if (lineno == 1) {
            if (strcmp(line, "vyto-win-manifest 1") != 0)
                fatal("%s printed \"%s\" where a manifest starts with \"vyto-win-manifest 1\" "
                      "(build one with lib/vyto/win/manifest.vt and call emit())",
                      manifest_vt, line);
        } else if (*line) {
            char *eq = strchr(line, '=');
            if (!eq) fatal("%s: manifest line %d has no '=': %s", manifest_vt, lineno, line);
            *eq = 0;
            const char *key = line, *val = arena_strdup(&g_arena, eq + 1);
            if (strcmp(key, "name") == 0) { app.name = val; saw_name = true; }
            else if (strcmp(key, "version") == 0) app.version = val;
            else if (strcmp(key, "icon") == 0)
                app.icon = val[0] == '/' ? val : arena_printf(&g_arena, "%s/%s", base, val);
            else if (strcmp(key, "company") == 0) app.company = val;
            else if (strcmp(key, "description") == 0) app.description = val;
            else if (strcmp(key, "copyright") == 0) app.copyright = val;
            else if (strcmp(key, "dpi") == 0) app.dpi_unaware = strcmp(val, "unaware") == 0;
            else if (strcmp(key, "service_name") == 0) app.svc_name = val;
            else if (strcmp(key, "service_display") == 0) app.svc_display = val;
            else if (strcmp(key, "service_desc") == 0) app.svc_desc = val;
            else if (strcmp(key, "service_start") == 0) app.svc_start = val;
            else if (strcmp(key, "service_deps") == 0) app.svc_deps = val;
            else if (strcmp(key, "program") == 0) {
                const char *tab = strchr(val, '\t');
                if (!tab || app.nprog >= 16)
                    fatal("%s: bad program line (or more than 16 programs): %s", manifest_vt, val);
                const char *rel = arena_strdup(&g_arena, tab + 1);
                app.prog_id[app.nprog] = arena_printf(&g_arena, "%.*s", (int)(tab - val), val);
                app.prog_path[app.nprog] = rel[0] == '/' ? rel : arena_printf(&g_arena, "%s/%s", base, rel);
                app.nprog++;
            }
            else if (strcmp(key, "svc") == 0) {
                if (app.nsvc >= 16) fatal("%s: more than 16 services", manifest_vt);
                app.svc_line[app.nsvc++] = val;
            }
            else fatal("%s: unknown manifest key '%s' (this vytoc is older than the "
                       "manifest library that printed it?)", manifest_vt, key);
        }
        line = nl ? nl + 1 : NULL;
    }
    if (!saw_name) fatal("%s printed a manifest with no name", manifest_vt);
    return app;
}

/* The application manifest every Windows exe carries. Older Windows ignores
   what it does not know, so one manifest serves 7 through 11:
   - supportedOS: without it Windows 8.1+ reports itself as 8.0 to the app
     and applies compatibility shims
   - asInvoker: no UAC prompt, even for an exe whose name looks like an
     installer (setup, update, install)
   - activeCodePage UTF-8: the ANSI file, argv and window-title APIs speak
     UTF-8 on Windows 10 1903 and later
   - longPathAware: paths past 260 characters, where the system allows them
   - dpiAware: rendered at the display's scale instead of bitmap-stretched */
static const char *win_manifest_xml(const WinApp *app) {
    return arena_printf(&g_arena,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<!-- generated by vytoc; do not edit -->\n"
        "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\n"
        "  <assemblyIdentity type=\"win32\" name=\"%s\" version=\"1.0.0.0\"/>\n"
        "  <trustInfo xmlns=\"urn:schemas-microsoft-com:asm.v3\">\n"
        "    <security><requestedPrivileges>\n"
        "      <requestedExecutionLevel level=\"%s\" uiAccess=\"false\"/>\n"
        "    </requestedPrivileges></security>\n"
        "  </trustInfo>\n"
        "  <compatibility xmlns=\"urn:schemas-microsoft-com:compatibility.v1\">\n"
        "    <application>\n"
        "      <supportedOS Id=\"{35138b9a-5d96-4fbd-8e2d-a2440225f93a}\"/>\n" /* 7 */
        "      <supportedOS Id=\"{4a2f28e3-53b9-4441-ba9c-d69d4a4a6e38}\"/>\n" /* 8 */
        "      <supportedOS Id=\"{1f676c76-80e1-4239-95bb-83d0f6d0da78}\"/>\n" /* 8.1 */
        "      <supportedOS Id=\"{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}\"/>\n" /* 10, 11 */
        "    </application>\n"
        "  </compatibility>\n"
        "  <application xmlns=\"urn:schemas-microsoft-com:asm.v3\">\n"
        "    <windowsSettings>\n"
        "%s"
        "      <activeCodePage xmlns=\"http://schemas.microsoft.com/SMI/2019/WindowsSettings\">UTF-8</activeCodePage>\n"
        "      <longPathAware xmlns=\"http://schemas.microsoft.com/SMI/2016/WindowsSettings\">true</longPathAware>\n"
        "    </windowsSettings>\n"
        "  </application>\n"
        "</assembly>\n",
        xml_escape(app->name),
        /* An installer for all users needs administrator rights before it
           starts; everything else runs as the user who launched it. */
        app->elevate ? "requireAdministrator" : "asInvoker",
        app->dpi_unaware ? ""
                         : "      <dpiAware xmlns=\"http://schemas.microsoft.com/SMI/2005/WindowsSettings\">true</dpiAware>\n");
}

/* The icon as .ico bytes: an .ico passes through; a PNG is wrapped as a
   single-image .ico (PNG-compressed icon images load on Vista and later). */
static const char *win_icon_bytes(const char *path, size_t *len) {
    size_t n = 0;
    char *data = read_file(path, &n);
    if (!data) fatal("cannot read the manifest's icon %s", path);
    const unsigned char *u = (const unsigned char *)data;
    if (n >= 6 && u[0] == 0 && u[1] == 0 && u[2] == 1 && u[3] == 0) { *len = n; return data; }
    if (n < 24 || memcmp(u, "\x89PNG\r\n\x1a\n", 8) != 0)
        fatal("icon %s is neither a PNG nor an .ico", path);
    uint32_t w = (uint32_t)u[16] << 24 | (uint32_t)u[17] << 16 | (uint32_t)u[18] << 8 | u[19];
    uint32_t h = (uint32_t)u[20] << 24 | (uint32_t)u[21] << 16 | (uint32_t)u[22] << 8 | u[23];
    if (w == 0 || h == 0 || w > 256 || h > 256)
        fatal("icon %s is %ux%u; a Windows icon image is at most 256x256", path, w, h);
    size_t total = 6 + 16 + n;
    unsigned char *ico = arena_alloc(&g_arena, total);
    memset(ico, 0, 22);
    ico[2] = 1;                     /* type: icon */
    ico[4] = 1;                     /* one image */
    ico[6] = (unsigned char)(w == 256 ? 0 : w);
    ico[7] = (unsigned char)(h == 256 ? 0 : h);
    ico[10] = 1;                    /* planes */
    ico[12] = 32;                   /* bits per pixel */
    ico[14] = (unsigned char)(n & 0xFF);
    ico[15] = (unsigned char)((n >> 8) & 0xFF);
    ico[16] = (unsigned char)((n >> 16) & 0xFF);
    ico[17] = (unsigned char)((n >> 24) & 0xFF);
    ico[18] = 22;                   /* image data offset */
    memcpy(ico + 22, data, n);
    *len = total;
    return (const char *)ico;
}

/* FILEVERSION wants exactly four numbers; "1.2" becomes "1,2,0,0". */
static const char *rc_version_quad(const char *v) {
    unsigned parts[4] = {0, 0, 0, 0};
    int i = 0;
    for (const char *c = v; *c && i < 4; c++) {
        if (*c == '.') { i++; continue; }
        parts[i] = parts[i] * 10 + (unsigned)(*c - '0');
    }
    return arena_printf(&g_arena, "%u,%u,%u,%u", parts[0], parts[1], parts[2], parts[3]);
}

static const char *win_rc(const WinApp *app, const char *manifest_path, const char *ico_path,
                          uint64_t ico_digest, const char *exe_name) {
    SBuf b;
    sb_init(&b);
    sb_puts(&b, "// generated by vytoc; do not edit\n");
    sb_printf(&b, "// icon digest %016llx\n", (unsigned long long)ico_digest);
    sb_printf(&b, "1 24 \"%s\"\n", rc_escape(manifest_path)); /* 24 = RT_MANIFEST */
    if (ico_path) sb_printf(&b, "1 ICON \"%s\"\n", rc_escape(ico_path));
    /* the installer's payload (vyto/win/setup reads RCDATA 100); its digest
       is in the text so a new payload changes the .rc and rebuilds it */
    if (app->rcdata) {
        size_t plen = 0;
        char *pd = read_file(app->rcdata, &plen);
        if (!pd) fatal("cannot read the installer payload %s", app->rcdata);
        sb_printf(&b, "// payload digest %016llx\n", (unsigned long long)fnv1a(pd, plen));
        sb_printf(&b, "100 RCDATA \"%s\"\n", rc_escape(app->rcdata));
    }
    if (app->version) {
        const char *quad = rc_version_quad(app->version);
        sb_printf(&b, "1 VERSIONINFO\nFILEVERSION %s\nPRODUCTVERSION %s\n", quad, quad);
        sb_puts(&b, "FILEOS 0x40004\nFILETYPE 0x1\nBEGIN\n  BLOCK \"StringFileInfo\"\n  BEGIN\n"
                    "    BLOCK \"040904B0\"\n    BEGIN\n");
        sb_printf(&b, "      VALUE \"ProductName\", \"%s\"\n", rc_escape(app->name));
        sb_printf(&b, "      VALUE \"FileDescription\", \"%s\"\n",
                  rc_escape(app->description ? app->description : app->name));
        if (app->company) sb_printf(&b, "      VALUE \"CompanyName\", \"%s\"\n", rc_escape(app->company));
        if (app->copyright)
            sb_printf(&b, "      VALUE \"LegalCopyright\", \"%s\"\n", rc_escape(app->copyright));
        sb_printf(&b, "      VALUE \"FileVersion\", \"%s\"\n", rc_escape(app->version));
        sb_printf(&b, "      VALUE \"ProductVersion\", \"%s\"\n", rc_escape(app->version));
        sb_printf(&b, "      VALUE \"OriginalFilename\", \"%s\"\n", rc_escape(exe_name));
        sb_puts(&b, "    END\n  END\n  BLOCK \"VarFileInfo\"\n  BEGIN\n"
                    "    VALUE \"Translation\", 0x409, 1200\n  END\nEND\n");
    }
    const char *out = arena_strdup(&g_arena, b.data);
    sb_free(&b);
    return out;
}

/* The resource compiler that pairs with this cc: $VYTO_WINDRES, else the
   cc's own prefix with windres (x86_64-w64-mingw32-gcc -> ...-windres),
   else the stock mingw one. */
static const char *find_windres(const char *cc) {
    const char *env = getenv("VYTO_WINDRES");
    if (env && *env) return env;
    const char *sp = strchr(cc, ' ');
    size_t first = sp ? (size_t)(sp - cc) : strlen(cc);
    if (first >= 3 && strncmp(cc + first - 3, "gcc", 3) == 0)
        return arena_printf(&g_arena, "%.*swindres", (int)(first - 3), cc);
    return "x86_64-w64-mingw32-windres";
}

static const char *base_of(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* ---- --sign: Authenticode via osslsigncode ----
 *
 * osslsigncode is the one Authenticode signer that runs on Linux; there is no
 * signing code in vytoc itself. It writes a new file (never in place), so the
 * signature goes to <file>.signed and is renamed over the original.
 *
 * Signing must come after everything that rewrites the file: the link's -s
 * strip happens inside the link, and the audit only reads, so the order in the
 * build is link -> copy DLLs -> audit -> sign.
 *
 * The password never reaches a command line, where `ps` and --verbose would
 * show it: it is read from $VYTO_SIGN_PASS, written to a 0600 file in the
 * cache, handed over with -readpass and deleted again. */
static void win_sign(const char *file, const char *pfx, const char *ts, const char *name,
                     const char *cache, bool verbose) {
    const char *tool = getenv("VYTO_OSSLSIGNCODE");
    if (!tool || !*tool) tool = "osslsigncode";
    if (run_cmd(arena_printf(&g_arena, "command -v %s >/dev/null 2>&1", sh_quote(tool)), false) != 0)
        fatal("--sign needs osslsigncode (apt install osslsigncode, or set VYTO_OSSLSIGNCODE)");
    const char *passflag = "";
    const char *passfile = NULL;
    const char *pass = getenv("VYTO_SIGN_PASS");
    if (pass && *pass) {
        passfile = arena_printf(&g_arena, "%s/sign.pass.%d", cache, (int)getpid());
        unlink(passfile);
        int fd = open(passfile, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) fatal("cannot write a private password file in %s", cache);
        size_t n = strlen(pass);
        bool ok = write(fd, pass, n) == (ssize_t)n;
        close(fd);
        if (!ok) { unlink(passfile); fatal("cannot write a private password file in %s", cache); }
        passflag = arena_printf(&g_arena, " -readpass %s", sh_quote(passfile));
    }
    const char *out = arena_printf(&g_arena, "%s.signed", file);
    unlink(out);
    char *cl = arena_printf(&g_arena, "%s sign -pkcs12 %s%s -h sha256%s%s%s%s -in %s -out %s >/dev/null",
                            sh_quote(tool), sh_quote(pfx), passflag,
                            name ? " -n " : "", name ? sh_quote(name) : "",
                            ts ? " -ts " : "", ts ? sh_quote(ts) : "",
                            sh_quote(file), sh_quote(out));
    int rc = run_cmd(cl, verbose);
    if (passfile) unlink(passfile);
    if (rc != 0) {
        unlink(out);
        /* Delete the unsigned file too. The link stamp already records
           --sign, so a rebuild with nothing changed would otherwise skip the
           link, skip signing (it only signs a fresh link) and leave an
           unsigned exe that looks finished. With the file gone the next
           build relinks and signs again. */
        unlink(file);
        fatal("signing %s failed (osslsigncode exit %d)%s", file, rc,
              pass && *pass ? "" : " — if the certificate has a password, set VYTO_SIGN_PASS");
    }
    if (rename(out, file) != 0) {
        unlink(out);
        fatal("cannot replace %s with its signed copy", file);
    }
}

/* ---- --installer: <App>-<version>-setup.exe ----
 *
 * The setup program is vyto/win/setup, a stdlib package; this only drives it.
 * Three runs of this same vytoc, in a work dir inside the app's cache:
 *
 *   1. build the setup program bare            -> uninstall.exe
 *   2. run vyto/win/setup/pack on this host    -> payload.bin (zlib)
 *      over the app's exe, everything shipped beside it (DLLs, cacert.pem),
 *      uninstall.exe and a .setup.ini describing the install
 *   3. build the setup program again with payload.bin as RCDATA 100
 *      -> <exe dir>/<App>-<version>-setup.exe
 *
 * Both setup builds carry the app's --manifest identity (icon, name, version
 * info) and, for a per-machine install, requireAdministrator; --sign signs
 * them like the app. The entry files are generated here rather than built
 * from lib/, so a read-only installed stdlib is never written to. */
static const char *installer_key(const char *name) {
    char *k = arena_strdup(&g_arena, name);
    for (char *p = k; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(isalnum(c) || c == ' ' || c == '.' || c == '_' || c == '-' || c >= 0x80)) *p = '_';
    }
    return k;
}

static void make_installer(const WinApp *app, const char *exe, const char **ship, int nship,
                           const char *cache, const char *cc_override, const char *manifest_vt,
                           bool per_user, const char *sign_pfx, const char *sign_ts, bool audit,
                           bool verbose) {
    if ((app->svc_name || app->nsvc > 0) && per_user)
        fatal("%s declares a Windows service, and installing a service needs a per-machine "
              "installer: drop --per-user", manifest_vt ? manifest_vt : "the app");
    char self[4096];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0) fatal("cannot locate vytoc itself to build the installer");
    self[n] = 0;
    const char *work = arena_printf(&g_arena, "%s/installer", cache);
    if (mkdir_p(work) != 0) fatal("cannot create %s", work);
    const char *exedir = dir_of(exe);
    const char *exebase = base_of(exe);
    const char *version = app->version ? app->version : "1.0.0";

    const char *entry = arena_printf(&g_arena, "%s/setup_main.vt", work);
    const char *etext = "// generated by vytoc --installer\n"
                        "import { setupMain } from \"vyto/win/setup\";\n"
                        "fn main() { setupMain(); }\n";
    const char *pentry = arena_printf(&g_arena, "%s/pack_main.vt", work);
    const char *ptext = "// generated by vytoc --installer\n"
                        "import { packMain } from \"vyto/win/setup/pack\";\n"
                        "import { exit } from \"vyto/os\";\n"
                        "fn main() { exit(packMain(args())); }\n";
    bool ch;
    if (!write_if_changed(entry, etext, strlen(etext), &ch) ||
        !write_if_changed(pentry, ptext, strlen(ptext), &ch))
        fatal("cannot write the installer's entry files in %s", work);

    SBuf common;
    sb_init(&common);
    sb_puts(&common, " --target windows-x64 --release");
    if (cc_override) sb_printf(&common, " --cc %s", sh_quote(cc_override));
    if (manifest_vt) sb_printf(&common, " --manifest %s", sh_quote(manifest_vt));
    if (!per_user) sb_puts(&common, " --win-elevate");
    if (sign_pfx) sb_printf(&common, " --sign %s", sh_quote(sign_pfx));
    if (sign_ts) sb_printf(&common, " --timestamp %s", sh_quote(sign_ts));
    if (verbose) sb_puts(&common, " --verbose");

    /* 1. the uninstaller: the setup program with no payload */
    const char *uninst = arena_printf(&g_arena, "%s/uninstall.exe", work);
    if (run_cmd(arena_printf(&g_arena, "%s build %s%s -o %s >/dev/null", sh_quote(self),
                             sh_quote(entry), common.data, sh_quote(uninst)), verbose) != 0)
        fatal("building the uninstaller failed");

    /* 2. the payload */
    SBuf ini;
    sb_init(&ini);
    sb_printf(&ini, "name=%s\nversion=%s\nexe=%s\nscope=%s\nkey=%s\n", app->name, version, exebase,
              per_user ? "user" : "machine", installer_key(app->name));
    if (app->company) sb_printf(&ini, "company=%s\n", app->company);
    if (app->description) sb_printf(&ini, "description=%s\n", app->description);
    if (app->svc_name) {
        sb_printf(&ini, "service_name=%s\nservice_display=%s\nservice_desc=%s\nservice_start=%s\n",
                  app->svc_name, app->svc_display ? app->svc_display : app->svc_name,
                  app->svc_desc ? app->svc_desc : "", app->svc_start ? app->svc_start : "manual");
        if (app->svc_deps) sb_printf(&ini, "service_deps=%s\n", app->svc_deps);
    }
    for (int i = 0; i < app->nsvc; i++) sb_printf(&ini, "svc=%s\n", app->svc_line[i]);
    const char *inipath = arena_printf(&g_arena, "%s/setup.ini", work);
    if (!write_if_changed(inipath, ini.data, strlen(ini.data), &ch)) fatal("cannot write %s", inipath);
    sb_free(&ini);
    SBuf list;
    sb_init(&list);
    sb_printf(&list, "%s\t%s\n", exebase, exe);
    for (int i = 0; i < nship; i++) {
        const char *b = base_of(ship[i]);
        sb_printf(&list, "%s\t%s/%s\n", b, exedir, b);
    }
    /* The suite's other programs: each built like the app, into its own dir,
       then installed beside it. One folder holds every exe; a file two builds
       both ship (cacert.pem) goes in once, and two different files under one
       name are an error rather than a silent pick. */
    const char *names[256];
    const char *srcs[256];
    int nnames = 0;
    names[nnames] = exebase; srcs[nnames++] = exe;
    for (int i = 0; i < nship && nnames < 256; i++) {
        names[nnames] = base_of(ship[i]);
        srcs[nnames++] = arena_printf(&g_arena, "%s/%s", exedir, base_of(ship[i]));
    }
    for (int p = 0; p < app->nprog; p++) {
        const char *pdir = arena_printf(&g_arena, "%s/programs/%s", work, app->prog_id[p]);
        if (mkdir_p(pdir) != 0) fatal("cannot create %s", pdir);
        const char *pexe = arena_printf(&g_arena, "%s/%s.exe", pdir, app->prog_id[p]);
        if (strcmp(arena_printf(&g_arena, "%s.exe", app->prog_id[p]), exebase) == 0)
            fatal("program \"%s\" would replace the main exe %s", app->prog_id[p], exebase);
        if (run_cmd(arena_printf(&g_arena, "%s build %s%s%s -o %s >/dev/null", sh_quote(self),
                                 sh_quote(app->prog_path[p]), common.data, audit ? " --audit" : "",
                                 sh_quote(pexe)), verbose) != 0)
            fatal("building program \"%s\" (%s) failed", app->prog_id[p], app->prog_path[p]);
        DIR *d = opendir(pdir);
        if (!d) fatal("cannot read %s", pdir);
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            const char *src = arena_printf(&g_arena, "%s/%s", pdir, de->d_name);
            if (dir_exists(src)) continue;
            int dup = -1;
            for (int k = 0; k < nnames; k++) if (strcasecmp(names[k], de->d_name) == 0) dup = k;
            if (dup >= 0) {
                size_t la = 0, lb = 0;
                char *a = read_file(srcs[dup], &la), *b = read_file(src, &lb);
                if (!a || !b || la != lb || memcmp(a, b, la) != 0)
                    fatal("two programs of the suite ship different files named %s", de->d_name);
                continue;
            }
            if (nnames >= 256) fatal("too many files in the suite");
            names[nnames] = arena_strdup(&g_arena, de->d_name);
            srcs[nnames++] = src;
        }
        closedir(d);
    }
    for (int k = 1 + nship; k < nnames; k++) sb_printf(&list, "%s\t%s\n", names[k], srcs[k]);
    if (verbose)
        for (int k = 0; k < nnames; k++) fprintf(stderr, "installer payload: %s\n", names[k]);
    sb_printf(&list, "uninstall.exe\t%s\n.setup.ini\t%s\n", uninst, inipath);
    const char *listpath = arena_printf(&g_arena, "%s/files.txt", work);
    if (!write_if_changed(listpath, list.data, strlen(list.data), &ch)) fatal("cannot write %s", listpath);
    sb_free(&list);
    const char *payload = arena_printf(&g_arena, "%s/payload.bin", work);
    if (run_cmd(arena_printf(&g_arena, "%s run %s -- %s %s", sh_quote(self), sh_quote(pentry),
                             sh_quote(payload), sh_quote(listpath)), verbose) != 0)
        fatal("packing the installer failed (the reason is above)");

    /* 3. the installer: the same program, carrying the payload */
    char *fname = arena_printf(&g_arena, "%s-%s-setup.exe", installer_key(app->name), version);
    for (char *p = fname; *p; p++) if (*p == ' ') *p = '-';
    const char *setup = arena_printf(&g_arena, "%s/%s", exedir, fname);
    if (run_cmd(arena_printf(&g_arena, "%s build %s%s --win-rcdata %s%s -o %s >/dev/null",
                             sh_quote(self), sh_quote(entry), common.data, sh_quote(payload),
                             audit ? " --audit" : "", sh_quote(setup)), verbose) != 0)
        fatal("building the installer failed");
    sb_free(&common);
    fprintf(stderr, "installer: %s\n", setup);
}

/* Locate the runtime directory relative to the vytoc executable. */
static const char *find_runtime_dir(void) {
    const char *env = getenv("VYTO_HOME");
    if (env) {
        const char *p = arena_printf(&g_arena, "%s/runtime", env);
        if (file_exists(arena_printf(&g_arena, "%s/vyto_rt.c", p))) return p;
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
            if (file_exists(arena_printf(&g_arena, "%s/vyto_rt.c", cands[i]))) return cands[i];
    }
    fatal("cannot locate the Vyto runtime (set VYTO_HOME)");
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

static const char *vyto_triple(void) {
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

/* Which triples does a package ship prebuilt libraries for? A package's native/
   dir holds `src` (in-tree C, compiled every build) plus one directory per
   triple, and optionally a build-*.sh that produces them. Fills `out` with a
   comma-separated list of those triples and returns how many there were; also
   reports the build script's name via *script (NULL if there is none), so the
   caller can say how to produce the missing one. */
static int prebuilt_triples(const char *ndir, SBuf *out, const char **script) {
    *script = NULL;
    DIR *dp = opendir(ndir);
    if (!dp) return 0;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(dp))) {
        if (de->d_name[0] == '.') continue;
        if (strcmp(de->d_name, "src") == 0) continue;
        if (!dir_exists(arena_printf(&g_arena, "%s/%s", ndir, de->d_name))) {
            if (strncmp(de->d_name, "build-", 6) == 0 && has_suffix(de->d_name, ".sh"))
                *script = arena_strdup(&g_arena, de->d_name);
            continue;
        }
        sb_printf(out, "%s%s", n ? ", " : "", de->d_name);
        n++;
    }
    closedir(dp);
    return n;
}

static void usage(void) {
    fprintf(stderr,
            "vytoc — the Vyto compiler\n"
            "usage:\n"
            "  vytoc build <file.vt> [-o out] [--release] [--bundle] [--verbose]\n"
            "              [--with-assets] [--target <triple>] [--cc <cmd>] [--clean]\n"
            "              [--modpath <dir>] [--freestanding] [--shared] [--no-float]\n"
            "              [--no-fs] [--gui | --console] [--manifest <file.vt>]\n"
            "              [--audit] [--sign <cert.pfx> [--timestamp <url>]]\n"
            "              [--installer [--per-user]]\n"
            "    --modpath     a directory CONTAINING packages (like vyto_modules/),\n"
            "                   searched after the importing file's own directory and\n"
            "                   before the stdlib. Repeatable, relative to the current\n"
            "                   directory, and overrides $VYTO_PATH (a ':'-separated\n"
            "                   list of the same). A vyto_modules/ found by walking up\n"
            "                   from the entry file is always searched too\n"
            "    --bundle       statically link prebuilt native libs + the C++ runtime\n"
            "                   into one self-contained exe (no shipped .so)\n"
            "    --with-assets  embed the app's assets/, conf/ and storage/ dirs into\n"
            "                   the binary; without it they resolve from disk at runtime\n"
            "    --clean        delete this file's own .vyto-cache and rebuild it; the\n"
            "                   shared object cache is content-keyed, so it is left alone\n"
            "    --freestanding no libc: route alloc/print/abort through vt_host_* hooks\n"
            "                   the embedder supplies; output is lib<name>.a, not an exe\n"
            "    --shared       build lib<name>.so (<name>.dll on Windows) instead of an exe:\n"
            "                   an exported vyto_app_main() the loader calls (Android/JNI)\n"
            "    --gui          Windows: link for the GUI subsystem (no console window).\n"
            "                   The default when the program imports vyto/surface\n"
            "    --console      Windows: link for the console subsystem even if the\n"
            "                   program imports vyto/surface (a CLI tool that also draws)\n"
            "    --manifest     Windows: run this Vyto program on the build host and embed\n"
            "                   the app identity it prints (name, version, icon, DPI) —\n"
            "                   see lib/vyto/win/manifest.vt\n"
            "    --audit        Windows: check every import of the exe and the DLLs shipped\n"
            "                   beside it against Windows 7's real exports; fail if one\n"
            "                   would not load there (same check as `vytoc audit`)\n"
            "    --sign         Windows: Authenticode-sign the exe and its shipped DLLs\n"
            "                   with this PKCS#12 certificate via osslsigncode. The\n"
            "                   password, if any, comes from $VYTO_SIGN_PASS\n"
            "    --timestamp    with --sign: RFC 3161 timestamp server URL, so the\n"
            "                   signature outlives the certificate (needs the network)\n"
            "    --installer    Windows: also build <Name>-<version>-setup.exe beside the\n"
            "                   exe: the app, the files shipped beside it and an\n"
            "                   uninstaller, for Program Files (asks for admin rights).\n"
            "                   Programs and services the --manifest declares go in too\n"
            "    --per-user     with --installer: install for the current user only, to\n"
            "                   %%LOCALAPPDATA%%\\Programs, without admin rights (no services)\n"
            "    --no-float    stub float-to-string (no soft-float formatter pulled in)\n"
            "    --no-fs        drop the filesystem layer (File ops become no-ops/panics)\n"
            "  vytoc run   <file.vt> [--release] [--verbose] [--clean]\n"
            "              [--modpath <dir>] [-- args...]\n"
            "  vytoc audit <file.exe|file.dll>...\n"
            "                   will these Windows binaries load on Windows 7? (see --audit)\n"
            "targets: linux-x64 linux-arm64 macos-x64 macos-arm64 windows-x64 android-arm64\n"
            "  --cc (or VYTO_CC) overrides the C compiler; put sysroots/flags inside it,\n"
            "  e.g. --cc 'zig cc -target aarch64-linux-gnu' or (freestanding)\n"
            "  --cc 'arm-none-eabi-gcc -mcpu=cortex-m4 -ffreestanding' or (android)\n"
            "  --cc \"$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/\"\n"
            "        \"aarch64-linux-android24-clang\"\n");
    exit(2);
}

static const struct { const char *triple; const char *cc; } cross_cc_table[] = {
    {"linux-x64", "x86_64-linux-gnu-gcc"},
    {"linux-arm64", "aarch64-linux-gnu-gcc"},
    {"windows-x64", "x86_64-w64-mingw32-gcc"},
    {"macos-x64", NULL}, /* no conventional Linux-hosted default; require --cc */
    {"macos-arm64", NULL},
    /* The NDK's clang lives under a versioned, user-chosen install root and
       encodes the minSdk in its own name (aarch64-linux-android24-clang), so
       there is nothing conventional to default to -- require --cc. */
    {"android-arm64", NULL},
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

/* The Windows 7 export allowlist: $WIN7_EXPORTS, else the one that ships in
   the stdlib (lib/vyto/win/win7_exports.txt, regenerated by
   tests/win/dump_exports.c on a real Windows 7 box). */
static const char *win7_exports_path(void) {
    const char *e = getenv("WIN7_EXPORTS");
    if (e && *e) return e;
    const char *lib = find_lib_dir();
    if (!lib) fatal("cannot find the stdlib (set VYTO_HOME) for lib/vyto/win/win7_exports.txt");
    return arena_printf(&g_arena, "%s/vyto/win/win7_exports.txt", lib);
}

int main(int argc, char **argv) {
    if (argc < 3) usage();
    const char *cmd = argv[1];
    if (strcmp(cmd, "audit") == 0)
        return pe_audit(win7_exports_path(), argc - 2, (const char **)argv + 2);
    const char *input = argv[2];
    bool release = false, verbose = false, bundle = false, with_assets = false;
    bool freestanding = false, shared = false, no_float = false, no_fs = false, clean = false;
    bool force_gui = false, force_console = false, audit = false;
    bool installer = false, per_user = false, win_elevate = false;
    const char *win_rcdata = NULL;
    const char *sign_pfx = NULL, *sign_ts = NULL;
    const char *outpath = NULL, *target = NULL, *cc_override = NULL, *manifest_vt = NULL;
    const char *modpath[MAX_ROOTS];
    int nmodpath = 0;
    int prog_argc = 0;
    char **prog_argv = NULL;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--release") == 0) release = true;
        else if (strcmp(argv[i], "--bundle") == 0) bundle = true;
        else if (strcmp(argv[i], "--with-assets") == 0) with_assets = true;
        else if (strcmp(argv[i], "--verbose") == 0) verbose = true;
        else if (strcmp(argv[i], "--freestanding") == 0) freestanding = true;
        else if (strcmp(argv[i], "--shared") == 0) shared = true;
        else if (strcmp(argv[i], "--no-float") == 0) no_float = true;
        else if (strcmp(argv[i], "--no-fs") == 0) no_fs = true;
        else if (strcmp(argv[i], "--clean") == 0) clean = true;
        else if (strcmp(argv[i], "--gui") == 0) force_gui = true;
        else if (strcmp(argv[i], "--console") == 0) force_console = true;
        else if (strcmp(argv[i], "--manifest") == 0 && i + 1 < argc) manifest_vt = argv[++i];
        else if (strcmp(argv[i], "--audit") == 0) audit = true;
        else if (strcmp(argv[i], "--installer") == 0) installer = true;
        else if (strcmp(argv[i], "--per-user") == 0) per_user = true;
        /* internal: used by --installer for its own two builds */
        else if (strcmp(argv[i], "--win-elevate") == 0) win_elevate = true;
        else if (strcmp(argv[i], "--win-rcdata") == 0 && i + 1 < argc) win_rcdata = argv[++i];
        else if (strcmp(argv[i], "--sign") == 0 && i + 1 < argc) sign_pfx = argv[++i];
        else if (strcmp(argv[i], "--timestamp") == 0 && i + 1 < argc) sign_ts = argv[++i];
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) outpath = argv[++i];
        else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) target = argv[++i];
        else if (strcmp(argv[i], "--cc") == 0 && i + 1 < argc) cc_override = argv[++i];
        else if (strcmp(argv[i], "--modpath") == 0 && i + 1 < argc) {
            if (nmodpath < MAX_ROOTS) modpath[nmodpath++] = argv[++i];
            else i++;
        }
        else if (strcmp(argv[i], "--") == 0) { prog_argc = argc - i - 1; prog_argv = argv + i + 1; break; }
        else usage();
    }
    bool do_run = strcmp(cmd, "run") == 0;
    if (!do_run && strcmp(cmd, "build") != 0) usage();
    if (freestanding && do_run)
        fatal("freestanding builds produce a library, not a runnable executable — "
              "link libvyto.a into your firmware and flash it");
    if (shared && do_run)
        fatal("--shared produces a library, not a runnable executable — load it from "
              "a host process (on Android, package it into the apk)");
    if (shared && freestanding)
        fatal("--shared and --freestanding are different output shapes — "
              "--freestanding archives to lib<name>.a, --shared links lib<name>.so");
    if (force_gui && force_console)
        fatal("--gui and --console pick opposite Windows subsystems — pass one");
    if (!cc_override) cc_override = getenv("VYTO_CC");
    if (target && !known_triple(target))
        fatal("unknown target '%s' (see vytoc --help for the list)", target);
    const char *host = vyto_triple();
    const char *triple = target ? target : host;
    bool cross = strcmp(triple, host) != 0;
    bool win_target = strncmp(triple, "windows", 7) == 0;
    bool android_target = strncmp(triple, "android", 7) == 0;
    if (manifest_vt && !win_target)
        fatal("--manifest describes a Windows exe (name, version, icon, DPI); "
              "this build targets %s", triple);
    if (audit && !win_target)
        fatal("--audit checks a Windows exe against Windows 7's exports; "
              "this build targets %s", triple);
    if (sign_pfx && !win_target)
        fatal("--sign is Authenticode, for Windows exes; this build targets %s", triple);
    if (sign_ts && !sign_pfx) fatal("--timestamp stamps a signature: pass --sign too");
    if (installer && !win_target)
        fatal("--installer makes a Windows setup.exe; this build targets %s", triple);
    if (installer && (do_run || shared))
        fatal("--installer packages an exe: use it with `vytoc build`, without --shared");
    if (per_user && !installer) fatal("--per-user chooses the installer's scope: pass --installer too");
    if (sign_pfx && !file_exists(sign_pfx)) fatal("--sign: no certificate at %s", sign_pfx);
    if (do_run && cross)
        fatal("built for %s but this host is %s — copy the binary to the target "
              "or run it under an emulator (qemu/wine)", triple, host);

    /* ---- module search roots ----
     *
     * Must be complete before the first load_module: it is the sole consumer,
     * and a module loaded before a root was registered would be named against
     * a shorter root list than one loaded after. */
    for (int i = 0; i < nmodpath; i++)
        if (!add_root(modpath[i], "--modpath") && !dir_exists(modpath[i]))
            fprintf(stderr, "vytoc: warning: --modpath %s is not a directory\n", modpath[i]);
    if (nmodpath == 0) {                     /* flag overrides env, cf. VYTO_CC */
        const char *vpath = getenv("VYTO_PATH");
        if (vpath && *vpath) {
            char *copy = arena_strdup(&g_arena, vpath);
            for (char *tok = strtok(copy, ":"); tok; tok = strtok(NULL, ":"))
                add_root(tok, "VYTO_PATH");
        }
    }
    add_vyto_modules_roots(dir_of(input));
    add_root(find_lib_dir(), "the stdlib");  /* LAST: nothing shadows the stdlib */

    /* ---- front end ---- */
    Module *entry = load_module(input);
    check_all(g_modules, entry);

    /* ---- emit C into the cache dir (per-target subdir for explicit targets) ----
     *
     * The cache is keyed by the ENTRY FILE, not just its directory:
     *
     *     examples/.vyto-cache/107_reactor/mod_vyto_os_worker.c
     *     examples/.vyto-cache/14_layout/mod_vyto_ui_core.c
     *
     * Two programs in one directory emit DIFFERENT C for the same stdlib module,
     * because a generic instantiation is attached to the module that declared the
     * generic (check.c, instantiate_fn) and only what the program actually uses
     * gets compiled. One program's `Deque<byte[]>` and another's `Deque<int>` both
     * want to be mod_vyto_coll_deque.c. Sharing the directory made them overwrite
     * each other, which is why two builds in the same directory could not run
     * concurrently and why a sequential run could serve a stale object.
     *
     * Per entry file, they simply never meet. That is what lets the test suite
     * run examples in parallel — see tests/run_tests.sh. */
    const char *cache_root = arena_printf(&g_arena, "%s/.vyto-cache", dir_of(input));
    mkdir(cache_root, 0755);
    const char *cache = arena_printf(&g_arena, "%s/%s", cache_root, stem_of(input));
    /* --clean: discard this input's cache and build from scratch. The escape
       hatch for a cache that is serving a stale object — see the incremental
       rules further down, which decide per module from the *generated* C, not
       from .vt mtimes. Scoped to this one entry file's cache, so it can't touch
       a sibling program's build, let alone a sibling project's. */
    if (clean) {
        char *rm = arena_printf(&g_arena, "rm -rf %s", cache);
        if (run_cmd(rm, verbose) != 0) fatal("cannot clear cache %s", cache);
    }
    mkdir(cache, 0755);
    if (target) {
        cache = arena_printf(&g_arena, "%s/%s", cache, target);
        mkdir(cache, 0755);
    }

    const char *rtdir = find_runtime_dir();
    if (freestanding && !cc_override)
        fatal("freestanding builds need a bare-metal toolchain — pass one with "
              "--cc or VYTO_CC (e.g. --cc 'arm-none-eabi-gcc -mcpu=cortex-m4')");
    const char *cc;
    if (cc_override) {
        cc = cc_override;
    } else if (cross || (target && win_target)) {
        cc = default_cross_cc(triple);
        if (!cc && android_target)
            fatal("no default cross compiler for %s — pass the NDK clang with --cc or "
                  "VYTO_CC, e.g. --cc "
                  "\"$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/"
                  "aarch64-linux-android24-clang\"", triple);
        if (!cc)
            fatal("no default cross compiler for %s — pass one with --cc or VYTO_CC", triple);
        char *probe = arena_printf(&g_arena, "command -v %s >/dev/null 2>&1", cc);
        if (run_cmd(probe, false) != 0)
            fatal("cross compiler '%s' not found — install it or pass --cc/VYTO_CC "
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
    /* Shared objects must be position-independent, and that has to reach *every*
       compile -- the runtime, each module and every package's native shim all
       land in the same .so, so one non-PIC object fails the whole link. Putting
       it here also keys the object caches (both hash the command line), so PIC
       and non-PIC objects can never be mistaken for each other. */
    if (shared) fsflags = arena_printf(&g_arena, "%s -fPIC", fsflags);
    if (no_float) fsflags = arena_printf(&g_arena, "%s -DVT_NO_FLOAT", fsflags);
    if (no_fs) fsflags = arena_printf(&g_arena, "%s -DVT_NO_FS", fsflags);
    /* The Windows 7 floor, stated once for every compile instead of per shim.
       The mingw headers otherwise default to an older target, and a shim that
       raised it on its own could call a Win8+ API that refuses to load on
       Windows 7. Anything newer must go through GetProcAddress, and
       `vytoc audit` (src/peaudit.c) checks the result against Win7's real exports. */
    if (win_target)
        fsflags = arena_printf(&g_arena, "%s -D_WIN32_WINNT=0x0601 -DWINVER=0x0601", fsflags);
    /* Dead-code stripping. Vyto compiles each .vt file as one translation unit
       and does NO symbol-level tree-shaking, so importing a single symbol from a
       module drags the whole module's code into the object. -ffunction-sections
       / -fdata-sections put every function and datum in its own section; the
       matching -Wl,--gc-sections at link time (added to the link command below)
       then collects any section left unreferenced in the final program.
       Measured on the charting lib: a lineChart-only program shrank 325KB -> 64KB
       and the full 15-chart showcase 715KB -> 317KB, with no runtime cost —
       vtable data sections reference every dynamically-dispatched method, so
       gc-sections never drops a live override. tcc understands neither flag, so
       skip it there (tcc is only the default for fast debug builds anyway). */
    if (strcmp(cc, "tcc") != 0)
        fsflags = arena_printf(&g_arena, "%s -ffunction-sections -fdata-sections", fsflags);
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
       the freestanding profile likewise needs its own objects, and a PIC object
       is not interchangeable with the non-PIC one built from the same C. */
    const char *prof = arena_printf(&g_arena, "%s%s%s%s", freestanding ? "_fs" : "",
                                    shared ? "_pic" : "", no_float ? "_nf" : "",
                                    no_fs ? "_nofs" : "");
    const char *osuf = arena_printf(&g_arena, "%s%s", release ? "_rel" : (san[0] ? "_dbg" : ""),
                                    prof);

    SBuf objs;
    sb_init(&objs);
    bool relink = false;
    bool linked_now = false;

    /* Runtime object. Compiled from fixed sources with a fixed command line, so
       it does not depend on the program being built and is shared across every
       entry-file directory. The builtin-method helpers are amalgamated into
       vyto_rt.c via #include, so the translation unit is still vyto_rt.c, but
       edits to them must invalidate it -- dir_digest over the whole runtime
       directory covers that and the header at once. */
    const char *rt_c = arena_printf(&g_arena, "%s/vyto_rt.c", rtdir);
    const char *rt_cmd = arena_printf(&g_arena, "%s %s%s -w", cc, opt, fsflags);
    const char *rt_o = cached_object(rt_c, dir_digest(rtdir, NULL), rt_cmd,
                                     "the runtime", verbose, &relink);
    if (!rt_o) {
        /* No shared cache (or it failed): fall back to a local object, keyed the
           way it always was. cc is sanitized because --cc may contain spaces. */
        rt_o = arena_printf(&g_arena, "%s/vyto_rt_%s%s%s.o", cache, sanitize(cc),
                            release ? "_rel" : "", prof);
        static const char *rt_amalg[] = {"vyto_rt_num.c", "vyto_rt_str.c",
                                         "vyto_rt_arr.c", "vyto_rt_map.c", "vyto_vfs.c"};
        long rt_newest = file_mtime(rt_c);
        long rt_h_mt = file_mtime(arena_printf(&g_arena, "%s/vyto_rt.h", rtdir));
        if (rt_h_mt > rt_newest) rt_newest = rt_h_mt;
        for (size_t i = 0; i < sizeof rt_amalg / sizeof *rt_amalg; i++) {
            long t = file_mtime(arena_printf(&g_arena, "%s/%s", rtdir, rt_amalg[i]));
            if (t > rt_newest) rt_newest = t;
        }
        if (!file_exists(rt_o) || rt_newest > file_mtime(rt_o)) {
            char *c = arena_printf(&g_arena, "%s %s%s -w -c -o %s %s", cc, opt, fsflags,
                                   rt_o, rt_c);
            if (run_cmd(c, verbose) != 0) fatal("runtime compilation failed");
            relink = true;
        }
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

        /* Inputs every native object of this package shares: the per-triple
           include dir and the runtime's Windows helper header. Kept apart from
           the source-dir digests below so each source dir can be keyed on its
           own tree plus these, and nothing more. */
        uint64_t shared_digest = 1469598103934665603ULL;
        long shared_newest = -1;
        /* Re-provisioning native/<triple>/include has to invalidate the objects
           too, or a header swap is served from a stale .o. */
        {
            const char *ip = arena_printf(&g_arena, "%s/native/%s/include", mdir, triple);
            long t = file_mtime(ip);
            if (t > shared_newest) shared_newest = t;
            shared_digest ^= dir_digest(ip, NULL);
            shared_digest *= 1099511628211ULL;
        }
        /* Shims may include the runtime's shared Windows helpers (vyto_win.h,
           found via -I<runtime dir> below), so an edit there is an input of
           every shim object too. Only that one header: keying shims on the
           whole runtime would recompile every package for any runtime edit. */
        {
            const char *wh = arena_printf(&g_arena, "%s/vyto_win.h", rtdir);
            long t = file_mtime(wh);
            if (t > shared_newest) shared_newest = t;
            size_t wlen = 0;
            char *wsrc = read_file(wh, &wlen);
            if (wsrc) {
                shared_digest ^= fnv1a(wsrc, wlen);
                shared_digest *= 1099511628211ULL;
            }
        }
        /* Headers that belong to one target only. native/src is shared by every
           triple, so a package vendoring headers there imposes them on all of
           them — vyto/intl hit exactly that: ICU headers vendored for Windows
           made the Linux build compile against ICU 74's version-suffixed
           symbols and link against the system ICU 70, so every call came out
           undefined. A per-triple dir searched first keeps the two apart. */
        const char *ninc = arena_printf(&g_arena, "%s/native/%s/include", mdir, triple);
        const char *incflag = dir_exists(ninc)
            ? arena_printf(&g_arena, " -I%s", ninc) : "";
        const char *lib_dir = find_lib_dir();
        const char *app_dir = dir_of(entry->path);

        /* Two source dirs, compiled the same way:
           - native/src: every target. Its objects key on the whole of
             native/src (headers aren't tracked per object -- there is no
             dependency scanner -- so a .h edit must not serve a stale .o).
           - native/<triple>/src: this target only. It is how a package carries
             sources that cannot compile elsewhere (vyto/net's vendored curl and
             mbedTLS for windows-x64) without an #ifdef around every file and
             without every other target compiling hundreds of empty TUs. Its
             objects key on their own tree and see only their own headers (no
             -I native/src), so a shim edit does not recompile the vendored
             library and the library cannot silently depend on the shim's dir.
           Both glob flat: vendored trees live in subdirs that are included
           into, not compiled directly. */
        bool has_tsrc = false;
        for (int pass = 0; pass < 2; pass++) {
            const char *sdir = pass == 0
                ? arena_printf(&g_arena, "%s/native/src", mdir)
                : arena_printf(&g_arena, "%s/native/%s/src", mdir, triple);
            if (!dir_exists(sdir)) continue;
            if (pass == 1) has_tsrc = true;
            unsigned uses = 0;
            uint64_t digest = dir_digest(sdir, &uses);
            digest ^= shared_digest;
            digest *= 1099511628211ULL;
            /* The local fallback (no shared cache) takes the newest mtime. */
            long newest = shared_newest;
            DIR *ndp = opendir(sdir);
            if (ndp) {
                struct dirent *de;
                while ((de = readdir(ndp))) {
                    if (de->d_name[0] == '.') continue;
                    long t = file_mtime(arena_printf(&g_arena, "%s/%s", sdir, de->d_name));
                    if (t > newest) newest = t;
                }
                closedir(ndp);
            }
            /* Bake the resolved stdlib dir into native shims so vyto_lib_dir()
               can return it at runtime (used to locate vendored assets like
               fonts). Single-quoted so the shell keeps the inner quotes and the
               C preprocessor sees a string literal. Runtime VYTO_HOME still
               wins. Also the app's own directory (the entry file's dir,
               canonical) so os_app_dir()/appDir() can find its
               assets/conf/storage. Runtime VYTO_APP_DIR still wins.
               Passed ONLY to sources that mention them. VYTO_APPDIR differs per
               program, so handing it to a package that ignores it -- which is
               every package except vyto/os -- would make otherwise identical
               objects unshareable for no benefit. */
            const char *libdef = (lib_dir && (uses & USES_LIBDIR))
                ? arena_printf(&g_arena, " -DVYTO_LIBDIR='\"%s\"'", lib_dir) : "";
            const char *appdef = (app_dir && (uses & USES_APPDIR))
                ? arena_printf(&g_arena, " -DVYTO_APPDIR='\"%s\"'", app_dir) : "";
            /* A per-triple src dir may name extra include dirs, one per line,
               relative to itself, in include-dirs.txt. Vendored libraries are
               written for a build system that passes -I flags (curl's sources
               include "curl_setup.h" from every subdirectory and expect -Ilib),
               and there is no other per-package way to say so. The file sits
               inside the src dir, so it is part of that dir's digest; the dirs
               it names should too, or an edit there would not rebuild. */
            const char *xinc = "";
            if (pass == 1) {
                char *lst = read_file(arena_printf(&g_arena, "%s/include-dirs.txt", sdir), NULL);
                if (lst) {
                    SBuf xb;
                    sb_init(&xb);
                    for (char *line = strtok(lst, "\r\n"); line; line = strtok(NULL, "\r\n")) {
                        while (*line == ' ' || *line == '\t') line++;
                        if (!*line || *line == '#') continue;
                        if (line[0] == '/' || strstr(line, ".."))
                            fatal("%s/include-dirs.txt: '%s' must be a path inside that "
                                  "directory", sdir, line);
                        sb_printf(&xb, " -I%s/%s", sdir, line);
                    }
                    xinc = arena_strdup(&g_arena, xb.data ? xb.data : "");
                    sb_free(&xb);
                }
            }

            DIR *dp = opendir(sdir);
            if (!dp) continue;
            struct dirent *de;
            while ((de = readdir(dp))) {
                if (!has_suffix(de->d_name, ".c")) continue;
                const char *csrc = arena_printf(&g_arena, "%s/%s", sdir, de->d_name);
                /* The command line carries cc, opt, sanitizer and freestanding
                   flags, both -D bakes and every -I, so hashing it keys the
                   object by target triple and build profile as well. It is also
                   what keeps vyto/os honest: its shim is the only one that reads
                   VYTO_APPDIR, and since appdef differs per app it lands on a
                   different key while every other package shares one. */
                const char *cmd = arena_printf(&g_arena, "%s %s%s%s%s -w%s -I%s%s -I%s", cc,
                                               release ? "-O2" : opt, fsflags, libdef, appdef,
                                               incflag, sdir, xinc, rtdir);
                const char *label = arena_printf(&g_arena, "native source of package '%s'",
                                                 m->name);
                const char *obj = cached_object_queued(csrc, digest, cmd, label, &relink);
                if (!obj) {
                    /* No shared cache: keep the object beside the program. The
                       per-triple pass gets its own infix so a.c in both dirs
                       cannot overwrite one another. */
                    obj = arena_printf(&g_arena, "%s/native_%s_%s%s%s%s.o", cache, m->name,
                                       pass ? "t_" : "", stem_of(de->d_name),
                                       release ? "_rel" : "", prof);
                    if (!file_exists(obj) || newest > file_mtime(obj)) {
                        cjob_add(cmd, csrc, obj, NULL, label);
                        relink = true;
                    }
                }
                sb_printf(&objs, " %s", obj);
            }
            closedir(dp);
        }

        /* native/<triple>/stage/: files shipped beside the output for this
           target (vyto/net's cacert.pem). Data, not code, so they go with
           --bundle too. */
        {
            const char *stdir = arena_printf(&g_arena, "%s/native/%s/stage", mdir, triple);
            DIR *sp = opendir(stdir);
            if (sp) {
                struct dirent *de;
                while ((de = readdir(sp))) {
                    if (de->d_name[0] == '.') continue;
                    const char *f = arena_printf(&g_arena, "%s/%s", stdir, de->d_name);
                    if (!file_exists(f) || dir_exists(f)) continue;
                    if (ncopy < 64) copy_libs[ncopy++] = f;
                    else fatal("too many files to ship beside the executable (max 64)");
                }
                closedir(sp);
            }
        }

        const char *pdir = arena_printf(&g_arena, "%s/native/%s", mdir, triple);
        DIR *dp = opendir(pdir);
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
            /* A triple provided from source (native/<triple>/src) needs no
               prebuilt; neither does one that only ships headers or staged
               files alongside such sources. */
            if (!any && !has_tsrc) fatal(bundle
                ? "package '%s' has no static library (.a) in native/%s for --bundle"
                : "package '%s' has native/%s/ but no shared library in it", m->name, triple);
        } else {
            /* No native/<triple>/ for this build. That is only an error if the
               package ships prebuilts for some OTHER triple — most of the
               stdlib is src-only and legitimately has no per-triple dirs at
               all. Keying off the absence of native/src instead (as this once
               did) misses any package with both, such as vyto/gfx, and leaves
               the build to die on an unreadable undefined-reference instead.

               Shipping prebuilts elsewhere still is not proof that this target
               needs one, though: a package may carry a bundled library for a
               platform that has none installed and link the system copy on the
               rest. vyto/net does exactly that — a libcurl DLL for Windows,
               `#link "curl" if "linux"` everywhere else. So a platform-
               applicable #link on the package counts as satisfying it. */
            bool linked_here = false;
            for (int di = 0; di < m->ndecls && !linked_here; di++) {
                Decl *d = m->decls[di];
                if (d->kind != D_LINK) continue;
                if (!d->link_cond || strncmp(triple, d->link_cond, strlen(d->link_cond)) == 0)
                    linked_here = true;
            }
            const char *ndir = arena_printf(&g_arena, "%s/native", mdir);
            SBuf have;
            sb_init(&have);
            const char *script = NULL;
            if (!linked_here && prebuilt_triples(ndir, &have, &script) > 0)
                fatal("package '%s' ships prebuilt native libraries, but none for %s\n"
                      "  (has: %s)%s%s%s",
                      m->name, triple, have.data,
                      script ? " — build one with " : "",
                      script ? arena_printf(&g_arena, "%s/%s", ndir, script) : "",
                      script ? arena_printf(&g_arena, " %s", triple) : "");
            sb_free(&have);
        }
    }

    /* every queued package shim compiles here, VYTO_JOBS at a time */
    run_compile_jobs(verbose);

    for (Module *m = g_modules; m; m = m->next) {
        SBuf h, c;
        sb_init(&h);
        sb_init(&c);
        emit_module(m, m == entry, !release,
                    freestanding ? ENTRY_FREESTANDING : shared ? ENTRY_SHARED : ENTRY_EXE,
                    &h, &c);
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
    long latest_h = file_mtime(arena_printf(&g_arena, "%s/vyto_rt.h", rtdir));
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

    /* ---- --with-assets: embed the app's data dirs (assets/conf/storage) ----
       Generate a C TU of .incbin blobs that register into the runtime VFS at
       startup, compile it, and add it to the link. Skipped for freestanding
       (no hosted constructors). Regenerated every build (cheap) so added or
       edited assets are picked up; if nothing to embed, no TU is produced. */
    if (with_assets && !freestanding) {
        const char *src = gen_asset_source(dir_of(entry->path));
        if (src) {
            const char *acpath = arena_printf(&g_arena, "%s/vyto_assets.c", cache);
            const char *aopath = arena_printf(&g_arena, "%s/vyto_assets%s.o", cache, osuf);
            bool achanged = false;
            if (!write_if_changed(acpath, src, strlen(src), &achanged))
                fatal("cannot write %s", acpath);
            if (achanged || !file_exists(aopath) || file_mtime(acpath) > file_mtime(aopath)) {
                char *cmdline = arena_printf(&g_arena, "%s %s%s%s -w -I%s -I%s -c -o %s %s", cc,
                                             opt, san, fsflags, rtdir, cache, aopath, acpath);
                if (run_cmd(cmdline, verbose) != 0) fatal("asset embedding failed to compile");
                relink = true;
            }
            sb_printf(&objs, " %s", aopath);
        }
    }

    const char *sign_name = NULL;   /* the signature's program name (osslsigncode -n) */
    WinApp installer_app = {0};     /* the identity --installer packages */
    /* ---- Windows resources (see win_manifest_xml / read_win_manifest) ----
       Every exe gets the application manifest; --manifest adds its icon and
       version info. A DLL is left alone: its manifest is resource 2, and
       supportedOS / DPI / UTF-8 belong to the exe that loads it. */
    if (win_target && !shared && !freestanding) {
        WinApp app = {0};
        if (manifest_vt) app = read_win_manifest(manifest_vt, verbose);
        else app.name = stem_of(input);
        app.elevate = win_elevate;
        app.rcdata = win_rcdata;
        sign_name = app.name;
        installer_app = app;
        const char *mpath = arena_printf(&g_arena, "%s/vyto_app.manifest", cache);
        const char *mxml = win_manifest_xml(&app);
        bool rchanged = false, ch = false;
        if (!write_if_changed(mpath, mxml, strlen(mxml), &ch)) fatal("cannot write %s", mpath);
        rchanged |= ch;
        const char *ipath = NULL;
        uint64_t idigest = 0;
        if (app.icon) {
            size_t ilen = 0;
            const char *ico = win_icon_bytes(app.icon, &ilen);
            ipath = arena_printf(&g_arena, "%s/vyto_app.ico", cache);
            idigest = fnv1a(ico, ilen);
            if (!write_if_changed(ipath, ico, ilen, &ch)) fatal("cannot write %s", ipath);
            rchanged |= ch;
        }
        const char *exe_name = outpath ? stem_of(outpath) : stem_of(input);
        exe_name = arena_printf(&g_arena, "%s.exe", exe_name);
        const char *rc = win_rc(&app, mpath, ipath, idigest, exe_name);
        const char *rcpath = arena_printf(&g_arena, "%s/vyto_res.rc", cache);
        const char *ropath = arena_printf(&g_arena, "%s/vyto_res%s.o", cache, osuf);
        if (!write_if_changed(rcpath, rc, strlen(rc), &ch)) fatal("cannot write %s", rcpath);
        rchanged |= ch;
        if (rchanged || !file_exists(ropath)) {
            const char *windres = find_windres(cc);
            char *probe = arena_printf(&g_arena, "command -v %s >/dev/null 2>&1", windres);
            if (run_cmd(probe, false) != 0)
                fatal("the Windows resource compiler '%s' was not found; it ships with the "
                      "mingw binutils (Debian/Ubuntu: binutils-mingw-w64-x86-64), or set "
                      "VYTO_WINDRES", windres);
            /* --codepage 65001: the .rc is UTF-8, so a name like "Zoë's App"
               reaches the version info intact. */
            char *cmdline = arena_printf(&g_arena, "%s --codepage=65001 -O coff -i %s -o %s",
                                         windres, sh_quote(rcpath), sh_quote(ropath));
            if (run_cmd(cmdline, verbose) != 0) fatal("compiling the Windows resources failed");
            relink = true;
        }
        sb_printf(&objs, " %s", ropath);
    }

    /* ---- freestanding: archive, don't link ----
       Bare-metal has no hosted crt0/main and often no linker at this stage —
       the embedder links libvyto.a with their vt_host_* hooks, startup, and
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

    /* ---- link ----
       --shared swaps the output shape but not the inputs: same objects, same
       resolved -l set, plus -shared. The `lib` prefix is what a loader expects
       to find (Android's System.loadLibrary("foo") opens libfoo.so). */
    const char *exe = outpath ? outpath
                      : shared ? (win_target
                                      ? arena_printf(&g_arena, "%s/%s.dll", cache, stem_of(input))
                                      : arena_printf(&g_arena, "%s/lib%s.so", cache, stem_of(input)))
                               : arena_printf(&g_arena, "%s/%s%s", cache, stem_of(input),
                                              win_target ? ".exe" : "");
    const char *rpath_flag = "";
    if (need_rpath) {
        /* $ORIGIN / @loader_path must reach the linker unexpanded (single quotes) */
        if (strncmp(triple, "linux", 5) == 0 || android_target)
            rpath_flag = " '-Wl,-rpath,$ORIGIN'";
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
    {
        /* --bundle: bake the static archives + their deps and the C++/GCC
           runtimes into the exe (no shipped .so). System libs (libc, libX11)
           stay dynamic — fully static X11/glibc needs archives that usually
           aren't installed and is fragile. */
        const char *bundle_flags = bundle ? " -static-libstdc++ -static-libgcc" : "";
        const char *bundle_link = bundle
            ? arena_printf(&g_arena, "%s -lstdc++", bundle_deps.data) : "";
        /* Garbage-collect the per-function/per-datum sections emitted above, so
           code from imported modules that the program never references is left
           out of the final binary (see the -ffunction-sections note near the top
           of this function). Paired flag; skipped for tcc, which emits neither. */
        const char *gcsec = strcmp(cc, "tcc") != 0 ? " -Wl,--gc-sections" : "";
        /* -shared drops the crt0/main requirement. gc-sections still applies:
           default-visibility symbols are roots via the dynamic symbol table, so
           vyto_app_main() and every exported shim survive.

           --no-undefined is the important half. A shared link tolerates
           unresolved symbols by default, so a mistyped or missing FFI function
           produces a .so that builds clean and then dies at load time on the
           device — System.loadLibrary throwing UnsatisfiedLinkError with no
           build-time warning of any kind. Requiring every symbol to resolve at
           link time turns that into an error naming the symbol. Verified
           against a deliberately missing extern.

           Skipped for macOS, whose linker spells this -undefined error and
           already defaults to it for dylibs. */
        const char *shared_flag = "";
        if (shared) {
            shared_flag = strncmp(triple, "macos", 5) == 0
                              ? " -shared"
                              : " -shared -Wl,--no-undefined";
        }
        /* Android 15 ships devices with 16 KB memory pages, and a .so whose
           LOAD segments are aligned to the old 4 KB simply will not load there
           — the failure is at dlopen on the device, with nothing wrong at build
           time. Padding the alignment costs a few KB of file and is backward
           compatible, so it is unconditional for the triple rather than tied to
           a target API level. */
        const char *page_flag =
            (shared && android_target) ? " -Wl,-z,max-page-size=16384" : "";
        /* Windows defaults, so an exe built with no extra flags runs on a bare
           machine and looks like a native app:
           - -static-libgcc: no libgcc_s DLL to ship (it used to take --cc)
           - ASLR and DEP, stated rather than left to the binutils version
           - the subsystem: GUI for a program that opens windows (it imports
             vyto/surface), so no console window appears behind it; console
             otherwise, or when --console asks. A GUI exe started from cmd
             still prints there: the runtime attaches to the parent console.
           - -s under --release: a stripped exe, a fraction of the size */
        const char *win_flags = "";
        if (win_target) {
            bool gui = false;
            if (force_gui) gui = true;
            else if (!force_console)
                for (Module *m = g_modules; m && !gui; m = m->next)
                    if (strcmp(m->name, "vyto_surface") == 0) gui = true;
            win_flags = arena_printf(&g_arena,
                                     " -static-libgcc -Wl,--dynamicbase,--nxcompat,--high-entropy-va"
                                     " -Wl,--subsystem,%s%s",
                                     gui ? "windows" : "console", release ? " -s" : "");
        }
        char *cmdline = arena_printf(&g_arena, "%s%s%s%s%s%s%s -o %s%s%s%s%s%s -lm", cc, san,
                                     bundle_flags, gcsec, shared_flag, page_flag, win_flags, exe,
                                     objs.data, shlibs.data, libs.data, bundle_link,
                                     rpath_flag);
        /* Relink when the link command itself changed, not only when an object
           was rebuilt. Without this a link-only change — --console, --release's
           strip, a new resource object, a different --cc — left the old exe in
           place until --clean. */
        const char *stamp = arena_printf(&g_arena, "%s/link%s.cmd", cache, osuf);
        /* --sign is part of the stamp too, with the certificate's mtime: signing
           happens right after a link, so turning it on or off, or renewing the
           certificate, has to produce a fresh link to sign (or to leave
           unsigned). A rebuild with nothing changed keeps the already-signed
           exe instead of signing a signed file again. */
        const char *stampdata = sign_pfx
            ? arena_printf(&g_arena, "%s\n#sign %s %ld %s", cmdline, sign_pfx,
                           file_mtime(sign_pfx), sign_ts ? sign_ts : "")
            : cmdline;
        bool lchanged = false;
        if (!write_if_changed(stamp, stampdata, strlen(stampdata), &lchanged))
            fatal("cannot write %s", stamp);
        if (lchanged) relink = true;
        if (relink || !file_exists(exe)) {
            linked_now = true;
            if (run_cmd(cmdline, verbose) != 0) {
                /* a failed link must not leave a stamp claiming it succeeded */
                remove(stamp);
                fatal("link failed");
            }
        }
    }
    /* ship prebuilt libraries next to the executable */
    const char *exedir = dir_of(exe);
    for (int i = 0; i < ncopy; i++) {
        if (strcmp(dir_of(copy_libs[i]), exedir) == 0) continue;
        char *cl = arena_printf(&g_arena, "cp -p %s %s/", copy_libs[i], exedir);
        if (run_cmd(cl, verbose) != 0) fatal("cannot copy %s next to the executable", copy_libs[i]);
    }
    /* The exe plus every DLL it ships: what --audit checks and --sign signs. */
    const char *shipped[65];
    int nshipped = 0;
    shipped[nshipped++] = exe;
    for (int i = 0; i < ncopy; i++)
        if (has_suffix(copy_libs[i], ".dll") || has_suffix(copy_libs[i], ".DLL"))
            shipped[nshipped++] = arena_printf(&g_arena, "%s/%s", exedir, base_of(copy_libs[i]));
    if (audit) {
        fflush(stdout);
        if (pe_audit(win7_exports_path(), nshipped, shipped) != 0)
            fatal("%s would not load on Windows 7 (see above); the import audit failed", exe);
    }
    if (sign_pfx) {
        /* The exe is signed only when it was just linked (see the stamp); the
           DLLs are fresh copies on every build, so they always are. */
        for (int i = linked_now ? 0 : 1; i < nshipped; i++)
            win_sign(shipped[i], sign_pfx, sign_ts, sign_name, cache, verbose);
    }
    if (installer)
        make_installer(&installer_app, exe, copy_libs, ncopy, cache, cc_override, manifest_vt,
                       per_user, sign_pfx, sign_ts, audit, verbose);
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
