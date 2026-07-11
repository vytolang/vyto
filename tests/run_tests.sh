#!/bin/sh
# Runs every examples/NN_*.vt and diffs stdout against examples/NN_*.expected.
# Also builds the greeter prebuilt-.so package and checks voltbind output.
set -u
cd "$(dirname "$0")/.."
fail=0

# --- prepare the greeter package: prebuilt .so + voltbind-generated binding ---
triple=linux-x64   # matches volt_triple() on this CI/dev box
mkdir -p "examples/greeter/native/$triple"
so="examples/greeter/native/$triple/libgreeter.so"
if [ ! -f "$so" ] || [ examples/greeter/csrc/greeter.c -nt "$so" ]; then
    cc -shared -fPIC -O2 -Wl,-soname,libgreeter.so \
       -o "$so" examples/greeter/csrc/greeter.c || exit 1
fi
./voltbind examples/greeter/csrc/greeter.h \
    --filter 'greeter_*' --filter 'GREET*' > examples/greeter/greeter.vt || exit 1
if diff -u tests/greeter.vt.expected examples/greeter/greeter.vt >/dev/null 2>&1; then
    echo "PASS voltbind_greeter"
else
    echo "FAIL voltbind_greeter (binding differs from tests/greeter.vt.expected)"
    diff -u tests/greeter.vt.expected examples/greeter/greeter.vt | head -40
    fail=1
fi

# --- cbwrap package binding (fn-pointer params must map to rawptr) ---
./voltbind examples/cbwrap/native/src/cbwrap.h --filter 'cb_*' > examples/cbwrap/cbwrap.vt || exit 1
if diff -u tests/cbwrap.vt.expected examples/cbwrap/cbwrap.vt >/dev/null 2>&1; then
    echo "PASS voltbind_cbwrap"
else
    echo "FAIL voltbind_cbwrap"
    diff -u tests/cbwrap.vt.expected examples/cbwrap/cbwrap.vt | head -20
    fail=1
fi

# --- cross-compile driver logic (host cc standing in as the cross compiler) ---
rm -rf examples/.volt-cache/linux-x64
if ./voltc build examples/01_hello.vt --target linux-x64 --cc cc >/dev/null &&
   [ -x examples/.volt-cache/linux-x64/01_hello ] &&
   [ "$(examples/.volt-cache/linux-x64/01_hello | head -1)" = "hello, volt" ]; then
    echo "PASS cross_target_cache"
else
    echo "FAIL cross_target_cache"
    fail=1
fi
if ./voltc run examples/01_hello.vt --target linux-arm64 2>&1 | grep -q "copy the binary"; then
    echo "PASS cross_run_refused"
else
    echo "FAIL cross_run_refused"
    fail=1
fi

# --- freestanding runtime profile: -DVT_NO_LIBC path builds, links, runs ---
# The runtime is compiled with no libc and reaches the host only through the six
# vt_host_* hooks; host_hooks.c bridges them to libc as a no-cross-toolchain
# stand-in. Also asserts the runtime object references zero libc symbols.
./voltc build examples/01_hello.vt --freestanding --cc gcc \
        -o examples/.volt-cache/libhello_fs.a >/dev/null 2>&1 &&
    cc -c examples/freestanding/host_hooks.c -o examples/.volt-cache/host_hooks.o 2>/dev/null &&
    cc examples/.volt-cache/host_hooks.o examples/.volt-cache/libhello_fs.a \
       -o examples/.volt-cache/hello_fs 2>/dev/null
if [ "$(examples/.volt-cache/hello_fs 2>/dev/null | head -1)" = "hello, volt" ] &&
   [ "$(examples/.volt-cache/hello_fs 2>/dev/null | sed -n 5p)" = "x*2 = 5" ]; then
    echo "PASS freestanding_build_run"
else
    echo "FAIL freestanding_build_run"
    fail=1
fi
if nm examples/.volt-cache/volt_rt_gcc_fs.o 2>/dev/null | grep ' U ' | grep -qv 'vt_host_'; then
    echo "FAIL freestanding_no_libc (runtime object references a libc symbol)"
    nm examples/.volt-cache/volt_rt_gcc_fs.o | grep ' U ' | grep -v 'vt_host_' | head
    fail=1
else
    echo "PASS freestanding_no_libc"
fi
if ./voltc run examples/01_hello.vt --freestanding 2>&1 | grep -q "not a runnable executable"; then
    echo "PASS freestanding_run_refused"
else
    echo "FAIL freestanding_run_refused"
    fail=1
fi

# --- stdlib search path: VOLT_HOME lib, stem-collision naming, local shadowing ---
got=$(VOLT_HOME=tests/fixtures/volthome ./voltc run tests/fixtures/libpath/main.vt 2>&1)
want="hello from VOLT_HOME lib
hello from the user module"
if [ "$got" = "$want" ]; then
    echo "PASS libpath_volthome"
else
    echo "FAIL libpath_volthome"
    printf '%s\n' "$got"
    fail=1
fi
got=$(VOLT_HOME=tests/fixtures/volthome ./voltc run tests/fixtures/libpath/shadow/main.vt 2>&1)
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
    got=$(./voltc run "$src" 2>&1)
    if [ ! -f "$expected" ]; then
        echo "SKIP $name (no .expected)"
        continue
    fi
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
    got=$(./voltc build "$src" 2>&1 | sed -n '1p' | sed 's/^[^ ]*: error:/error:/')
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
got=$(./voltc run tests/fixtures/overflow_trap.vt 2>&1)
if echo "$got" | grep -q "integer overflow in '+'"; then
    echo "PASS overflow_trap_debug"
else
    echo "FAIL overflow_trap_debug (expected a panic, got: $got)"
    fail=1
fi
got=$(./voltc run tests/fixtures/overflow_trap.vt --release 2>&1)
if echo "$got" | grep -q -- "-9223372036854775808"; then
    echo "PASS overflow_wrap_release"
else
    echo "FAIL overflow_wrap_release (expected wrap to i64 min, got: $got)"
    fail=1
fi

# --- command-line args: args() builtin sees what follows `--` ---
got=$(./voltc run tests/fixtures/args_echo.vt -- alpha beta 2>&1)
if [ "$got" = "$(printf 'n=2\nalpha\nbeta')" ]; then
    echo "PASS args_builtin"
else
    echo "FAIL args_builtin (got: $got)"
    fail=1
fi

# --- volt/gfx: blend2d Canvas -> blitPtr (gated on the prebuilt lib) ---
if [ -f lib/volt/gfx/native/linux-x64/libblend2d.so ]; then
    gfx_bin=apps/gfxdemo/.volt-cache/gfxdemo_test
    if ./voltc build apps/gfxdemo/gfxdemo.vt -o "$gfx_bin" >/dev/null 2>&1; then
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
    # volt/ui rendered through GfxPainter (blend2d rich tier)
    uigfx_bin=apps/uigfx/.volt-cache/uigfx_test
    if ./voltc build apps/uigfx/uigfx.vt -o "$uigfx_bin" >/dev/null 2>&1; then
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
    # --bundle: one self-contained exe, no libblend2d.so/libstdc++ alongside
    if [ -f lib/volt/gfx/native/linux-x64/libblend2d.a ]; then
        bnd_bin=apps/uigfx/.volt-cache/uigfx_bundled
        if ./voltc build apps/uigfx/uigfx.vt --bundle -o "$bnd_bin" >/dev/null 2>&1 &&
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
    # Image/Avatar's http:// loading path — see volt/gfx/painter.vt's
    # load_image_url and lib/volt/net.
    got=$(./voltc run tests/fixtures/gfx_load_bytes.vt 2>&1)
    if [ "$got" = "w=300 h=300" ]; then
        echo "PASS gfx_load_bytes"
    else
        echo "FAIL gfx_load_bytes (got: $got)"
        fail=1
    fi
else
    echo "SKIP gfx_canvas_blit (no libblend2d — run lib/volt/gfx/native/build-blend2d.sh)"
fi

# --- readfile/readlines on a growable /proc file (size 0 by stat) ---
if [ -r /proc/self/status ]; then
    got=$(./voltc run tests/fixtures/proc_read.vt 2>&1)
    if [ "$got" = "ok" ]; then
        echo "PASS proc_read"
    else
        echo "FAIL proc_read (got: $got)"
        fail=1
    fi
fi

# --- volt/surface binding: regenerate with voltbind, golden-check ---
./voltbind lib/volt/surface/native/src/vsurf.h \
    --lib X11@linux --lib X11@macos --lib gdi32@windows --lib user32@windows \
    --filter 'vs_*' --filter 'VS_*' > lib/volt/surface/vsurf.vt || exit 1
if diff -u tests/vsurf.vt.expected lib/volt/surface/vsurf.vt >/dev/null 2>&1; then
    echo "PASS voltbind_vsurf"
else
    echo "FAIL voltbind_vsurf"
    diff -u tests/vsurf.vt.expected lib/volt/surface/vsurf.vt | head -30
    fail=1
fi

# --- conditional #link: platform-filtered libraries stay off the link line ---
got=$(./voltc run tests/fixtures/condlink.vt 2>&1)
if [ "$got" = "conditional link ok" ]; then
    echo "PASS condlink"
else
    echo "FAIL condlink"
    printf '%s\n' "$got"
    fail=1
fi

# --- volt/ui headless golden tests (VS_HEADLESS backend, scripted events) ---
for src in tests/ui/[0-9]*.vt; do
    name=$(basename "$src" .vt)
    got=$(VS_HEADLESS=1 VS_EVENTS="tests/ui/$name.events" ./voltc run "$src" 2>&1)
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

# --- volt/ui named golden tests: TextArea + menus (no env needed) ---
for name in textarea menu gallery; do
    got=$(VS_HEADLESS=1 VS_EVENTS="tests/ui/$name.events" ./voltc run "tests/ui/$name.vt" 2>&1)
    if [ "$got" = "$(cat "tests/ui/$name.expected")" ]; then
        echo "PASS ui_$name"
    else
        echo "FAIL ui_$name"
        echo "--- expected ---"; cat "tests/ui/$name.expected"
        echo "--- got ---"; printf '%s\n' "$got"
        fail=1
    fi
done

# --- framebuffer backend, file target: render a known pattern into a raw
#     XRGB8888 file and verify it byte-for-byte (no display or fb device) ---
mkdir -p tests/tmp
rm -f tests/tmp/fb.raw
render=$(VS_FBDEV=tests/tmp/fb.raw VS_FB_W=4 VS_FB_H=4 \
         ./voltc run tests/ui/fb_render.vt 2>&1)
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
got=$(./voltc run tests/fixtures/listdir.vt 2>&1)
want=".. (dir)
a.txt (file)
b.txt (file)
sub (dir)"
if [ "$got" = "$want" ]; then
    echo "PASS listdir"
else
    echo "FAIL listdir"; printf '%s\n' "$got"; fail=1
fi
got=$(VS_HEADLESS=1 VS_EVENTS=tests/ui/filepicker.events VOLT_PICK_DIR=tests/tmp/pickdir \
      ./voltc run tests/ui/filepicker.vt 2>&1)
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
out=$(VS_HEADLESS=1 VS_EVENTS=tests/ui/notepad_save.events VOLT_NOTE_DIR=tests/tmp/notedir \
      ./voltc run apps/notepad/notepad.vt 2>&1)
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
out=$(VS_HEADLESS=1 VS_EVENTS=tests/ui/todo2.events VOLT_TODO_FILE=tests/tmp/todo2.txt \
      ./voltc run apps/todo2/todo2.vt 2>&1)
saved=$(cat tests/tmp/todo2.txt 2>/dev/null)
out2=$(VS_HEADLESS=1 VS_EVENTS=tests/ui/todo2_reload.events VOLT_TODO_FILE=tests/tmp/todo2.txt \
       ./voltc run apps/todo2/todo2.vt 2>&1)
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
    ./voltbind apps/todo/x11/native/src/xshim.h --lib X11 \
        --filter 'xs_*' --filter 'EV_*' --filter 'KEY_*' > apps/todo/x11/xshim.vt || exit 1
    if diff -u tests/xshim.vt.expected apps/todo/x11/xshim.vt >/dev/null 2>&1; then
        echo "PASS voltbind_xshim"
    else
        echo "FAIL voltbind_xshim"
        diff -u tests/xshim.vt.expected apps/todo/x11/xshim.vt | head -30
        fail=1
    fi
    ./voltbind /usr/include/X11/Xlib.h \
        --filter 'XOpenDisplay' --filter 'XCloseDisplay' --filter 'XCreateSimpleWindow' \
        --filter 'XMapWindow' --filter 'XStoreName' --filter 'XFillRectangle' \
        --filter 'XDrawRectangle' --filter 'XDrawLine' --filter 'XDrawString' \
        --filter 'XSetForeground' --filter 'XFlush' --filter 'XFreeGC' \
        > apps/todo/x11/x11.vt || exit 1
    if ./voltc build apps/todo/todo.vt >/dev/null; then
        echo "PASS app_todo_builds"
    else
        echo "FAIL app_todo_builds"
        fail=1
    fi
    if ./voltc build apps/uidemo/uidemo.vt >/dev/null; then
        echo "PASS app_uidemo_builds"
    else
        echo "FAIL app_uidemo_builds"
        fail=1
    fi
    if ./voltc build apps/notepad/notepad.vt >/dev/null; then
        echo "PASS app_notepad_builds"
    else
        echo "FAIL app_notepad_builds"
        fail=1
    fi
    # snake: build to a standalone native exe, then run THAT binary headless
    # (scripted ticks + keys + close) and require a clean exit — covers the
    # native-build path and the surface game loop (wait_timeout/EV_TIMER).
    mkdir -p tests/tmp
    snake_bin=apps/snake/.volt-cache/snake_test
    if ./voltc build apps/snake/snake.vt -o "$snake_bin" >/dev/null; then
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
    chip8_bin=apps/chip8/.volt-cache/chip8_test
    if ./voltc build apps/chip8/chip8.vt -o "$chip8_bin" >/dev/null; then
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
out=$(./voltc build examples/09_prebuilt_so.vt) || fail=1
if [ -f "examples/.volt-cache/libgreeter.so" ]; then
    echo "PASS so_deployed_next_to_exe"
else
    echo "FAIL so_deployed_next_to_exe"
    fail=1
fi

exit $fail
