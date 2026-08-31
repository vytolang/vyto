# vytopack — fetch Vyto packages into a project

The other half of the package system. `vytoc` resolves imports from package
roots and names modules by their root-relative path; `vytopack` is what *puts*
a package where the resolver will find it.

Scope today: `install --url` + cache + lock, plus the `name` field of
`vyto.json`. Transitive dependencies, the rest of the manifest format and
`add`/`update`/`remove` are deliberately absent — a working install with an
exact pin and a verified hash is independently useful, and it is what proves
the resolver.

## Build

```sh
make                 # builds it alongside vytoc and vytobind
make test-pack       # 36 checks, no network
```

It is the one payload binary written in Vyto rather than C, so `make` is a
two-stage build: `vytoc` first, then this with it. That adds no dependency —
fetching shells out to `git`, not libcurl, so `ldd vytopack` is libc and libm
and the prerequisites stay a C99 compiler, make and git, exactly as
`docs/getting-started.md` §1 promises.

`test-pack` is split out of `make test` only to keep that target's runtime
down; it shells out to git repeatedly.

The build passes `--modpath src`, which makes `src/` a package root. That is
what lets `vytopack.vt` import its siblings as `vytopack/safety` rather than by
bare stem — the tool is its own first customer.

## Use

```sh
cd myapp
vytopack install --url=https://github.com/vytolang/vyto-kv --rev=v0.3.1
vytopack list
vytopack verify
vytopack audit [name]
vytopack cache
```

`install` writes `vyto_modules/<name>/` and records the resolved commit and a
tree hash in `vyto.lock`. Nothing else is needed to build:

```vyto
import { kv_get } from "vyto-kv/store";
```

`vytoc` finds `vyto_modules/` by walking up from the entry file, so no flag is
required. `--dir` puts a package elsewhere, and then the build needs
`--modpath <parent>`; `install` prints that line when it applies.

## You do not need this tool

A package root is a plain directory. Download a tarball, unzip it into
`vyto_modules/<name>/`, and the import above resolves — no manifest, no
lockfile, no vytopack. That is why there is only one transport (`git`): a
forge-tarball path would have bought convenience for a case that already works,
and charged libcurl in the toolchain's build for it.

Hand-installing is a first-class route, not a degraded one. Because the digest
is over the **tree** and not an archive, a `git archive` unzipped by hand hashes
identically to a clone, so `vytopack verify` works against a package vytopack
never fetched — drop the matching entry in `vyto.lock` and it is checkable like
any other. `vytopack_manual_verify` and `vytopack_manual_builds` in the test
suite hold that open.

What the tool adds over unzipping is the bookkeeping: resolving a tag to an
exact commit, recording it, hashing what landed, and reusing a shared cache
across projects.

## Layout

| File | What |
|---|---|
| `vytopack.vt` | the CLI and the install flow |
| `safety.vt` | URL / revision / name / path validation |
| `tree.vt` | the content hash of a package tree |
| `store.vt` | the cache location and the lockfile |
| `fetch.vt` | git resolve + clone + tree copy |
| `manifest.vt` | `vyto.json` — the `name` field only |
| `audit.vt` | the `audit` report — patterns, not proof |

## Naming and versions

A package may ship a `vyto.json` declaring its own name:

```json
{ "name": "vyto-semver", "license": "MIT" }
```

Precedence is `--name`, then this file, then the URL's last path component.
The manifest earns its place because without it a fork at
`github.com/them/semver-lib` installs as `semver-lib` and every
`import "vyto-semver/…"` *inside the package* stops resolving — the import path
would be a property of where the package is hosted rather than of the package.

**Only `name` is read.** A field the tool ignores is a field that drifts, so the
rest of the format arrives with the code that consumes it.

**There is deliberately no `version` in the manifest.** The git ref is the
version: `vyto.lock` records what was requested (`v0.1.2`, `master`, a raw sha)
alongside the commit it resolved to.

```json
"version": "v0.1.2",                       what you asked for
"rev": "3bbca28c3c22…",                    what it resolved to
```

Both are needed and neither substitutes for the other — `rev` is the exact pin
that gets rebuilt, `version` is what a person recognises. A manifest field would
have been a second source of truth requiring a manual bump with every tag, and
it drifted on the first release that had one (`0.1.0` inside a `v0.1.1` tag).
A branch install records `master`, which no manifest field could express.

## Three rules the code exists to keep

**It never executes code from a package.** No hooks, no `postinstall`.
Fetching runs git; nothing in the fetched tree runs at all. This is the single
largest supply-chain hole in npm and PyPI, and it costs nothing to close here
because the native-build convention already handles compilation.

**Untrusted strings never reach a shell unvalidated.** `vyto/os`'s `capture()`
is `popen(3)`, so a URL carrying `;` or `$(…)` would be *executed* rather than
fetched — the "no install scripts" promise is worthless if fetching the URL
runs code before the package is even on disk. `safety.vt` allow-lists the
characters a git URL legitimately needs rather than escaping the ones a shell
reads: escaping is a blocklist in disguise, and it has to anticipate every
metacharacter of a shell we do not control.

**It never writes outside the project.** `--dir` is containment-checked
textually (the destination does not exist yet, so there is nothing for
`realpath` to resolve), and an absolute path or one that climbs out with `..`
is refused.

`tests/run_tests_pack.sh` asserts the *reason* each refusal happened, not just
that the command failed. That distinction is the test: an install carrying
`--rev='v1;touch x'` fails either way, because with validation off `git
ls-remote` simply finds no such ref. Fault injection proved it — stubbing the
validators to return "" left an exit-status-only check passing on every case.

## `audit` — and what it is not

```
$ vytopack audit
audit vyto-kv (vyto_modules/vyto-kv)
  6 Vyto file(s), 2 C file(s)

  WARN  (package)
        compiles 2 native source file(s)
        vytoc compiles and links native/src automatically, so this C becomes
        part of your program merely by importing the package
  WARN  native/src/shim.c:14
        executes a shell command
        a library that shells out can run anything the build or the program can
```

**A report, not a verdict, and not a security guarantee.** It is pattern
matching over source text: it cannot prove a package is safe, it is evadable by
anyone trying, and a clean report means only that the obvious things are absent.
What it does well is answer the question a person actually has before adding a
dependency — *does this compile C? does it open sockets? does it shell out?* —
in ten seconds rather than an afternoon of reading.

It aims at the surface that matters. `vytopack` never runs code *from* a
package; there are no install hooks. But **`native/src/*.c` is compiled and
linked into your program automatically by `vytoc`**, so a shim carrying
`__attribute__((constructor))` runs before `main()` with no call site anywhere.
That is the path an attacker takes, and it is C rather than Vyto — which is why
the Vyto-side rules here are the shallower half.

Shipped scripts are scanned too, though nothing runs them: `native/build-*.sh`
is a real convention someone may run by hand, and a script noted only by name
would let an obvious credential grab pass as "ships a script".

**It never blocks an install.** A tool that refuses legitimate packages — and a
C shim is legitimate — gets switched off, and then it protects nobody. `install`
prints a one-line count at the moment trust is being decided; `audit` gives the
detail. It exits non-zero when anything is flagged, so CI can gate on it, but
that is a signal to look rather than a claim of malice: a networking package
opening sockets is expected, a string library doing it is not.

Signing matters more than any scanner and is still unbuilt — a scanner tells you
what code does, signing tells you who wrote it.

## The hash

`sha256` over a canonical rendering of the tree: `<path> LF <x|-> LF <sha256 of
contents> LF` per regular file, sorted by path.

Over the **tree**, not a tarball, because a tarball digest changes with
compression level, member order and mtimes — none of which are the package.
Sorted, so filesystem iteration order cannot change it. The path is inside the
digest, so a rename is a change. The executable bit is too, because a script's
executability is behaviour. `.git` is excluded: it is transport bookkeeping and
differs between a clone and a tarball of the same commit.

Verified: identical for a fresh copy and an mtime change; different for a
content edit, a rename, and a chmod.

## The cache

Content-addressed at `pkg/sha256/<hex>/`, located like the compiler's object
cache — `$VYTO_PKG_CACHE` (or `off`), then `$XDG_CACHE_HOME/vyto/pkg`, then
`~/.cache/vyto/pkg`. Not `~/.config/vyto`: this is regenerable data, and a tool
that silently fills a config directory with clones is a bad citizen.

Content-addressing means two projects pinning one revision share a copy, and an
entry can never be stale because its path *is* its content. A cached tree is
reused only when a previous lock says which content that revision produced —
the digest is of the tree, not of the sha, so without a lock entry there is
nothing to look up.
