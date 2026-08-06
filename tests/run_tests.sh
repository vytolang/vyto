#!/bin/sh
# Runs every examples/NN_*.vt and diffs stdout against examples/NN_*.expected.
# Also builds the greeter prebuilt-.so package and checks vytobind output.
set -u
cd "$(dirname "$0")/.."
fail=0

# Scratch dir for tests that write files. Created here rather than beside its
# first use, because `make clean-cache` removes it and the UI section (which
# runs earlier) writes an events file into it.
mkdir -p tests/tmp

# --- prepare the greeter package: prebuilt .so + vytobind-generated binding ---
triple=linux-x64   # matches vyto_triple() on this CI/dev box
mkdir -p "examples/greeter/native/$triple"
so="examples/greeter/native/$triple/libgreeter.so"
if [ ! -f "$so" ] || [ examples/greeter/csrc/greeter.c -nt "$so" ]; then
    cc -shared -fPIC -O2 -Wl,-soname,libgreeter.so \
       -o "$so" examples/greeter/csrc/greeter.c || exit 1
fi
./vytobind examples/greeter/csrc/greeter.h \
    --filter 'greeter_*' --filter 'GREET*' > examples/greeter/greeter.vt || exit 1
if diff -u tests/greeter.vt.expected examples/greeter/greeter.vt >/dev/null 2>&1; then
    echo "PASS vytobind_greeter"
else
    echo "FAIL vytobind_greeter (binding differs from tests/greeter.vt.expected)"
    diff -u tests/greeter.vt.expected examples/greeter/greeter.vt | head -40
    fail=1
fi

# --- cbwrap package binding (fn-pointer params must map to rawptr) ---
./vytobind examples/cbwrap/native/src/cbwrap.h --filter 'cb_*' > examples/cbwrap/cbwrap.vt || exit 1
if diff -u tests/cbwrap.vt.expected examples/cbwrap/cbwrap.vt >/dev/null 2>&1; then
    echo "PASS vytobind_cbwrap"
else
    echo "FAIL vytobind_cbwrap"
    diff -u tests/cbwrap.vt.expected examples/cbwrap/cbwrap.vt | head -20
    fail=1
fi

# --- cross-compile driver logic (host cc standing in as the cross compiler) ---
rm -rf examples/.vyto-cache/linux-x64
if ./vytoc build examples/01_hello.vt --target linux-x64 --cc cc >/dev/null &&
   [ -x examples/.vyto-cache/linux-x64/01_hello ] &&
   [ "$(examples/.vyto-cache/linux-x64/01_hello | head -1)" = "hello, vyto" ]; then
    echo "PASS cross_target_cache"
else
    echo "FAIL cross_target_cache"
    fail=1
fi
if ./vytoc run examples/01_hello.vt --target linux-arm64 2>&1 | grep -q "copy the binary"; then
    echo "PASS cross_run_refused"
else
    echo "FAIL cross_run_refused"
    fail=1
fi

# --- freestanding runtime profile: -DVT_NO_LIBC path builds, links, runs ---
# The runtime is compiled with no libc and reaches the host only through the six
# vt_host_* hooks; host_hooks.c bridges them to libc as a no-cross-toolchain
# stand-in. Also asserts the runtime object references zero libc symbols.
./vytoc build examples/01_hello.vt --freestanding --cc gcc \
        -o examples/.vyto-cache/libhello_fs.a >/dev/null 2>&1 &&
    cc -c examples/freestanding/host_hooks.c -o examples/.vyto-cache/host_hooks.o 2>/dev/null &&
    cc examples/.vyto-cache/host_hooks.o examples/.vyto-cache/libhello_fs.a \
       -o examples/.vyto-cache/hello_fs 2>/dev/null
if [ "$(examples/.vyto-cache/hello_fs 2>/dev/null | head -1)" = "hello, vyto" ] &&
   [ "$(examples/.vyto-cache/hello_fs 2>/dev/null | sed -n 5p)" = "x*2 = 5" ]; then
    echo "PASS freestanding_build_run"
else
    echo "FAIL freestanding_build_run"
    fail=1
fi
if nm examples/.vyto-cache/vyto_rt_gcc_fs.o 2>/dev/null | grep ' U ' | grep -qv 'vt_host_'; then
    echo "FAIL freestanding_no_libc (runtime object references a libc symbol)"
    nm examples/.vyto-cache/vyto_rt_gcc_fs.o | grep ' U ' | grep -v 'vt_host_' | head
    fail=1
else
    echo "PASS freestanding_no_libc"
fi
if ./vytoc run examples/01_hello.vt --freestanding 2>&1 | grep -q "not a runnable executable"; then
    echo "PASS freestanding_run_refused"
else
    echo "FAIL freestanding_run_refused"
    fail=1
fi
# A package shim under --freestanding. 01_hello imports nothing, so it never
# exercises a native/src at all; vyto/mmap is the first shim in the tree with a
# VT_NO_LIBC arm, and without this nothing would notice if that arm rotted.
if ./vytoc build tests/fixtures/mmap_freestanding.vt --freestanding --cc gcc \
        -o examples/.vyto-cache/libmmap_fs.a >/dev/null 2>&1; then
    echo "PASS freestanding_shim_stub"
else
    echo "FAIL freestanding_shim_stub (vyto/mmap's VT_NO_LIBC arm does not build)"
    fail=1
fi

# --- stdlib search path: VYTO_HOME lib, stem-collision naming, local shadowing ---
got=$(VYTO_HOME=tests/fixtures/volthome ./vytoc run tests/fixtures/libpath/main.vt 2>&1)
want="hello from VYTO_HOME lib
hello from the user module"
if [ "$got" = "$want" ]; then
    echo "PASS libpath_volthome"
else
    echo "FAIL libpath_volthome"
    printf '%s\n' "$got"
    fail=1
fi
got=$(VYTO_HOME=tests/fixtures/volthome ./vytoc run tests/fixtures/libpath/shadow/main.vt 2>&1)
if [ "$got" = "hello from the local shadow" ]; then
    echo "PASS libpath_shadow"
else
    echo "FAIL libpath_shadow"
    printf '%s\n' "$got"
    fail=1
fi

# --- run all examples against golden output ---
for src in examples/[0-9]*.vt; do
    name=$(basename "$src" .vt)
    expected="examples/$name.expected"
    # Look for the golden BEFORE running. The 18 hardware examples (51-69) have
    # none, because asserting on them needs a device: a USB port to plug into, a
    # sensor, a camera. Several block waiting for one that is not there --
    # 69_uevent watches the kernel for a full 10s, a third of the entire
    # examples run. Checking first skips them without paying for them.
    if [ ! -f "$expected" ]; then
        echo "SKIP $name (no .expected)"
        continue
    fi
    got=$(./vytoc run "$src" 2>&1)
    if [ "$got" = "$(cat "$expected")" ]; then
        echo "PASS $name"
    else
        echo "FAIL $name"
        echo "--- expected ---"
        cat "$expected"
        echo "--- got ---"
        printf '%s\n' "$got"
        fail=1
    fi
done

# --- compile-error golden tests: each .vt must fail to build with its message ---
for src in tests/errors/*.vt; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .vt)
    expected="tests/errors/$name.expected-error"
    [ -f "$expected" ] || continue
    # first diagnostic line, with the file:line prefix stripped for portability
    got=$(./vytoc build "$src" 2>&1 | sed -n '1p' | sed 's/^[^ ]*: error:/error:/')
    if [ "$got" = "$(cat "$expected")" ]; then
        echo "PASS err_$name"
    else
        echo "FAIL err_$name"
        echo "  expected: $(cat "$expected")"
        echo "  got:      $got"
        fail=1
    fi
done

# --- integer overflow: checked (traps) in debug, wraps in --release ---
got=$(./vytoc run tests/fixtures/overflow_trap.vt 2>&1)
if echo "$got" | grep -q "integer overflow in '+'"; then
    echo "PASS overflow_trap_debug"
else
    echo "FAIL overflow_trap_debug (expected a panic, got: $got)"
    fail=1
fi
got=$(./vytoc run tests/fixtures/overflow_trap.vt --release 2>&1)
if echo "$got" | grep -q -- "-9223372036854775808"; then
    echo "PASS overflow_wrap_release"
else
    echo "FAIL overflow_wrap_release (expected wrap to i64 min, got: $got)"
    fail=1
fi

# --- command-line args: args() builtin sees what follows `--` ---
got=$(./vytoc run tests/fixtures/args_echo.vt -- alpha beta 2>&1)
if [ "$got" = "$(printf 'n=2\nalpha\nbeta')" ]; then
    echo "PASS args_builtin"
else
    echo "FAIL args_builtin (got: $got)"
    fail=1
fi

# --- vyto/anim + vyto/geom/path: pure-Vyto animation clock & path flatten
#     (no blend2d — always runs) ---
got=$(./vytoc run tests/fixtures/anim_clock.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/anim_clock.expected)" ]; then
    echo "PASS anim_clock"
else
    echo "FAIL anim_clock"
    echo "--- expected ---"; cat tests/fixtures/anim_clock.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- vyto/geom: Vec2/Vec3/Vec4 value types (pure Vyto — always runs) ---
got=$(./vytoc run tests/fixtures/geom_vec.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/geom_vec.expected)" ]; then
    echo "PASS geom_vec"
else
    echo "FAIL geom_vec"
    echo "--- expected ---"; cat tests/fixtures/geom_vec.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- vyto/geom: Mat4 transforms, inverse, projections (pure Vyto) ---
got=$(./vytoc run tests/fixtures/geom_mat.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/geom_mat.expected)" ]; then
    echo "PASS geom_mat"
else
    echo "FAIL geom_mat"
    echo "--- expected ---"; cat tests/fixtures/geom_mat.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- vyto/geo: coords, datums, spherical measures, BBox (pure Vyto) ---
got=$(./vytoc run tests/fixtures/geo_math.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/geo_math.expected)" ]; then
    echo "PASS geo_math"
else
    echo "FAIL geo_math"
    echo "--- expected ---"; cat tests/fixtures/geo_math.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- vyto/geo: ECEF, ENU basis, slerp, ray intersection (pure Vyto) ---
got=$(./vytoc run tests/fixtures/geo_3d.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/geo_3d.expected)" ]; then
    echo "PASS geo_3d"
else
    echo "FAIL geo_3d"
    echo "--- expected ---"; cat tests/fixtures/geo_3d.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- vyto/geo: Geometry, area/length, point-in-polygon, simplify (pure Vyto) ---
got=$(./vytoc run tests/fixtures/geo_shape.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/geo_shape.expected)" ]; then
    echo "PASS geo_shape"
else
    echo "FAIL geo_shape"
    echo "--- expected ---"; cat tests/fixtures/geo_shape.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- vyto/geo: projections and the slippy-map tile scheme (pure Vyto) ---
got=$(./vytoc run tests/fixtures/geo_tiles.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/geo_tiles.expected)" ]; then
    echo "PASS geo_tiles"
else
    echo "FAIL geo_tiles"
    echo "--- expected ---"; cat tests/fixtures/geo_tiles.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- vyto/geo: GeoJSON reader and encoded polyline codec (pure Vyto) ---
got=$(./vytoc run tests/fixtures/geo_format.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/geo_format.expected)" ]; then
    echo "PASS geo_format"
else
    echo "FAIL geo_format"
    echo "--- expected ---"; cat tests/fixtures/geo_format.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- vyto/cli: accepted flag/option/operand/subcommand syntax (pure Vyto) ---
got=$(./vytoc run tests/fixtures/cli_parse.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/cli_parse.expected)" ]; then
    echo "PASS cli_parse"
else
    echo "FAIL cli_parse"
    echo "--- expected ---"; cat tests/fixtures/cli_parse.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- vyto/cli: every rejection path. Reaching the end is the assertion — the
#     same inputs through to_int/to_float abort the process (pure Vyto) ---
got=$(./vytoc run tests/fixtures/cli_errors.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/cli_errors.expected)" ]; then
    echo "PASS cli_errors"
else
    echo "FAIL cli_errors"
    echo "--- expected ---"; cat tests/fixtures/cli_errors.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- vyto/cli: generated --help, byte for byte (pure Vyto) ---
got=$(./vytoc run tests/fixtures/cli_help.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/cli_help.expected)" ]; then
    echo "PASS cli_help"
else
    echo "FAIL cli_help"
    echo "--- expected ---"; cat tests/fixtures/cli_help.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- vyto/util/url: parsing, encoding, query strings, and the RFC 3986 §5.4
#     reference-resolution examples (pure Vyto) ---
got=$(./vytoc run tests/fixtures/url_parse.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/url_parse.expected)" ]; then
    echo "PASS url_parse"
else
    echo "FAIL url_parse"
    echo "--- expected ---"; cat tests/fixtures/url_parse.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- vyto/util/mime: base64 (RFC 4648 §10 vectors), quoted-printable, media
#     types, extension lookup and multipart (pure Vyto) ---
got=$(./vytoc run tests/fixtures/mime_codec.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/mime_codec.expected)" ]; then
    echo "PASS mime_codec"
else
    echo "FAIL mime_codec"
    echo "--- expected ---"; cat tests/fixtures/mime_codec.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- vyto/util/uuid: format/parse against fixed vectors, plus properties of a
#     few thousand generated values. The one vyto/util module with a native
#     shim, so unlike its siblings this one links C (getrandom/urandom) ---
got=$(./vytoc run tests/fixtures/uuid_format.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/uuid_format.expected)" ]; then
    echo "PASS uuid_format"
else
    echo "FAIL uuid_format"
    echo "--- expected ---"; cat tests/fixtures/uuid_format.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- vyto/util/csv: the RFC 4180 grammar (quotes, embedded delimiters and
#     newlines), dialects, sniffing, writer quoting, and the file round trip.
#     Writes under /tmp, not the repo (pure Vyto) ---
got=$(./vytoc run tests/fixtures/csv_parse.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/csv_parse.expected)" ]; then
    echo "PASS csv_parse"
else
    echo "FAIL csv_parse"
    echo "--- expected ---"; cat tests/fixtures/csv_parse.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- vyto/util/xml: the pull reader's token stream, namespace scoping, entity
#     decoding, the DOM's search methods, serialization, every error path
#     (pure Vyto) ---
got=$(./vytoc run tests/fixtures/xml_parse.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/xml_parse.expected)" ]; then
    echo "PASS xml_parse"
else
    echo "FAIL xml_parse"
    echo "--- expected ---"; cat tests/fixtures/xml_parse.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- vyto/util/toml: the whole v1.0 grammar, the documented deviations
#     (dates as strings, integers as floats), errors, encoder round trip.
#     Results print as JSON because a TOML document *is* a JsonValue here ---
got=$(./vytoc run tests/fixtures/toml_parse.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/toml_parse.expected)" ]; then
    echo "PASS toml_parse"
else
    echo "FAIL toml_parse"
    echo "--- expected ---"; cat tests/fixtures/toml_parse.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- vyto/util/ini: sections, both separators, both comment characters,
#     quoting, nested mode, errors, encoder round trip (pure Vyto) ---
got=$(./vytoc run tests/fixtures/ini_parse.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/ini_parse.expected)" ]; then
    echo "PASS ini_parse"
else
    echo "FAIL ini_parse"
    echo "--- expected ---"; cat tests/fixtures/ini_parse.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- vyto/util/html: the escapers (every case is a string that would be an
#     injection unescaped), the entity decoder, the builder and its error
#     paths, the one-shot helpers (pure Vyto) ---
got=$(./vytoc run tests/fixtures/html_escape.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/html_escape.expected)" ]; then
    echo "PASS html_escape"
else
    echo "FAIL html_escape"
    echo "--- expected ---"; cat tests/fixtures/html_escape.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- vyto/util/markdown: block structure, inline spans, the tree shape, and
#     the documented exclusions (raw HTML is escaped, not forwarded) ---
got=$(./vytoc run tests/fixtures/markdown_render.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/markdown_render.expected)" ]; then
    echo "PASS markdown_render"
else
    echo "FAIL markdown_render"
    echo "--- expected ---"; cat tests/fixtures/markdown_render.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- vyto/util/log: levels, both formats, fields and context, escaping,
#     timestamps, the file sink. Every logger pins its clock with fixedTime(),
#     which is the only reason a logging module is golden-testable ---
got=$(./vytoc run tests/fixtures/log_format.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/log_format.expected)" ]; then
    echo "PASS log_format"
else
    echo "FAIL log_format"
    echo "--- expected ---"; cat tests/fixtures/log_format.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- vyto/regex: PCRE2 through the shim — groups, named groups, the zero-width
#     findAll step, embedded NULs, invalid UTF-8, replace's grow-and-retry, and
#     a hostile pattern hitting the match limit instead of hanging.
#     Deliberately NOT gated on anything: PCRE2 is vendored under
#     lib/vyto/regex/native/src/pcre2/ and built from source, so unlike the gfx
#     block below this can never legitimately skip. The fixture's output is
#     identical with JIT on and off, so it also holds where sljit has no
#     backend ---
got=$(./vytoc run tests/fixtures/regex_match.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/regex_match.expected)" ]; then
    echo "PASS regex_match"
else
    echo "FAIL regex_match"
    echo "--- expected ---"; cat tests/fixtures/regex_match.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- vyto/regex: the same fixture with the JIT forced off. Guards the
#     interpreter fallback that a hardened runtime, a TCC build and every
#     unsupported architecture rely on ---
got=$(VYTO_REGEX_JIT=0 ./vytoc run tests/fixtures/regex_match.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/regex_match.expected)" ]; then
    echo "PASS regex_match_nojit"
else
    echo "FAIL regex_match_nojit"
    echo "--- expected ---"; cat tests/fixtures/regex_match.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- vyto/validator: the coercion chain changing the type of the check, the
#     short-circuit that keeps one failing value to one error, characters vs
#     bytes in the length rules, required vs optional, every format predicate,
#     and both custom-rule forms ---
got=$(./vytoc run tests/fixtures/validator.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/validator.expected)" ]; then
    echo "PASS validator"
else
    echo "FAIL validator"
    echo "--- expected ---"; cat tests/fixtures/validator.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- vyto/validator: the core must NOT drag in vyto/regex. The three rules that
#     need a regex engine live in vyto/validator/pattern precisely so a program
#     doing length and range checks doesn't compile 30 PCRE2 translation units.
#     Build a core-only program into a scratch cache and prove no regex object
#     was produced for it ---
rm -rf tests/tmp/valcore
mkdir -p tests/tmp/valcore
cat > tests/tmp/valcore/core.vt <<'VTEOF'
import { validation } from "vyto/validator";
fn main() { let v = validation(); v.check("hi").required().minChars(2); print(v.ok()); }
VTEOF
if VYTO_OBJ_CACHE=tests/tmp/valcore/obj ./vytoc build tests/tmp/valcore/core.vt \
        -o tests/tmp/valcore/core >/dev/null 2>&1; then
    if grep -rl vregex tests/tmp/valcore/obj >/dev/null 2>&1; then
        echo "FAIL validator_no_regex (core pulled in PCRE2)"
        fail=1
    else
        echo "PASS validator_no_regex"
    fi
else
    echo "FAIL validator_no_regex (build failed)"
    fail=1
fi

# --- vyto/regex: the convenience layer — the four match walkers agreeing on a
#     zero-width pattern, rx_quote round-tripping every metacharacter,
#     fullMatch's \z beating a trailing newline, expand's template syntax, the
#     flags default on every rx_*, and the two validators ---
got=$(./vytoc run tests/fixtures/regex_extra.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/regex_extra.expected)" ]; then
    echo "PASS regex_extra"
else
    echo "FAIL regex_extra"
    echo "--- expected ---"; cat tests/fixtures/regex_extra.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- vyto/regex: the convenience layer with the JIT forced off ---
got=$(VYTO_REGEX_JIT=0 ./vytoc run tests/fixtures/regex_extra.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/regex_extra.expected)" ]; then
    echo "PASS regex_extra_nojit"
else
    echo "FAIL regex_extra_nojit"
    echo "--- expected ---"; cat tests/fixtures/regex_extra.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- vyto/mmap: mapping a file as a zero-copy byte[] view. The check that
#     matters most is a view outliving its Mapping -- the view retains it, so
#     the pages cannot be unmapped underneath. Also sub-views, windowed maps
#     (the offset the shim rounds down), read/write + msync, anonymous
#     mappings, and the LE/BE decoders ---
got=$(./vytoc run tests/fixtures/mmap_basics.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/mmap_basics.expected)" ]; then
    echo "PASS mmap_basics"
else
    echo "FAIL mmap_basics"
    echo "--- expected ---"; cat tests/fixtures/mmap_basics.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- vyto/mmap guards: a view is fixed-length and (over a read-only mapping)
#     unwritable. Grep-style rather than a golden, because a panic aborts the
#     process and truncates stdout. Without these the read-only cases would be
#     a bare SIGSEGV with no file, line or message. ---
for c in push pop insert remove_at extend reserve clear; do
    if ./vytoc run tests/fixtures/mmap_guards.vt -- "$c" 2>&1 |
           grep -q "$c on a fixed-length array view"; then
        echo "PASS mmap_guard_$c"
    else
        echo "FAIL mmap_guard_$c (expected a fixed-length panic)"
        fail=1
    fi
done
for c in store compound fill reverse; do
    if ./vytoc run tests/fixtures/mmap_guards.vt -- "$c" 2>&1 |
           grep -q "write to a read-only array view"; then
        echo "PASS mmap_guard_$c"
    else
        echo "FAIL mmap_guard_$c (expected a read-only panic)"
        fail=1
    fi
done
for c in view_neg view_past; do
    if ./vytoc run tests/fixtures/mmap_guards.vt -- "$c" 2>&1 | grep -q "view out of bounds"; then
        echo "PASS mmap_guard_$c"
    else
        echo "FAIL mmap_guard_$c (expected a bounds panic)"
        fail=1
    fi
done
# ...and the operations a view IS allowed: in-place stores on a writable view,
# fill/reverse/sort, extending FROM a view, slicing one. A guard that also
# blocked these would be overreach.
if ./vytoc run tests/fixtures/mmap_guards.vt -- allowed 2>&1 | grep -q "no panic for allowed"; then
    echo "PASS mmap_guard_allowed"
else
    echo "FAIL mmap_guard_allowed (a permitted operation panicked)"
    fail=1
fi

# --- vyto/ds: 200 checks over the nine index structures. Trie's flat
#     first-child/next-sibling layout and longest_prefix, Dsu over a 1000-long
#     chain, Fenwick build()==repeated add() and lower_bound, SegTree with a
#     deliberately non-commutative combine, Bloom's no-false-negative
#     guarantee, SkipList over 300 elements with 150 removals, SkipMap's
#     ceiling/floor across a hole left by a removal, Graph where
#     Dijkstra must beat BFS, and IntervalTree's half-open boundaries ---
got=$(./vytoc run tests/fixtures/ds_structures.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/ds_structures.expected)" ]; then
    echo "PASS ds_structures"
else
    echo "FAIL ds_structures"
    echo "--- expected ---"; cat tests/fixtures/ds_structures.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- switch: int subjects (a real C jump table) and string subjects (a
#     vt_str_eq chain), multi-value arms, default written anywhere, and above
#     all `break` inside an arm inside a loop — C binds that to the switch,
#     Vyto binds it to the loop, so the emitter turns it into a goto. Also a
#     switch inside a generic, which guards the arm deep-clone ---
got=$(./vytoc run tests/fixtures/switch_stmt.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/switch_stmt.expected)" ]; then
    echo "PASS switch_stmt"
else
    echo "FAIL switch_stmt"
    echo "--- expected ---"; cat tests/fixtures/switch_stmt.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- vyto/coll: 145 checks over the eight containers. The ones that matter are
#     HashMap/HashSet deletion (backward shift, verified by removing half a
#     table and then checking every key, and again with every key colliding),
#     Deque ring wrap, RingBuffer's drop accounting, LRU eviction order under
#     churn, and Slab generations refusing to revive a stale handle ---
got=$(./vytoc run tests/fixtures/coll_containers.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/coll_containers.expected)" ]; then
    echo "PASS coll_containers"
else
    echo "FAIL coll_containers"
    echo "--- expected ---"; cat tests/fixtures/coll_containers.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- for-in over a user container (a class with len()/at(i)): the release
#     paths (break, continue, return), len() being re-read each iteration so a
#     container that grows or shrinks in the body stays correct, virtual at(),
#     inherited len/at, and a generic container holding refs ---
got=$(./vytoc run tests/fixtures/forin_container.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/forin_container.expected)" ]; then
    echo "PASS forin_container"
else
    echo "FAIL forin_container"
    echo "--- expected ---"; cat tests/fixtures/forin_container.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- vyto/hash: 87 known-answer checks against other people's numbers —
#     FNV-1a and CRC-32/32C published vectors, xxHash64 reference values, all
#     sixteen SipHash-2-4 vectors from the paper, plus the properties the table
#     helpers promise (non-negative, deterministic, scattering) ---
got=$(./vytoc run tests/fixtures/hash_vectors.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/hash_vectors.expected)" ]; then
    echo "PASS hash_vectors"
else
    echo "FAIL hash_vectors"
    echo "--- expected ---"; cat tests/fixtures/hash_vectors.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- generics across modules: a generic declared in one module, instantiated
#     in another with the *caller's* own class. The instance is emitted into
#     the declaring module, which forward-declares the foreign class in its
#     header and includes the caller's header from its .c. Covers generic
#     classes, two type arguments, a subclass, an array-of-T field, and a
#     generic fn taking T by value. ---
got=$(./vytoc run tests/fixtures/generic_cross_class.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/generic_cross_class.expected)" ]; then
    echo "PASS generic_cross_class"
else
    echo "FAIL generic_cross_class"
    echo "--- expected ---"; cat tests/fixtures/generic_cross_class.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- generics across modules: holding another module's generic *struct*
#     instance as a by-value field. emit_one_struct used to recurse into any
#     struct with a generic_origin regardless of owning module, emitting a
#     second copy of the body and failing the C compile. ---
got=$(./vytoc run tests/fixtures/generic_cross_struct_field.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/generic_cross_struct_field.expected)" ]; then
    echo "PASS generic_cross_struct_field"
else
    echo "FAIL generic_cross_struct_field"
    echo "--- expected ---"; cat tests/fixtures/generic_cross_struct_field.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- vyto/gfx: blend2d Canvas -> blitPtr (gated on the prebuilt lib) ---
if [ -f lib/vyto/gfx/native/linux-x64/libblend2d.so ]; then
    gfx_bin=apps/gfxdemo/.vyto-cache/gfxdemo_test
    if ./vytoc build apps/gfxdemo/gfxdemo.vt -o "$gfx_bin" >/dev/null 2>&1; then
        got=$(VS_HEADLESS=1 VS_EVENTS=/dev/null "$gfx_bin" 2>&1)
        if echo "$got" | grep -q "gfx demo painted"; then
            echo "PASS gfx_canvas_blit"
        else
            echo "FAIL gfx_canvas_blit (got: $got)"
            fail=1
        fi
    else
        echo "FAIL gfx_canvas_blit (build failed)"
        fail=1
    fi
    # vyto/ui rendered through GfxPainter (blend2d rich tier)
    uigfx_bin=apps/uigfx/.vyto-cache/uigfx_test
    if ./vytoc build apps/uigfx/uigfx.vt -o "$uigfx_bin" >/dev/null 2>&1; then
        printf 'close\n' > tests/tmp/uigfx.events
        got=$(VS_HEADLESS=1 VS_EVENTS=tests/tmp/uigfx.events "$uigfx_bin" 2>&1)
        if echo "$got" | grep -q "uigfx done"; then
            echo "PASS gfx_ui_painter"
        else
            echo "FAIL gfx_ui_painter (got: $got)"
            fail=1
        fi
    else
        echo "FAIL gfx_ui_painter (build failed)"
        fail=1
    fi
    # motion gallery: 13 animation tiles exercising transforms/paths/gradients/
    # anim on a raw Surface loop (scripted events end in close so it exits)
    motion_bin=apps/motiondemo/.vyto-cache/motiondemo_test
    if ./vytoc build apps/motiondemo/motiondemo.vt -o "$motion_bin" >/dev/null 2>&1; then
        got=$(VS_HEADLESS=1 VS_EVENTS=apps/motiondemo/motiondemo.events "$motion_bin" 2>&1)
        if echo "$got" | grep -q "motion gallery ready"; then
            echo "PASS motion_demo"
        else
            echo "FAIL motion_demo (got: $got)"
            fail=1
        fi
    else
        echo "FAIL motion_demo (build failed)"
        fail=1
    fi
    # VoltPhone: the iOS-skin shell over Icon/Avatar/Image. Nothing covered it
    # before, and it silently rotted out of compiling as Rect/Size went
    # sub-pixel. The scripted events tap into all three screens and back out
    # again, and VOLTPHONE_TRACE makes the app name each screen it shows — a
    # tap that misses leaves the app happily on the screen it was already
    # showing, which an exit-status check cannot tell from a working tap.
    iphone_bin=apps/iphone/.vyto-cache/iphone_test
    if ./vytoc build apps/iphone/iphone.vt -o "$iphone_bin" >/dev/null 2>&1; then
        got=$(VOLTPHONE_TRACE=1 VS_HEADLESS=1 VS_EVENTS=apps/iphone/iphone.events \
              "$iphone_bin" 2>&1)
        want="screen home
screen settings
screen home
screen contacts
screen home
screen photos
screen home
voltphone closed"
        if [ "$got" = "$want" ]; then
            echo "PASS app_iphone_runs"
        else
            echo "FAIL app_iphone_runs (got: $got)"
            fail=1
        fi
    else
        echo "FAIL app_iphone_runs (build failed)"
        fail=1
    fi
    # --bundle: one self-contained exe, no libblend2d.so/libstdc++ alongside
    if [ -f lib/vyto/gfx/native/linux-x64/libblend2d.a ]; then
        bnd_bin=apps/uigfx/.vyto-cache/uigfx_bundled
        if ./vytoc build apps/uigfx/uigfx.vt --bundle -o "$bnd_bin" >/dev/null 2>&1 &&
           ! ldd "$bnd_bin" 2>/dev/null | grep -qiE "blend2d|stdc\+\+"; then
            echo "PASS gfx_bundle_static"
        else
            echo "FAIL gfx_bundle_static (blend2d/stdc++ still dynamic or build failed)"
            fail=1
        fi
    else
        echo "SKIP gfx_bundle_static (no libblend2d.a)"
    fi
    # gfx_image_load_bytes: in-memory decode (no curl/network) backing
    # Image/Avatar's http:// loading path — see vyto/gfx/painter.vt's
    # load_image_url and lib/vyto/net.
    got=$(./vytoc run tests/fixtures/gfx_load_bytes.vt 2>&1)
    if [ "$got" = "w=300 h=300" ]; then
        echo "PASS gfx_load_bytes"
    else
        echo "FAIL gfx_load_bytes (got: $got)"
        fail=1
    fi
else
    echo "SKIP gfx_canvas_blit (no libblend2d — run lib/vyto/gfx/native/build-blend2d.sh)"
fi

# --- readfile/readlines on a growable /proc file (size 0 by stat) ---
if [ -r /proc/self/status ]; then
    got=$(./vytoc run tests/fixtures/proc_read.vt 2>&1)
    if [ "$got" = "ok" ]; then
        echo "PASS proc_read"
    else
        echo "FAIL proc_read (got: $got)"
        fail=1
    fi
fi

# --- vyto/surface binding: regenerate with vytobind, golden-check ---
./vytobind lib/vyto/surface/native/src/vsurf.h \
    --lib X11@linux --lib X11@macos --lib gdi32@windows --lib user32@windows \
    --filter 'vs_*' --filter 'VS_*' > lib/vyto/surface/vsurf.vt || exit 1
if diff -u tests/vsurf.vt.expected lib/vyto/surface/vsurf.vt >/dev/null 2>&1; then
    echo "PASS vytobind_vsurf"
else
    echo "FAIL vytobind_vsurf"
    diff -u tests/vsurf.vt.expected lib/vyto/surface/vsurf.vt | head -30
    fail=1
fi

# --- conditional #link: platform-filtered libraries stay off the link line ---
got=$(./vytoc run tests/fixtures/condlink.vt 2>&1)
if [ "$got" = "conditional link ok" ]; then
    echo "PASS condlink"
else
    echo "FAIL condlink"
    printf '%s\n' "$got"
    fail=1
fi

# Start the UI section from a cold cache, but only when it would actually be
# stale. These tests are the only ones that assert on lib/vyto/ui behaviour,
# and a stale object here makes them validate the previous build instead of the
# current one — observed in practice, with the suite reporting results one
# edit-generation behind the source on disk. That risk is real and this guard
# keeps it covered; what changed is the price.
#
# The cost was badly underestimated. "Only the first test pays the cold build"
# is not what happens: emitted Vyto C is per-entry-file (generics are
# monomorphized into the module that declared them), so each of the ~55 UI
# tests compiles its own copy of the whole toolkit. Measured on this tree,
# the UI section takes 370s from cold and 16s warm — a 23x difference, and
# most of the entire suite's runtime.
#
# So wipe on the condition that matters: something the cache was built from is
# newer than the cache. A stamp file rather than the directory's mtime, because
# a directory's mtime only moves when its entries change, and a fully-cached
# run adds no entries. Nested .vyto-cache dirs under lib/ are pruned so a
# stray one from a manual run in a package directory cannot trigger a wipe.
#
# Sources checked: src (the compiler), lib (the stdlib), runtime, and the vytoc
# binary itself — a rebuilt compiler can change the emitted C for unchanged
# input, so its mtime has to count.
ui_stamp=tests/ui/.vyto-cache/.suite-stamp
if [ ! -f "$ui_stamp" ]; then
    rm -rf tests/ui/.vyto-cache
elif [ -n "$(find src lib runtime vytoc -name .vyto-cache -prune -o \
                  -newer "$ui_stamp" -type f -print -quit 2>/dev/null)" ]; then
    rm -rf tests/ui/.vyto-cache
fi
mkdir -p tests/ui/.vyto-cache
# Stamped before the loop, not after: the sources are not changing while the
# suite runs, and a run that dies partway then leaves a stamp older than
# nothing in particular — the next run rebuilds only if a source moved, which
# is the same rule. Use `make clean-cache` to force a cold one.
touch "$ui_stamp"

# --- vyto/ui headless golden tests (VS_HEADLESS backend, scripted events) ---
for src in tests/ui/[0-9]*.vt; do
    name=$(basename "$src" .vt)
    # Charts live in tests/run_tests_charts.sh (make test-charts). They are 21
    # of these cases and the most expensive third of the suite, and chart.vt is
    # a leaf nothing else imports — so they are both the costliest and the
    # least likely to be broken by a change elsewhere.
    # Charts and mobile widgets each have their own runner (make test-charts,
    # make test-mobile). Both are leaf packages nothing else imports, and both
    # are expensive: every entry file compiles its own copy of the ui stack,
    # because generics are monomorphized into the declaring module. Splitting
    # them keeps `make test` usable as an inner-loop check without dropping any
    # coverage. 53/54_dragscroll stay here — they exercise core.vt's on_drag,
    # not the mobile package.
    case "$name" in *_chart_*|*_mobile_*) continue ;; esac
    got=$(VS_HEADLESS=1 VS_EVENTS="tests/ui/$name.events" ./vytoc run "$src" 2>&1)
    if [ "$got" = "$(cat "tests/ui/$name.expected")" ]; then
        echo "PASS ui_$name"
    else
        echo "FAIL ui_$name"
        echo "--- expected ---"
        cat "tests/ui/$name.expected"
        echo "--- got ---"
        printf '%s\n' "$got"
        fail=1
    fi
done

# --- vyto/ui named golden tests: TextArea + menus (no env needed) ---
for name in textarea menu gallery scale spring disabled motion layout_anim transition dark_theme ripple; do
    got=$(VS_HEADLESS=1 VS_EVENTS="tests/ui/$name.events" ./vytoc run "tests/ui/$name.vt" 2>&1)
    if [ "$got" = "$(cat "tests/ui/$name.expected")" ]; then
        echo "PASS ui_$name"
    else
        echo "FAIL ui_$name"
        echo "--- expected ---"; cat "tests/ui/$name.expected"
        echo "--- got ---"; printf '%s\n' "$got"
        fail=1
    fi
done

# --- render snapshots: rasterize real widget trees through the rich tier
#     (blend2d) and assert on a hash of the pixels. The goldens above cover
#     widget state; these cover what the widgets actually look like.
#
#     On failure the frames are dumped as PPMs under tests/tmp/snap so the
#     change can be inspected — a hash says a frame moved, not how.
mkdir -p tests/tmp
for src in tests/ui/snap_*.vt; do
    [ -e "$src" ] || continue
    name=$(basename "$src" .vt)
    got=$(VS_HEADLESS=1 VS_EVENTS="tests/ui/$name.events" ./vytoc run "$src" 2>&1)
    if [ "$got" = "$(cat "tests/ui/$name.expected")" ]; then
        echo "PASS ui_$name"
    else
        echo "FAIL ui_$name"
        echo "--- expected ---"; cat "tests/ui/$name.expected"
        echo "--- got ---"; printf '%s\n' "$got"
        rm -rf "tests/tmp/snap/$name"; mkdir -p "tests/tmp/snap/$name"
        VS_HEADLESS=1 VS_EVENTS="tests/ui/$name.events" \
            VYTO_SNAP_DIR="tests/tmp/snap/$name" ./vytoc run "$src" >/dev/null 2>&1
        echo "--- frames written to tests/tmp/snap/$name ---"
        fail=1
    fi
done

# --- framebuffer backend, file target: render a known pattern into a raw
#     XRGB8888 file and verify it byte-for-byte (no display or fb device) ---
rm -f tests/tmp/fb.raw
render=$(VS_FBDEV=tests/tmp/fb.raw VS_FB_W=4 VS_FB_H=4 \
         ./vytoc run tests/ui/fb_render.vt 2>&1)
got=$(od -An -tx1 -w16 tests/tmp/fb.raw 2>/dev/null)
if [ "$render" = "rendered 4x4" ] && [ "$got" = "$(cat tests/ui/fb_render.expected)" ]; then
    echo "PASS fbdev_render"
else
    echo "FAIL fbdev_render"
    echo "--- render stdout ---"; printf '%s\n' "$render"
    echo "--- expected ---"; cat tests/ui/fb_render.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- listdir / isdir builtins + FilePicker over a seeded directory ---
mkdir -p tests/tmp/pickdir/sub
printf 'aaa\n' > tests/tmp/pickdir/a.txt
printf 'bbb\n' > tests/tmp/pickdir/b.txt
printf 'inner\n' > tests/tmp/pickdir/sub/inner.txt
got=$(./vytoc run tests/fixtures/listdir.vt 2>&1)
want=".. (dir)
a.txt (file)
b.txt (file)
sub (dir)"
if [ "$got" = "$want" ]; then
    echo "PASS listdir"
else
    echo "FAIL listdir"; printf '%s\n' "$got"; fail=1
fi
got=$(VS_HEADLESS=1 VS_EVENTS=tests/ui/filepicker.events VYTO_PICK_DIR=tests/tmp/pickdir \
      ./vytoc run tests/ui/filepicker.vt 2>&1)
if [ "$got" = "$(cat tests/ui/filepicker.expected)" ]; then
    echo "PASS ui_filepicker"
else
    echo "FAIL ui_filepicker"
    echo "--- expected ---"; cat tests/ui/filepicker.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- VoltPad headless end-to-end: type, Save As through the menu+picker ---
mkdir -p tests/tmp/notedir
rm -f tests/tmp/notedir/out.txt
out=$(VS_HEADLESS=1 VS_EVENTS=tests/ui/notepad_save.events VYTO_NOTE_DIR=tests/tmp/notedir \
      ./vytoc run apps/notepad/notepad.vt 2>&1)
saved=$(cat tests/tmp/notedir/out.txt 2>/dev/null)
want_note="hello
world"
if [ "$out" = "bye — 11 byte(s), file: tests/tmp/notedir/out.txt" ] && [ "$saved" = "$want_note" ]; then
    echo "PASS notepad_e2e"
else
    echo "FAIL notepad_e2e"
    printf 'out: %s\nsaved: %s\n' "$out" "$saved"
    fail=1
fi

# --- VoltTodo-V2 headless end-to-end: add/toggle, then reload from disk ---
mkdir -p tests/tmp
rm -f tests/tmp/todo2.txt
out=$(VS_HEADLESS=1 VS_EVENTS=tests/ui/todo2.events VYTO_TODO_FILE=tests/tmp/todo2.txt \
      ./vytoc run apps/todo2/todo2.vt 2>&1)
saved=$(cat tests/tmp/todo2.txt 2>/dev/null)
out2=$(VS_HEADLESS=1 VS_EVENTS=tests/ui/todo2_reload.events VYTO_TODO_FILE=tests/tmp/todo2.txt \
       ./vytoc run apps/todo2/todo2.vt 2>&1)
want_saved="0|milk
1|eggs"
if [ "$out" = "bye — 2 task(s) in tests/tmp/todo2.txt" ] && [ "$saved" = "$want_saved" ] &&
   [ "$out2" = "bye — 2 task(s) in tests/tmp/todo2.txt" ]; then
    echo "PASS todo2_e2e"
else
    echo "FAIL todo2_e2e"
    printf 'run1: %s\nsaved: %s\nrun2: %s\n' "$out" "$saved" "$out2"
    fail=1
fi

# --- VoltTodo app: regenerate bindings, golden-check the shim binding, build ---
if [ -f /usr/include/X11/Xlib.h ]; then
    ./vytobind apps/todo/x11/native/src/xshim.h --lib X11 \
        --filter 'xs_*' --filter 'EV_*' --filter 'KEY_*' > apps/todo/x11/xshim.vt || exit 1
    if diff -u tests/xshim.vt.expected apps/todo/x11/xshim.vt >/dev/null 2>&1; then
        echo "PASS vytobind_xshim"
    else
        echo "FAIL vytobind_xshim"
        diff -u tests/xshim.vt.expected apps/todo/x11/xshim.vt | head -30
        fail=1
    fi
    ./vytobind /usr/include/X11/Xlib.h \
        --filter 'XOpenDisplay' --filter 'XCloseDisplay' --filter 'XCreateSimpleWindow' \
        --filter 'XMapWindow' --filter 'XStoreName' --filter 'XFillRectangle' \
        --filter 'XDrawRectangle' --filter 'XDrawLine' --filter 'XDrawString' \
        --filter 'XSetForeground' --filter 'XFlush' --filter 'XFreeGC' \
        > apps/todo/x11/x11.vt || exit 1
    # apps/hn_android is deliberately absent from every check in this file. It
    # imports vyto/mobile/android/*, whose C shims are #ifdef __ANDROID__, so it
    # does not link for the host at all — and building it for its own triple
    # needs an NDK, which is exactly what this suite refuses to require so that
    # CI never downloads one. Its pipeline is apps/hn_android/build-apk.sh, run
    # by hand; the widgets underneath it are covered by tests/ui/*_mobile_*.vt
    # (make test-mobile), which do run everywhere.
    if ./vytoc build apps/todo/todo.vt >/dev/null; then
        echo "PASS app_todo_builds"
    else
        echo "FAIL app_todo_builds"
        fail=1
    fi
    if ./vytoc build apps/uidemo/uidemo.vt >/dev/null; then
        echo "PASS app_uidemo_builds"
    else
        echo "FAIL app_uidemo_builds"
        fail=1
    fi
    if ./vytoc build apps/gallery/gallery2.vt >/dev/null; then
        echo "PASS app_gallery2_builds"
    else
        echo "FAIL app_gallery2_builds"
        fail=1
    fi
    # skin gallery: every widget across all five skins (E4). Headless render must
    # emit a frame, exercising skin_vyto + the flagship path end to end.
    sg_bin=apps/skingallery/.vyto-cache/skingallery_test
    if ./vytoc build apps/skingallery/skingallery.vt -o "$sg_bin" >/dev/null 2>&1 &&
       [ "$(SKIN_SHOT=tests/tmp/skin.ppm VS_HEADLESS=1 "$sg_bin" 2>&1)" = "wrote tests/tmp/skin.ppm" ] &&
       [ -s tests/tmp/skin.ppm ]; then
        echo "PASS app_skingallery_renders"
    else
        echo "FAIL app_skingallery_renders"
        fail=1
    fi
    # datagrid: DataTable over the native columnar engine (synthetic 200k rows)
    dg_bin=apps/datagrid/.vyto-cache/datagrid_test
    if ./vytoc build apps/datagrid/datagrid.vt -o "$dg_bin" >/dev/null 2>&1; then
        printf 'close\n' > tests/tmp/datagrid.events
        if VS_HEADLESS=1 VS_EVENTS=tests/tmp/datagrid.events "$dg_bin" 2>&1 | grep -q "datagrid closed"; then
            echo "PASS app_datagrid_runs"
        else
            echo "FAIL app_datagrid_runs"
            fail=1
        fi
    else
        echo "FAIL app_datagrid_runs (build failed)"
        fail=1
    fi
    if ./vytoc build apps/notepad/notepad.vt >/dev/null; then
        echo "PASS app_notepad_builds"
    else
        echo "FAIL app_notepad_builds"
        fail=1
    fi
    # snake: build to a standalone native exe, then run THAT binary headless
    # (scripted ticks + keys + close) and require a clean exit — covers the
    # native-build path and the surface game loop (wait_timeout/EV_TIMER).
    mkdir -p tests/tmp
    snake_bin=apps/snake/.vyto-cache/snake_test
    if ./vytoc build apps/snake/snake.vt -o "$snake_bin" >/dev/null; then
        printf 'tick\nkey Up\ntick\nkey Right\ntick\ntick\nclose\n' > tests/tmp/snake.events
        if VS_HEADLESS=1 VS_EVENTS=tests/tmp/snake.events "$snake_bin" >/dev/null 2>&1; then
            echo "PASS app_snake_builds_runs"
        else
            echo "FAIL app_snake_builds_runs (native binary exited non-zero)"
            fail=1
        fi
    else
        echo "FAIL app_snake_builds_runs (build failed)"
        fail=1
    fi
    # chip8: build native exe, run the IBM ROM headless in debug mode, and
    # require it to decode through to the sprite-draw opcode (0xD015). Covers
    # Surface.blit, the FFI getenv/readfile ROM load, and the CPU decode loop.
    chip8_bin=apps/chip8/.vyto-cache/chip8_test
    if ./vytoc build apps/chip8/chip8.vt -o "$chip8_bin" >/dev/null; then
        printf 'tick\ntick\nclose\n' > tests/tmp/chip8.events
        got=$(CHIP8_DEBUG=1 VS_HEADLESS=1 VS_EVENTS=tests/tmp/chip8.events \
              "$chip8_bin" apps/chip8/ibm.ch8 2>&1)
        if echo "$got" | grep -q "OP=0xD015"; then
            echo "PASS app_chip8_runs_ibm"
        else
            echo "FAIL app_chip8_runs_ibm (draw opcode not reached)"
            fail=1
        fi
    else
        echo "FAIL app_chip8_runs_ibm (build failed)"
        fail=1
    fi
else
    echo "SKIP volttodo (no X11 headers)"
fi

# --- prebuilt .so deployment: exe must run from a copied-out directory ---
out=$(./vytoc build examples/09_prebuilt_so.vt) || fail=1
if [ -f "examples/.vyto-cache/libgreeter.so" ]; then
    echo "PASS so_deployed_next_to_exe"
else
    echo "FAIL so_deployed_next_to_exe"
    fail=1
fi

exit $fail
