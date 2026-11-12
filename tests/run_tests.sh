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

# Parallel section runner: par_begin / par_run / par_finish, plus TEST_JOBS.
# Extracted so tests/run_tests_dev.sh can use the same helpers rather than
# forking them — the xargs -d '\n' detail in there is easy to lose in a copy.
. tests/lib_par.sh

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

# --- --with-assets: a symlink in the tree must not hang the build ---
#
# emit_asset_dir used to detect a directory by "opendir() succeeded", which is
# also true of a symlink TO a directory -- so a link pointing at an ancestor
# recursed until the build died. pnpm and yarn workspaces build node_modules
# out of symlinks, so this is a normal thing to find beside an app.
#
# Fault-injected: reverting the lstat guard makes this case hang, which is why
# it is wrapped in a timeout rather than merely checked for exit status.
asset_dir=tests/tmp/assetloop
rm -rf "$asset_dir"
mkdir -p "$asset_dir/assets/deep"
printf 'real\n'   > "$asset_dir/assets/real.txt"
printf 'nested\n' > "$asset_dir/assets/deep/nested.txt"
ln -s ../../assets "$asset_dir/assets/deep/loop"     # points at an ancestor
ln -s /etc         "$asset_dir/assets/etclink"       # and outside the tree
cat > "$asset_dir/main.vt" <<'ASSETEOF'
import { assetExists } from "vyto/asset";
fn main() {
    print("real=" + assetExists("assets/real.txt"));
    print("nested=" + assetExists("assets/deep/nested.txt"));
}
ASSETEOF
if timeout 120 ./vytoc build "$asset_dir/main.vt" -o "$asset_dir/app" \
       --with-assets >/dev/null 2>&1 \
   && [ "$("$asset_dir/app" 2>/dev/null)" = "real=true
nested=true" ]; then
    echo "PASS asset_symlink_loop"
else
    echo "FAIL asset_symlink_loop (build hung, or assets missing)"
    fail=1
fi
rm -rf "$asset_dir"

# --- vt_vfs_get_exact: a suffix must NOT reach an embedded asset ---
#
# vt_vfs_get matches when the registered key is a path-component tail of the
# query, so a stored "assets/404.html" also answers "/anything/assets/404.html".
# That is fine for code holding a longer path and wrong for a server passing a
# REQUEST path to the registry: a URL that merely ends with an embedded name
# would serve the embedded blob.
#
# assetBytesExact/assetExistsExact are the whole-string forms. This pins both
# halves -- the suffix arm still works through assetExists, and does not through
# assetExistsExact -- because a test that only checked the new function would
# pass if the exact lookup were wired to the old one.
exact_dir=tests/tmp/vfsexact
rm -rf "$exact_dir"
mkdir -p "$exact_dir/assets"
printf 'FLOOR\n' > "$exact_dir/assets/404.html"
cat > "$exact_dir/main.vt" <<'EXACTEOF'
import { assetExists, assetExistsExact, assetBytes, assetBytesExact } from "vyto/asset";
fn main() {
    // The key as registered: both forms find it.
    print("exact_hit=" + assetExistsExact("assets/404.html"));
    print("suffix_hit=" + assetExists("assets/404.html"));

    // A request-shaped path ending in the key. The suffix arm matches it; the
    // exact one must not.
    print("suffix_reaches=" + assetExists("/evil/assets/404.html"));
    print("exact_reaches=" + assetExistsExact("/evil/assets/404.html"));

    // And the bytes follow the same rule -- a miss is empty, not the floor.
    print("exact_bytes=" + str(assetBytesExact("assets/404.html").len));
    print("suffix_bytes=" + str(assetBytesExact("/evil/assets/404.html").len));
}
EXACTEOF
if timeout 120 ./vytoc build "$exact_dir/main.vt" -o "$exact_dir/app" \
       --with-assets >/dev/null 2>&1 \
   && [ "$("$exact_dir/app" 2>/dev/null)" = "exact_hit=true
suffix_hit=true
suffix_reaches=true
exact_reaches=false
exact_bytes=6
suffix_bytes=0" ]; then
    echo "PASS vfs_get_exact"
else
    echo "FAIL vfs_get_exact (suffix match not confined, or exact lookup broken)"
    "$exact_dir/app" 2>&1 | head -10
    fail=1
fi
rm -rf "$exact_dir"

# --- res.sendBytes: an embedded body served without copying it to a string ---
#
# body_str would mean str_alloc + memcpy of the asset on every request
# (runtime/vyto_rt.c:854) for bytes that never change. sendBytes frames from the
# pointer length and appends straight from .rodata.
#
# HEAD is the case worth pinning: it must advertise the length a GET would have
# produced and send no body. A body source that ignores body_allowed desyncs the
# connection, which is the failure this whole framing is careful about.
sb_dir=tests/tmp/sendbytes
rm -rf "$sb_dir"
mkdir -p "$sb_dir/assets"
printf 'FLOOR\n' > "$sb_dir/assets/404.html"
cat > "$sb_dir/main.vt" <<'SBEOF'
import { server } from "vyto/net/server";
import { socket_connect } from "vyto/net/socket";

extern "C" {
    fn vt_vfs_ptr(key: cstring): rawptr;
    fn vt_vfs_size(key: cstring): i64;
}

fn main() {
    let s = server(0);
    s.withHandler((req, res) => {
        let p = vt_vfs_ptr("assets/404.html".cstr());
        let n = vt_vfs_size("assets/404.html".cstr()) as int;
        if (p == null) { res.text(500, "no asset"); return; }
        res.sendBytes(200, p, n);
    });
    let port = s.bind();
    if (port <= 0) { print("bind failed"); return; }
    for (let v in ["GET", "HEAD"]) {
        let c = socket_connect("127.0.0.1", port);
        c.setTimeoutMs(3000);
        c.sendText(v + " /x HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        s.serveOnce();
        let r = c.recvText(65536);
        c.close();
        print(v + " len=" + str(r.contains("Content-Length: 6")));
        print(v + " body=" + str(r.contains("FLOOR")));
    }
}
SBEOF
if timeout 120 ./vytoc build "$sb_dir/main.vt" -o "$sb_dir/app" \
       --with-assets >/dev/null 2>&1 \
   && [ "$("$sb_dir/app" 2>/dev/null)" = "GET len=true
GET body=true
HEAD len=true
HEAD body=false" ]; then
    echo "PASS res_send_bytes"
else
    echo "FAIL res_send_bytes (framing wrong, or HEAD sent a body)"
    "$sb_dir/app" 2>&1 | head -10
    fail=1
fi
rm -rf "$sb_dir"

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
rm -rf examples/.vyto-cache/01_hello/linux-x64
if ./vytoc build examples/01_hello.vt --target linux-x64 --cc cc >/dev/null &&
   [ -x examples/.vyto-cache/01_hello/linux-x64/01_hello ] &&
   [ "$(examples/.vyto-cache/01_hello/linux-x64/01_hello | head -1)" = "hello, vyto" ]; then
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
# Same shape for vyto/crypto/openssl, and it needs its own fixture for the same
# reason: nothing else in the suite compiles that shim without a libc, so its
# stub arm is the kind of thing that rots without anyone noticing until a
# freestanding build of an unrelated program stops linking.
if ./vytoc build tests/fixtures/openssl_freestanding.vt --freestanding --cc gcc \
        -o examples/.vyto-cache/libossl_fs.a >/dev/null 2>&1; then
    echo "PASS freestanding_openssl_stub"
else
    echo "FAIL freestanding_openssl_stub (vyto/crypto/openssl's VT_NO_LIBC arm does not build)"
    fail=1
fi
# And for vyto/media/image, whose shim instantiates stb_image_write — a header
# that needs malloc, stdio and sprintf, so an unguarded include would break the
# freestanding build of anything importing the package. The fixture imports only
# the encoder, never vyto/media/image/decode, which reaches blend2d (C++) and
# could never build here; that module split is what makes this test possible.
if ./vytoc build tests/fixtures/image_freestanding.vt --freestanding --cc gcc \
        -o examples/.vyto-cache/libimg_fs.a >/dev/null 2>&1; then
    echo "PASS freestanding_image_stub"
else
    echo "FAIL freestanding_image_stub (vyto/media/image's VT_NO_LIBC arm does not build)"
    fail=1
fi
# vyto/win/d2d off Windows. Its shim is all COM and Direct2D behind
# `#if defined(_WIN32) && !defined(VT_NO_LIBC)`; everywhere else a stub arm
# answers. Two checks, because they catch different rot: the hosted run proves
# the package links and reports itself unavailable; the freestanding build
# proves no include escaped the guard. The archive check reads nm rather than
# trusting the exit status: ar resolves nothing, so a broken guard would still
# "build". The stub object must reference no external symbol at all.
got=$(./vytoc run tests/fixtures/d2d_stub.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/d2d_stub.expected)" ]; then
    echo "PASS d2d_stub"
else
    echo "FAIL d2d_stub"
    echo "--- expected ---"; cat tests/fixtures/d2d_stub.expected
    echo "--- got ---"; echo "$got"
    fail=1
fi
if ./vytoc build tests/fixtures/d2d_stub.vt --freestanding --cc gcc \
        -o examples/.vyto-cache/libd2d_fs.a >/dev/null 2>&1; then
    d2d_obj=$(nm -A examples/.vyto-cache/libd2d_fs.a 2>/dev/null \
        | awk '/ T d2d_ok$/ { split($1, a, ":"); print a[2]; exit }')
    d2d_undef=$(nm -A examples/.vyto-cache/libd2d_fs.a 2>/dev/null \
        | grep -c ":$d2d_obj: *U ")
    if [ -n "$d2d_obj" ] && [ "$d2d_undef" -eq 0 ]; then
        echo "PASS freestanding_d2d_stub"
    else
        echo "FAIL freestanding_d2d_stub (shim object '$d2d_obj' has $d2d_undef undefined symbols)"
        fail=1
    fi
else
    echo "FAIL freestanding_d2d_stub (vyto/win/d2d's stub arm does not build freestanding)"
    fail=1
fi
# native/<triple>/src and native/<triple>/stage (src/main.c). Each triple dir
# of the fixture defines its own pt_target(), so the string that links says
# which dir compiled; a Linux build must contain none of the Windows source and
# must not receive the Windows-only staged file. The edit check copies the
# fixture so a per-triple source change is proven to reach the object (it keys
# on its own tree, not native/src's).
got=$(./vytoc run tests/fixtures/pertriple_main.vt 2>&1)
if [ "$got" = "common + linux-x64 src" ]; then
    echo "PASS pertriple_host"
else
    echo "FAIL pertriple_host (got: $got)"
    fail=1
fi
ptd=tests/tmp/pertriple; rm -rf "$ptd"; mkdir -p "$ptd/bin"
if ./vytoc build tests/fixtures/pertriple_main.vt -o "$ptd/bin/pt" >/dev/null 2>&1 \
        && ! grep -aq PT_WIN_MARKER "$ptd/bin/pt" && [ ! -e "$ptd/bin/pt_staged.txt" ]; then
    echo "PASS pertriple_host_excludes_windows"
else
    echo "FAIL pertriple_host_excludes_windows (Windows-only source or staged file reached a Linux build)"
    fail=1
fi
if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    r=bad
    if ./vytoc build tests/fixtures/pertriple_main.vt --target windows-x64 \
            -o "$ptd/bin/pt.exe" >/dev/null 2>&1 \
            && grep -aq PT_WIN_MARKER "$ptd/bin/pt.exe" \
            && ! grep -aq "linux-x64 src" "$ptd/bin/pt.exe" \
            && [ -f "$ptd/bin/pt_staged.txt" ]; then
        r=ok
    fi
    [ $r = ok ] && echo "PASS pertriple_windows" || { echo "FAIL pertriple_windows"; fail=1; }
    cp -r tests/fixtures/pertriple tests/fixtures/pertriple_main.vt "$ptd/"
    sed -i 's/PT_WIN_MARKER/PT_WIN_EDITED/' "$ptd/pertriple/native/windows-x64/src/pt_target.c"
    if ./vytoc build "$ptd/pertriple_main.vt" --target windows-x64 \
            -o "$ptd/bin/pt2.exe" >/dev/null 2>&1 \
            && grep -aq PT_WIN_EDITED "$ptd/bin/pt2.exe"; then
        echo "PASS pertriple_edit_recompiles"
    else
        echo "FAIL pertriple_edit_recompiles (an edit under native/windows-x64/src served a stale object)"
        fail=1
    fi
else
    echo "SKIP pertriple_windows (no mingw-w64)"
fi
# file_mkdirs with backslashes: one folder name on POSIX (its Windows golden,
# nested folders, runs in the staged Windows suite). Before the fix Windows
# made nothing at all, since only '/' split components — every app that made
# a folder under dataDir()/configDir() silently failed to save there.
rm -rf tests/tmp/mkbs*
got=$(./vytoc run tests/fixtures/mkdirs_backslash.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/mkdirs_backslash.expected)" ]; then
    echo "PASS mkdirs_backslash"
else
    echo "FAIL mkdirs_backslash (got: $got)"
    fail=1
fi
# --- vyto/desktop/service ---
# As an ordinary program: SIGTERM (what systemd sends) must reach stopping()
# and wake a 60 s wait() at once. Under a NOTIFY_SOCKET it must also report
# service mode and send READY=1 then STOPPING=1 — checked with a datagram
# listener standing in for systemd. INVOCATION_ID is stripped: it leaks into
# whole desktop sessions and must not count as "started by systemd".
sv=tests/tmp/service; rm -rf "$sv"; mkdir -p "$sv"
if ./vytoc build tests/fixtures/service_console.vt -o "$sv/svc" >/dev/null 2>&1; then
    env -u INVOCATION_ID -u NOTIFY_SOCKET "$sv/svc" > "$sv/plain.txt" 2>&1 &
    sp=$!; sleep 1; kill -TERM "$sp" 2>/dev/null; wait "$sp"
    if [ "$(cat "$sv/plain.txt")" = "service mode: false
running
stopped after 1 tick(s), early: true
exited cleanly" ]; then
        echo "PASS service_sigterm"
    else
        echo "FAIL service_sigterm (got: $(cat "$sv/plain.txt"))"; fail=1
    fi
    python3 - "$sv/notify.sock" > "$sv/notify.txt" 2>&1 <<'PY' &
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
s.bind(sys.argv[1]); s.settimeout(10)
try:
    for _ in range(2): print(s.recv(256).decode(), flush=True)
except Exception as e:
    print("ERR", e)
PY
    lp=$!; sleep 0.5
    env -u INVOCATION_ID NOTIFY_SOCKET="$(pwd)/$sv/notify.sock" "$sv/svc" > "$sv/notified.txt" 2>&1 &
    sp=$!; sleep 1; kill -TERM "$sp" 2>/dev/null; wait "$sp"; wait "$lp"
    if head -1 "$sv/notified.txt" | grep -qx "service mode: true" &&
       [ "$(cat "$sv/notify.txt")" = "READY=1
STOPPING=1" ]; then
        echo "PASS service_sd_notify"
    else
        echo "FAIL service_sd_notify (notify: $(cat "$sv/notify.txt"))"; fail=1
    fi
else
    echo "FAIL service_sigterm (build)"; fail=1
fi
# The systemd unit, and install/state/uninstall into a temp unit dir.
rm -rf "$sv/units"; mkdir -p "$sv/units"
got=$(VYTO_SYSTEMD_DIR="$(pwd)/$sv/units" ./vytoc run tests/fixtures/service_unit.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/service_unit.expected)" ]; then
    echo "PASS service_unit"
else
    echo "FAIL service_unit"; echo "$got" | diff tests/fixtures/service_unit.expected - | head; fail=1
fi
if ./vytoc build tests/fixtures/service_freestanding.vt --freestanding --cc gcc \
        -o examples/.vyto-cache/libsvc_fs.a >/dev/null 2>&1; then
    sobj=$(nm -A examples/.vyto-cache/libsvc_fs.a 2>/dev/null \
        | awk '/ T vsvc_state$/ { split($1, a, ":"); print a[2]; exit }')
    sund=$(nm -A examples/.vyto-cache/libsvc_fs.a 2>/dev/null | grep -c ":$sobj: *U ")
    if [ -n "$sobj" ] && [ "$sund" -eq 0 ]; then
        echo "PASS freestanding_service_stub"
    else
        echo "FAIL freestanding_service_stub ('$sobj' has $sund undefined symbols)"; fail=1
    fi
else
    echo "FAIL freestanding_service_stub (does not build freestanding)"; fail=1
fi
# --- vytoc --installer: the payload format, the manifest's service, flags ---
rm -rf tests/tmp/setup_pack; mkdir -p tests/tmp/setup_pack
got=$(./vytoc run tests/fixtures/setup_pack.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/setup_pack.expected)" ]; then
    echo "PASS setup_pack_roundtrip"
else
    echo "FAIL setup_pack_roundtrip"; echo "$got" | diff tests/fixtures/setup_pack.expected - | head; fail=1
fi
out=$(./vytoc run tests/fixtures/win_bad_service_manifest.vt 2>&1)
if [ $? -ne 0 ] && echo "$out" | grep -q 'service name "bad name!"' &&
   echo "$out" | grep -q 'service start "sometimes"'; then
    echo "PASS win_manifest_service_refused"
else
    echo "FAIL win_manifest_service_refused (got: $out)"; fail=1
fi
out=$(./vytoc build tests/fixtures/win_console_app.vt --installer 2>&1)
out2=$(./vytoc build tests/fixtures/win_console_app.vt --target windows-x64 --per-user 2>&1)
if echo "$out" | grep -q "makes a Windows setup.exe" && echo "$out2" | grep -q "pass --installer too"; then
    echo "PASS installer_flags_refused"
else
    echo "FAIL installer_flags_refused (got: $out | $out2)"; fail=1
fi
# --- vyto/desktop: dialogs, open, per-user folders ---
# The dialog programs are fakes (tests/fixtures/desktop/fake_dialog.sh) that
# log every argv element and replay canned answers, so each backend's exact
# command line is checked against a golden: the same fixture once as zenity,
# once as kdialog, once with no program at all. The hostile-title call is the
# shell-injection check — the title must arrive as one argument, unchanged,
# and the file it tries to create must not exist afterwards.
dk=tests/tmp/desktop; rm -rf "$dk"
for b in zenity kdialog none; do
    bd=$dk/$b; mkdir -p "$bd/bin"
    if [ "$b" = none ]; then
        prog=/nonexistent/zenity; fdir=
    else
        cp tests/fixtures/desktop/fake_dialog.sh "$bd/bin/$b"
        cp tests/fixtures/desktop/replies.txt "$bd/replies"
        prog=$(pwd)/$bd/bin/$b; fdir=$(pwd)/$bd
    fi
    got=$(cd "$bd" && FAKE_DIR=$fdir VYTO_DESKTOP_DIALOG=$prog \
          ../../../../vytoc run ../../../../tests/fixtures/desktop_dialogs.vt 2>&1)
    if [ "$got" = "$(cat tests/fixtures/desktop_dialogs.$b.expected)" ] && [ ! -e "$bd/pwned" ]; then
        echo "PASS desktop_dialogs_$b"
    else
        echo "FAIL desktop_dialogs_$b"
        echo "$got" | diff tests/fixtures/desktop_dialogs.$b.expected - | head -20
        fail=1
    fi
done
# zenity 4 (GTK 4) refuses the options it dropped; they must not be passed.
bd=$dk/zenity4; mkdir -p "$bd/bin"
cp tests/fixtures/desktop/fake_dialog.sh "$bd/bin/zenity"
cp tests/fixtures/desktop/replies.txt "$bd/replies"
(cd "$bd" && FAKE_ZENITY_VERSION=4.0.1 FAKE_DIR=$(pwd) VYTO_DESKTOP_DIALOG=$(pwd)/bin/zenity \
    ../../../../vytoc run ../../../../tests/fixtures/desktop_dialogs.vt >/dev/null 2>&1)
if [ -s "$bd/log" ] && ! grep -q -- "--attach\|--confirm-overwrite" "$bd/log"; then
    echo "PASS desktop_zenity4_flags"
else
    echo "FAIL desktop_zenity4_flags (GTK 3-only options passed to zenity 4)"
    fail=1
fi
# openUrl: refused schemes never reach xdg-open; accepted ones arrive whole.
# xdg-open runs detached, so give its log a moment to appear.
bd=$dk/open; mkdir -p "$bd/bin"
cp tests/fixtures/desktop/fake_dialog.sh "$bd/bin/xdg-open"
got=$(cd "$bd" && FAKE_DIR=$(pwd) VYTO_DESKTOP_OPEN=$(pwd)/bin/xdg-open \
      ../../../../vytoc run ../../../../tests/fixtures/desktop_open.vt 2>&1)
for _ in 1 2 3 4 5 6 7 8 9 10; do
    [ "$(grep -c '^xdg-open:' "$bd/log" 2>/dev/null)" -ge 3 ] && break
    sleep 0.2
done
want="https: true
mailto: true
javascript: false
file: false
bare path: false
empty: false
scheme only: false
newline: false
dash path: true"
if [ "$got" = "$want" ] && [ "$(grep -c '^xdg-open:' "$bd/log")" -eq 3 ] &&
   grep -qxF "  [https://example.com/a b?q=1&x='y']" "$bd/log" &&
   grep -qxF "  [./-x]" "$bd/log" && ! grep -q "javascript\|passwd\|xterm" "$bd/log"; then
    echo "PASS desktop_open_url"
else
    echo "FAIL desktop_open_url (got: $got)"; cat "$bd/log" 2>/dev/null
    fail=1
fi
# Per-user folders: XDG values win when absolute, a relative one is ignored
# (the spec says so), and an app name that is not a plain name gives "".
got=$(env -u XDG_CONFIG_HOME -u XDG_CACHE_HOME HOME=/home/t XDG_DATA_HOME=relative/x \
      ./vytoc run tests/fixtures/desktop_dirs.vt 2>&1)
got2=$(XDG_CONFIG_HOME=/cfg XDG_DATA_HOME=/dat XDG_CACHE_HOME=/c HOME=/home/t \
       ./vytoc run tests/fixtures/desktop_dirs.vt 2>&1 | head -3 | tr '\n' ' ')
if [ "$got" = "config: /home/t/.config/myapp
data: /home/t/.local/share/myapp
cache: /home/t/.cache/myapp
dotdot: []
slash: []
empty: []
colon: []" ] && [ "$got2" = "config: /cfg/myapp data: /dat/myapp cache: /c/myapp " ]; then
    echo "PASS desktop_dirs"
else
    echo "FAIL desktop_dirs (got: $got | $got2)"
    fail=1
fi
# Freestanding: the stub arm must build with no libc, and its object must
# reference nothing external (nm, not the exit status — see d2d above).
if ./vytoc build tests/fixtures/desktop_freestanding.vt --freestanding --cc gcc \
        -o examples/.vyto-cache/libdesk_fs.a >/dev/null 2>&1; then
    dobj=$(nm -A examples/.vyto-cache/libdesk_fs.a 2>/dev/null \
        | awk '/ T vdesk_available$/ { split($1, a, ":"); print a[2]; exit }')
    dund=$(nm -A examples/.vyto-cache/libdesk_fs.a 2>/dev/null | grep -c ":$dobj: *U ")
    if [ -n "$dobj" ] && [ "$dund" -eq 0 ]; then
        echo "PASS freestanding_desktop_stub"
    else
        echo "FAIL freestanding_desktop_stub (shim object '$dobj' has $dund undefined symbols)"
        fail=1
    fi
else
    echo "FAIL freestanding_desktop_stub (vyto/desktop's stub arm does not build freestanding)"
    fail=1
fi
# And for vyto/compress, whose vendored zlib is built on an allocator — there is
# nothing to degrade to, so the wrapper TUs compile to nothing under VT_NO_LIBC
# and zlib_shim.c's stub arm answers instead. Without that guard, merely
# importing vyto/compress would break any freestanding build.
if ./vytoc build tests/fixtures/compress_freestanding.vt --freestanding --cc gcc \
        -o examples/.vyto-cache/libcompress_fs.a >/dev/null 2>&1; then
    echo "PASS freestanding_compress_stub"
else
    echo "FAIL freestanding_compress_stub (vyto/compress's VT_NO_LIBC arm does not build)"
    fail=1
fi
# And for vyto/terminal, whose shim includes <termios.h>, <sys/ioctl.h> and
# <poll.h> on the POSIX arm — three hosted headers that would break the
# freestanding build of anything importing the package. The stub arm reports
# "not a terminal", which is a state the hosted arms reach too (under a pipe),
# so terminal.vt needs no platform conditional and this degrades to the same
# path examples/100_terminal.vt already covers.
#
# This one also checks the SYMBOLS, not just the exit status, and the extra
# check is not belt-and-braces. Fault injection showed that bypassing the
# VT_NO_LIBC guard still builds green: the output is an ARCHIVE, and `ar` never
# resolves anything, so the POSIX arm's undefined tcgetattr/poll/ioctl sit in
# the .a unnoticed. An exit-status-only check here proves only that the
# compiler ran. Asserting that no hosted-libc symbol survived is what actually
# tests the stub arm.
if ./vytoc build tests/fixtures/terminal_freestanding.vt --freestanding --cc gcc \
        -o examples/.vyto-cache/libterm_fs.a >/dev/null 2>&1; then
    if nm examples/.vyto-cache/libterm_fs.a 2>/dev/null \
            | grep -qE '^ +U (tcgetattr|tcsetattr|poll|ioctl)$'; then
        echo "FAIL freestanding_terminal_stub (POSIX arm compiled; VT_NO_LIBC guard bypassed)"
        fail=1
    else
        echo "PASS freestanding_terminal_stub"
    fi
else
    echo "FAIL freestanding_terminal_stub (vyto/terminal's VT_NO_LIBC arm does not build)"
    fail=1
fi
# Does vyto/terminal put the terminal back when the process is KILLED? Raw mode
# turns off echo, so a program that dies without restoring leaves the user
# typing blind into a shell that shows nothing back.
#
# Three exit paths, each of which was found broken by measurement in this order:
# deinit misses panic() (exit(101) unwinds nothing), atexit misses Ctrl+C
# (SIGINT runs no atexit handlers), and Ctrl+C is how people actually quit an
# interactive program. A regression is invisible in every other test — the
# program still works — until a user presses Ctrl+C.
#
# Needs a pty, since the state under test is a dead process's terminal, so it is
# a Python helper. Skipped where python3 is absent rather than failing the suite.
if command -v python3 >/dev/null 2>&1; then
    if python3 tests/terminal_signal_check.py; then
        :
    else
        fail=1
    fi
else
    echo "SKIP terminal_signal_check (no python3)"
fi
# Every extern "C" binding must agree with its shim on integer width on every
# target. C `long` is 64-bit here and 32-bit on Windows (LLP64), so a shim
# returning `long` to a Vyto `i64` passes every Linux test and returns
# 4294967295 for -1 on Windows. 153 such bindings shipped and were found only
# by running the suite on a real Windows 7 box; a Linux build cannot show it,
# so this compares the declarations instead.
if command -v python3 >/dev/null 2>&1; then
    if out=$(python3 tests/ffi_width_check.py 2>&1); then
        echo "PASS ffi_width_check (${out##*: })"
    else
        echo "FAIL ffi_width_check"
        echo "$out"
        fail=1
    fi
else
    echo "SKIP ffi_width_check (no python3)"
fi
# vyto/db is pure Vyto and must stay freestanding-safe; its SQLite subpackage is
# the one part that is not, so db_shim.c carries a VT_NO_LIBC arm that drops the
# whole library and returns failure sentinels. Both halves are checked because
# only the second one can rot silently.
#
# NOTE: do NOT extend this to vyto/db/pgsql. That driver reaches the network
# through vyto/net/socket, whose socket_shim.c has no VT_NO_LIBC arm at all, so
# a freestanding build of anything importing it cannot link. That is expected,
# not a regression: the fixture below imports vyto/db and no driver, which is
# exactly the surface that has to stay freestanding. Adding such a check would
# cost an afternoon and prove nothing.
if ./vytoc build tests/fixtures/db_nodriver.vt --freestanding --cc gcc \
        -o examples/.vyto-cache/libdb_fs.a >/dev/null 2>&1; then
    echo "PASS freestanding_db"
else
    echo "FAIL freestanding_db (vyto/db no longer builds freestanding)"
    fail=1
fi

# --- vyto/db does not drag a driver in ---
# THE architectural invariant of the package: native sources compile per package
# DIRECTORY for every module in the import closure, so a vyto/db that imported
# its drivers would build 9.5 MB of SQLite into a program that only wanted to
# render a query string. Asserting on the compiled object list rather than on
# "did it build" is the whole point — the contagious version builds fine.
rm -rf tests/tmp/dbcontagion
mkdir -p tests/tmp/dbcontagion
cp tests/fixtures/db_nodriver.vt tests/tmp/dbcontagion/
if ./vytoc build tests/tmp/dbcontagion/db_nodriver.vt \
        -o tests/tmp/dbcontagion/nodriver >/dev/null 2>&1; then
    # Also reject a socket object and an OpenSSL one. vyto/db/pgsql is pure
    # Vyto but reaches the network through vyto/net/socket and TLS through
    # vyto/crypto/openssl, both of which carry native shims — so the driver is
    # still contagious, just with a different payload than SQLite. The ossl
    # entry is what stops vyto/db itself from ever importing vyto/db/wire/tls,
    # which would put libssl on the link line of a program that renders a query
    # string and nothing else.
    if find tests/tmp/dbcontagion/.vyto-cache \
            \( -iname '*sqlite*' -o -iname '*socket*' -o -iname '*ossl*' \) 2>/dev/null | grep -q .; then
        echo "FAIL db_no_driver_contagion (importing vyto/db compiled a driver's native code)"
        find tests/tmp/dbcontagion/.vyto-cache \
             \( -iname '*sqlite*' -o -iname '*socket*' -o -iname '*ossl*' \) | head
        fail=1
    else
        echo "PASS db_no_driver_contagion"
    fi
else
    echo "FAIL db_no_driver_contagion (fixture does not build)"
    fail=1
fi

# --- and the mirror: naming ONE driver must not compile the other ---
# The drivers are separate package directories so that a program using Postgres
# does not build the SQLite amalgamation, and vice versa. That only holds while
# neither driver imports the other and vyto/db imports none.
#
# VYTO_OBJ_CACHE is set explicitly, as in validator_no_regex below: native
# objects are content-addressed and shared machine-wide (src/main.c:167-175), so
# a check that reads only the local .vyto-cache can be satisfied by a hit in the
# shared one and pass without proving anything.
rm -rf tests/tmp/pgonly
mkdir -p tests/tmp/pgonly
cat > tests/tmp/pgonly/pgonly.vt <<'VTEOF'
import { Db, select } from "vyto/db";
import { pgsql } from "vyto/db/pgsql";
fn main() {
    let db = new Db(pgsql("postgres://nobody@127.0.0.1:1/none"));
    print(db.isOpen());
}
VTEOF
#
# Two places have to be checked, because the compiler splits its output:
# per-module C and objects land in the local .vyto-cache under readable names,
# while native shim objects land in the object cache under content-addressed
# hashes. A name search finds the first and a content search finds the second;
# either alone passes while the other leaks.
if VYTO_OBJ_CACHE=tests/tmp/pgonly/obj ./vytoc build tests/tmp/pgonly/pgonly.vt \
        -o tests/tmp/pgonly/pgonly >/dev/null 2>&1; then
    leaked=""
    if find tests/tmp/pgonly/.vyto-cache -iname '*sqlite*' 2>/dev/null | grep -q .; then
        leaked="module objects"
    fi
    if grep -rl sqlite3_ tests/tmp/pgonly/obj >/dev/null 2>&1; then
        leaked="$leaked native objects"
    fi
    if [ -n "$leaked" ]; then
        echo "FAIL pgsql_no_sqlite (the Postgres driver compiled SQLite: $leaked)"
        find tests/tmp/pgonly/.vyto-cache -iname '*sqlite*' 2>/dev/null | head
        fail=1
    else
        echo "PASS pgsql_no_sqlite"
    fi
else
    echo "FAIL pgsql_no_sqlite (fixture does not build)"
    fail=1
fi

# --- and the same for the MySQL driver ---
rm -rf tests/tmp/myonly
mkdir -p tests/tmp/myonly
cat > tests/tmp/myonly/myonly.vt <<'VTEOF'
import { Db, select } from "vyto/db";
import { mysql } from "vyto/db/mysql";
fn main() {
    let db = new Db(mysql("mysql://nobody@127.0.0.1:1/none"));
    print(db.isOpen());
}
VTEOF
if VYTO_OBJ_CACHE=tests/tmp/myonly/obj ./vytoc build tests/tmp/myonly/myonly.vt \
        -o tests/tmp/myonly/myonly >/dev/null 2>&1; then
    leaked=""
    if find tests/tmp/myonly/.vyto-cache \( -iname '*sqlite*' -o -iname '*pgsql*' \) \
            2>/dev/null | grep -q .; then
        leaked="module objects"
    fi
    if grep -rl sqlite3_ tests/tmp/myonly/obj >/dev/null 2>&1; then
        leaked="$leaked native objects"
    fi
    if [ -n "$leaked" ]; then
        echo "FAIL mysql_no_other_driver (the MySQL driver compiled another one: $leaked)"
        fail=1
    else
        echo "PASS mysql_no_other_driver"
    fi
else
    echo "FAIL mysql_no_other_driver (fixture does not build)"
    fail=1
fi

# --- the vendored SQLite tree is byte-identical to what upstream shipped ---
# A local edit inside native/src/sqlite3/ would be silently reverted by the next
# refresh, so it is an error rather than a warning. Local changes belong in
# db_shim.c or sqlite_config.h.
if sh lib/vyto/db/sqlite/native/refresh-sqlite.sh --verify >/dev/null 2>&1; then
    echo "PASS db_vendored_sqlite_unmodified"
else
    echo "FAIL db_vendored_sqlite_unmodified (native/src/sqlite3/ has been edited)"
    fail=1
fi

# --- vyto/compress: the vendored zlib tree is byte-identical to upstream ---
# Same reasoning as the SQLite check above. Local changes belong in zlib_shim.c
# or zlib_config.h, both of which survive a refresh.
if sh lib/vyto/compress/native/refresh-zlib.sh --verify >/dev/null 2>&1; then
    echo "PASS compress_vendored_zlib_unmodified"
else
    echo "FAIL compress_vendored_zlib_unmodified (native/src/zlib/ has been edited)"
    fail=1
fi
# vyto/net's windows-x64 curl + mbedTLS + CA bundle: same rule. Build options
# belong in refresh-curl.sh (curl_config.h is generated from them) or
# vyto_mbedtls_user.h; glue in native/windows-x64/src/vnet_*.c.
if sh lib/vyto/net/native/refresh-curl.sh --verify >/dev/null 2>&1; then
    echo "PASS net_vendored_curl_unmodified"
else
    echo "FAIL net_vendored_curl_unmodified (native/windows-x64/ differs from refresh-curl.sh's output)"
    fail=1
fi

# --- vyto/compress: does our gzip interoperate with the system gzip? ---
#
# tests/unit/compress_roundtrip.vt proves decompress(compress(x)) == x across
# 69 assertions, and that is NOT this check. Those compare our encoder against
# our decoder, which share an implementation — a codec that got the format
# consistently wrong in both directions would pass every one of them.
#
# So this runs both halves against the system gzip, a witness that shares no
# code with anything in the tree. Writing something gunzip can read proves the
# encoder; reading what gzip wrote proves the decoder. A codec can fail either
# one alone, so both directions are checked.
#
# Deliberately NOT gated on anything. zlib is vendored (see the check above),
# so unlike the gfx block below this can never legitimately skip.
if command -v gzip >/dev/null 2>&1 && command -v gunzip >/dev/null 2>&1; then
    cref=tests/tmp/compress_ref.bin
    ./vytoc run tests/fixtures/compress_conformance.vt -- sample "$cref" >/dev/null 2>&1
    if [ ! -s "$cref" ]; then
        echo "FAIL compress_conformance (fixture produced no sample)"
        fail=1
    else
        # ours -> theirs
        ./vytoc run tests/fixtures/compress_conformance.vt -- write tests/tmp/compress_ours.gz >/dev/null 2>&1
        if gunzip -c tests/tmp/compress_ours.gz > tests/tmp/compress_a.bin 2>/dev/null &&
           cmp -s "$cref" tests/tmp/compress_a.bin; then
            echo "PASS compress_conformance_ours_to_gunzip"
        else
            echo "FAIL compress_conformance_ours_to_gunzip (system gunzip cannot read our gzip)"
            fail=1
        fi
        # theirs -> ours, at three levels, since the encoder settings differ
        cfail=0
        for lv in 1 6 9; do
            gzip -$lv -c < "$cref" > tests/tmp/compress_theirs.gz 2>/dev/null
            ./vytoc run tests/fixtures/compress_conformance.vt -- read \
                tests/tmp/compress_theirs.gz tests/tmp/compress_b.bin >/dev/null 2>&1
            cmp -s "$cref" tests/tmp/compress_b.bin || cfail=1
        done
        if [ "$cfail" -eq 0 ]; then
            echo "PASS compress_conformance_gzip_to_ours"
        else
            echo "FAIL compress_conformance_gzip_to_ours (cannot read what system gzip wrote)"
            fail=1
        fi
    fi
    rm -f tests/tmp/compress_ref.bin tests/tmp/compress_ours.gz tests/tmp/compress_theirs.gz \
          tests/tmp/compress_a.bin tests/tmp/compress_b.bin
else
    echo "SKIP compress_conformance (no system gzip)"
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

# --- package search path: vyto_modules/, --modpath, VYTO_PATH ---
#
# Sequential, like the libpath cases above: these share a .vyto-cache.
#
# The load-bearing case is the first one. alpha/ and beta/ each ship a
# config.vt; before package-root-scoped module names both were named 'config'
# and the build died with "two modules share the name", with no workaround
# available to either package.
pkg_want="alpha config (v1)
beta config"
got=$(./vytoc run tests/fixtures/modpath/main.vt 2>&1)
if [ "$got" = "$pkg_want" ]; then
    echo "PASS modpath_vyto_modules"
else
    echo "FAIL modpath_vyto_modules"
    printf '%s\n' "$got"
    fail=1
fi

# the vyto_modules/ walk climbs out of a nested entry directory
got=$(./vytoc run tests/fixtures/modpath/nested/main.vt 2>&1)
if [ "$got" = "$pkg_want" ]; then
    echo "PASS modpath_walk_up"
else
    echo "FAIL modpath_walk_up"
    printf '%s\n' "$got"
    fail=1
fi

# a file beside the entry still shadows a package root
got=$(./vytoc run tests/fixtures/modpath/shadow/main.vt 2>&1)
if [ "$got" = "shadowing alpha config" ]; then
    echo "PASS modpath_shadow"
else
    echo "FAIL modpath_shadow"
    printf '%s\n' "$got"
    fail=1
fi

# --modpath and VYTO_PATH reach a tree that is under no vyto_modules/, and a
# missing root in the list is skipped rather than fatal
got=$(./vytoc run tests/fixtures/vendored/app/main.vt \
        --modpath tests/fixtures/vendored/pkgs 2>&1)
if [ "$got" = "gamma config" ]; then
    echo "PASS modpath_flag"
else
    echo "FAIL modpath_flag"
    printf '%s\n' "$got"
    fail=1
fi
got=$(VYTO_PATH=tests/fixtures/vendored/pkgs \
        ./vytoc run tests/fixtures/vendored/app/main.vt 2>&1)
if [ "$got" = "gamma config" ]; then
    echo "PASS modpath_env"
else
    echo "FAIL modpath_env"
    printf '%s\n' "$got"
    fail=1
fi
got=$(VYTO_PATH=/nonexistent/one:tests/fixtures/vendored/pkgs \
        ./vytoc run tests/fixtures/vendored/app/main.vt 2>&1)
if [ "$got" = "gamma config" ]; then
    echo "PASS modpath_missing_root_skipped"
else
    echo "FAIL modpath_missing_root_skipped"
    printf '%s\n' "$got"
    fail=1
fi

# the naming itself, which is the only direct evidence of change 2: the two
# config.vt files emit distinct C, and a --modpath package is named exactly as
# a vendored one would be
if [ -f tests/fixtures/modpath/.vyto-cache/main/mod_alpha_config.c ] &&
   [ -f tests/fixtures/modpath/.vyto-cache/main/mod_beta_config.c ] &&
   [ -f tests/fixtures/vendored/app/.vyto-cache/main/mod_gamma_config.c ]; then
    echo "PASS modpath_scoped_names"
else
    echo "FAIL modpath_scoped_names (expected mod_alpha_config.c, mod_beta_config.c, mod_gamma_config.c)"
    fail=1
fi

# --- run all examples against golden output ---
#
# Parallel: see par_run at the top of this file for why this is safe.
d=$(par_begin examples)
# Look for the golden BEFORE running. The 18 hardware examples (51-69) have
# none, because asserting on them needs a device: a USB port to plug into, a
# sensor, a camera. Several block waiting for one that is not there --
# 69_uevent watches the kernel for a full 10s, a third of the entire examples
# run. Checking first skips them without paying for them. A SKIP is a verdict
# too, so it is written as one into the same results dir.
: > "$d/.queue"
for src in examples/[0-9]*.vt; do
    name=$(basename "$src" .vt)
    if [ ! -f "examples/$name.expected" ]; then
        echo "SKIP $name (no .expected)" > "$d/$name"
    else
        printf '%s\n' "$src" >> "$d/.queue"
    fi
done
par_run examples <<'WORKER'
#!/bin/sh
src="$1"; name=$(basename "$src" .vt); out="tests/tmp/par_examples/$name"
got=$(./vytoc run "$src" 2>&1)
if [ "$got" = "$(cat "examples/$name.expected")" ]; then
    echo "PASS $name" > "$out"
else
    { echo "FAIL $name"; echo "--- expected ---"; cat "examples/$name.expected"
      echo "--- got ---"; printf '%s\n' "$got"; } > "$out"
fi
WORKER
par_finish examples

# --- compile-error golden tests: each .vt must fail to build with its message ---
# Parallel: compile-only, no link, no run, no shared state beyond the
# per-entry cache. See par_run at the top of this file.
d=$(par_begin errors)
: > "$d/.queue"
for src in tests/errors/*.vt; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .vt)
    [ -f "tests/errors/$name.expected-error" ] || continue
    printf '%s\n' "$src" >> "$d/.queue"
done
par_run errors <<'WORKER'
#!/bin/sh
src="$1"; name=$(basename "$src" .vt)
expected="tests/errors/$name.expected-error"; out="tests/tmp/par_errors/$name"
# first diagnostic line, with the file:line prefix stripped for portability
got=$(./vytoc build "$src" 2>&1 | sed -n '1p' | sed 's/^[^ ]*: error:/error:/')
if [ "$got" = "$(cat "$expected")" ]; then
    echo "PASS err_$name" > "$out"
else
    { echo "FAIL err_$name"; echo "  expected: $(cat "$expected")"
      echo "  got:      $got"; } > "$out"
fi
WORKER
par_finish errors

# --- weak loads are optional: every narrowing shape compiles and runs ---
# The negative half is tests/errors/weak_deref*.vt. This is the half that has to
# keep working: a rule nobody can satisfy is worse than no rule.
got=$(./vytoc run tests/fixtures/weak_narrow.vt 2>&1)
if [ "$got" = "$(printf 'unset -1 -1\nset 7 14 7 7 7 7 7 7')" ]; then
    echo "PASS weak_narrowing"
else
    echo "FAIL weak_narrowing"
    echo "  got: $got"
    fail=1
fi

# --- integer overflow: checked (traps) in debug; --release is plain C, where
#     signed overflow is UB, so the wrap is only asserted under -fwrapv (the
#     documented opt-in). It used to be asserted under bare --release, which
#     passed only because gcc happens to lower this one expression as a wrap ---
got=$(./vytoc run tests/fixtures/overflow_trap.vt 2>&1)
if echo "$got" | grep -q "integer overflow in '+'"; then
    echo "PASS overflow_trap_debug"
else
    echo "FAIL overflow_trap_debug (expected a panic, got: $got)"
    fail=1
fi
# Built from a private copy: -fwrapv only reaches a module object that is
# compiled fresh (the cache does not key module objects on --cc), and --clean
# on tests/fixtures/ would race the other fixtures this suite builds there.
wrapv_dir=tests/tmp/wrapv
rm -rf "$wrapv_dir"
mkdir -p "$wrapv_dir"
cp tests/fixtures/overflow_trap.vt "$wrapv_dir/"
got=$(./vytoc run "$wrapv_dir/overflow_trap.vt" --release --cc "cc -fwrapv" 2>&1)
if echo "$got" | grep -q -- "-9223372036854775808"; then
    echo "PASS overflow_wrap_release"
else
    echo "FAIL overflow_wrap_release (expected -fwrapv wrap to i64 min, got: $got)"
    fail=1
fi

# --- T? syntax: every type position, and composition with generics ---
# Guards looks_like_type_args(): a missing T_QUESTION there breaks
# `ident<W?>(null)` while `new Box<W?>(null)` still works.
got=$(./vytoc run tests/fixtures/nullable_syntax.vt 2>&1)
want=$(printf 'fields 0\nf4 null\nlocal 3\nfree null\nvirtual 1\nmethod null\ngenerics 1\ncmp true')
if [ "$got" = "$want" ]; then
    echo "PASS nullable_syntax"
else
    echo "FAIL nullable_syntax"
    echo "  got: $got"
    fail=1
fi

# --- null receivers panic instead of segfaulting, in debug AND release ---
# Each of these was a bare segfault. The array cases are the subtle one: the
# bounds check existed but `((T*)a->data)[vt_arr_ati(...)]` loaded `->data`
# before the subscript ran, so a null array faulted before the check could.
for mode in "" "--release"; do
    tag=$([ -z "$mode" ] && echo debug || echo release)
    for case in str:to_upper arr_read:index arr_write:index arr_compound:index \
                closure:"a function call"; do
        c=${case%%:*}; want=${case#*:}
        got=$(./vytoc run tests/fixtures/null_recv_traps.vt $mode -- "$c" 2>&1)
        if echo "$got" | grep -q "panic: .*$want"; then
            echo "PASS null_recv_${c}_${tag}"
        else
            echo "FAIL null_recv_${c}_${tag} (expected a panic naming '$want', got: $got)"
            fail=1
        fi
    done
done

# --- null deref: named panic in debug, check compiled out in --release ---
# Covers the deliberate hole in the weak rule (docs/memory.md §3): a
# possibly-null reference may be passed along, so the checker cannot catch this.
got=$(./vytoc run tests/fixtures/null_deref_trap.vt 2>&1)
if echo "$got" | grep -q "null reference dereferenced"; then
    echo "PASS null_deref_trap_debug"
else
    echo "FAIL null_deref_trap_debug (expected a panic, got: $got)"
    fail=1
fi
# In --release the guard is gone: assert the emitted C carries no vt_ck_ptr,
# rather than running it — an unguarded null deref is a segfault, not an output.
rm -rf tests/fixtures/.vyto-cache
./vytoc build tests/fixtures/null_deref_trap.vt --release >/dev/null 2>&1
nd=tests/fixtures/.vyto-cache/null_deref_trap/mod_null_deref_trap.c
if [ -f "$nd" ] && ! grep -q "vt_ck_ptr" "$nd"; then
    echo "PASS null_deref_release_unchecked"
else
    echo "FAIL null_deref_release_unchecked (vt_ck_ptr survived --release)"
    fail=1
fi
rm -rf tests/fixtures/.vyto-cache

# --- command-line args: args() builtin sees what follows `--` ---
got=$(./vytoc run tests/fixtures/args_echo.vt -- alpha beta 2>&1)
if [ "$got" = "$(printf 'n=2\nalpha\nbeta')" ]; then
    echo "PASS args_builtin"
else
    echo "FAIL args_builtin (got: $got)"
    fail=1
fi

# --- non-ASCII paths, argv and environment ---
# Trivially true on Linux (the kernel takes bytes); kept here so the fixture
# and its golden cannot rot. Its real run is in run_tests_win.sh, where every
# one of these went through an ANSI-code-page API before.
rm -rf tests/tmp/ünï_*
got=$(VYTO_UNI_IN="Zöë日" ./vytoc run tests/fixtures/unicode_paths.vt -- "Zöë日" 2>&1)
if [ "$got" = "$(cat tests/fixtures/unicode_paths.expected)" ]; then
    echo "PASS unicode_paths"
else
    echo "FAIL unicode_paths"
    echo "$got" | grep -v ': true$'
    fail=1
fi
rm -rf tests/tmp/ünï_*

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

# --- vyto/os/reactor: the loop shape vyto/net/server runs on ---
#
# The gate that says the extracted reactor could carry that server before one
# line of it is refactored: accept-drain, fd-indexed dispatch, retire-on-false,
# POLL_WRITE arm/disarm, and the idle sweep, all against the public API.
#
# spurious_write=0 is the assertion that matters. A socket is almost always
# writable, so a POLL_WRITE left armed fires on every wakeup and spins the loop
# at 100% CPU (server.vt:2002-2011). Fault-injected to confirm the check is not
# vacuous: deleting the disarm makes it report 53, not 0.
got=$(./vytoc run tests/fixtures/reactor_server_shape.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/reactor_server_shape.expected)" ]; then
    echo "PASS reactor_server_shape"
else
    echo "FAIL reactor_server_shape"
    echo "--- expected ---"; cat tests/fixtures/reactor_server_shape.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- a streamed response body, under the REACTOR ---
#
# 91_server covers the framing on the BLOCKING path. The two paths write a
# stream through entirely different code, and a fault injected into the
# reactor's terminator left all of 91_server green -- which is what this exists
# to catch.
#
# WRAPPED IN timeout, deliberately. Removing the rule that judges a streaming
# connection on its own deadline gets it swept mid-body, and the client then
# waits forever for bytes that stop arriving: the failure is a HANG, not a red
# assertion. A CI job without the wrapper would stall instead of reporting.
got=$(timeout 60 ./vytoc run tests/fixtures/server_stream_reactor.vt 2>&1)
rc=$?
if [ "$rc" -eq 124 ]; then
    echo "FAIL server_stream_reactor (TIMED OUT -- a stream was swept mid-body?)"
    fail=1
elif [ "$got" = "$(cat tests/fixtures/server_stream_reactor.expected)" ]; then
    echo "PASS server_stream_reactor"
else
    echo "FAIL server_stream_reactor"
    echo "--- expected ---"; cat tests/fixtures/server_stream_reactor.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- vyto/os/reactor: the self-pipe wakeup and signal routing ---
got=$(./vytoc run tests/fixtures/reactor_wakeup.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/reactor_wakeup.expected)" ]; then
    echo "PASS reactor_wakeup"
else
    echo "FAIL reactor_wakeup"
    echo "--- expected ---"; cat tests/fixtures/reactor_wakeup.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- vyto/os/reactor: EINTR — a signal while blocked in epoll_wait ---
#
# Needs a REAL external signal: raise(3) from a timer runs after wait() has
# already returned and never produces the EINTR this covers. So the binary is
# built, backgrounded, and signalled from here.
#
# Before the fix in socket_shim.c, the first signal a process received killed
# its event loop silently (exit 0). Reverting that arm makes this print
# got=0 / survived_eintr=false.
if ./vytoc build tests/fixtures/reactor_eintr.vt \
        -o tests/tmp/reactor_eintr >/dev/null 2>&1; then
    tests/tmp/reactor_eintr > tests/tmp/reactor_eintr.out 2>&1 &
    eintr_pid=$!
    # Let it reach run() and block. The guard timer is 10s, so this is ample.
    sleep 2
    kill -USR1 "$eintr_pid" 2>/dev/null
    sleep 1
    kill -USR1 "$eintr_pid" 2>/dev/null
    wait "$eintr_pid" 2>/dev/null
    got=$(cat tests/tmp/reactor_eintr.out 2>/dev/null)
    if [ "$got" = "$(cat tests/fixtures/reactor_eintr.expected)" ]; then
        echo "PASS reactor_eintr"
    else
        echo "FAIL reactor_eintr"
        echo "--- expected ---"; cat tests/fixtures/reactor_eintr.expected
        echo "--- got ---"; printf '%s\n' "$got"
        fail=1
    fi
else
    echo "FAIL reactor_eintr (build)"
    fail=1
fi

# --- vyto/os/reactor: a window and sockets from one loop (phase 4) ---
#
# Both arrangements: the fd path (X11 exposes the display socket, so the window
# is just another watched fd) and the waiter path (Win32/Android have no
# descriptor, so the WINDOW owns the blocking call and the reactor sweeps its
# fds afterwards at zero timeout).
#
# Run under VS_HEADLESS, like every other surface-touching test here: CI has no
# X server at all, and Surface.init panics rather than degrading (surface.vt:228).
# The assertions are written to hold on either backend, so running it headless
# costs no coverage of the branch that matters — the waiter path is the one
# headless takes, and it is also the one Win32 and Android take, neither of
# which this suite can reach.
got=$(VS_HEADLESS=1 VS_EVENTS=/dev/null ./vytoc run tests/fixtures/reactor_surface.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/reactor_surface.expected)" ]; then
    echo "PASS reactor_surface"
else
    echo "FAIL reactor_surface"
    echo "--- expected ---"; cat tests/fixtures/reactor_surface.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- vyto/ui: Window.add_poll_fd waits on outside work, does not poll for it ---
#
# add_poll_source dropped the loop to a 16ms wait for as long as any source was
# registered — ~60 wakeups a second whether or not anything was happening.
# add_poll_fd carries the descriptor, so where the backend can wait on it (X11)
# the loop blocks on the window and the fd together. Measured on this machine:
# 61 turns/sec before, 4 after, with 0ms latency when the work does land.
#
# Run under VS_HEADLESS: CI has no X server, and Surface.init panics rather
# than degrading. Headless reports canWaitFds() false and takes the old 16ms
# fallback, so what this asserts there is that add_poll_fd is CORRECT on a
# backend that cannot wait on descriptors — the Win32/Android case. The
# waiting itself is verified on X11 by hand; see MODULES.md for the numbers.
got=$(VS_HEADLESS=1 VS_EVENTS=/dev/null ./vytoc run tests/fixtures/ui_poll_fd.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/ui_poll_fd.expected)" ]; then
    echo "PASS ui_poll_fd"
else
    echo "FAIL ui_poll_fd"
    echo "--- expected ---"; cat tests/fixtures/ui_poll_fd.expected
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

# --- vyto/geom/quat: orientation, slerp, Mat4 cross-checks (pure Vyto) ---
got=$(./vytoc run tests/fixtures/geom_quat.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/geom_quat.expected)" ]; then
    echo "PASS geom_quat"
else
    echo "FAIL geom_quat"
    echo "--- expected ---"; cat tests/fixtures/geom_quat.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- float semantics: literals, comparison, rounding, transcendentals, the
#     inf/nan edges and str() round-tripping. 90 assertions, pure Vyto, and it
#     lives in apps/ only because that is where it was written — it is a
#     language test, not an app, so it runs here rather than with the app
#     builds. Sub-second, unlike the large apps make test deliberately skips. ---
got=$(./vytoc run apps/float_paranoia.vt 2>&1)
if [ "$got" = "$(cat apps/float_paranoia.expected)" ]; then
    echo "PASS float_paranoia"
else
    echo "FAIL float_paranoia"
    printf '%s\n' "$got" | grep '^FAIL' | head -20
    fail=1
fi

# --- vyto/math/stats: descriptive statistics over float[]/int[] (pure Vyto) ---
got=$(./vytoc run tests/fixtures/math_stats.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/math_stats.expected)" ]; then
    echo "PASS math_stats"
else
    echo "FAIL math_stats"
    echo "--- expected ---"; cat tests/fixtures/math_stats.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- vyto/math/algebra: general N×M matrices, arbitrary-length vectors
#     (pure Vyto) ---
got=$(./vytoc run tests/fixtures/math_algebra.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/math_algebra.expected)" ]; then
    echo "PASS math_algebra"
else
    echo "FAIL math_algebra"
    echo "--- expected ---"; cat tests/fixtures/math_algebra.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- vyto/math/algebra: inverting a singular matrix panics. Grep-style
#     rather than a golden, because a panic aborts the process and truncates
#     stdout — same pattern as the mmap_guards checks below. ---
if ./vytoc run tests/fixtures/math_algebra.vt -- singular 2>&1 | grep -q "Matrix.inverted: singular matrix"; then
    echo "PASS math_algebra_singular"
else
    echo "FAIL math_algebra_singular (expected a singular-matrix panic)"
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

# --- vyto/mobile/android/manifest: the generated AndroidManifest.xml.
#     Pure Vyto with no shim behind it, so it runs in the toolchain-free suite —
#     which matters, because the only other thing that exercises this module is
#     build-apk.sh, and that needs an SDK. A wrong manifest is caught on a
#     device or not at all. ---
got=$(./vytoc run tests/fixtures/android_manifest.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/android_manifest.expected)" ]; then
    echo "PASS android_manifest"
else
    echo "FAIL android_manifest"
    echo "--- expected ---"; cat tests/fixtures/android_manifest.expected
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

# --- for-in holds its array at +1: reassigning the iterated variable inside
#     the body must not free the array under the loop (was a use-after-free) ---
got=$(./vytoc run tests/fixtures/forin_hold.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/forin_hold.expected)" ]; then
    echo "PASS forin_hold"
else
    echo "FAIL forin_hold"
    echo "--- expected ---"; cat tests/fixtures/forin_hold.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- builtin Map: contents and INSERTION ORDER against a parallel-array model,
#     over a seeded random mix of set/remove/has/get_or/clear, plus merge and
#     re-insert rules. Order is a documented guarantee since the compact-dict
#     rewrite (vyto_rt.h, above VtMapEntry); fault injection confirmed this
#     fails both on a broken backward-shift delete and on reversed keys() ---
got=$(./vytoc run tests/fixtures/map_order.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/map_order.expected)" ]; then
    echo "PASS map_order"
else
    echo "FAIL map_order"
    echo "--- expected ---"; cat tests/fixtures/map_order.expected
    echo "--- got ---"; printf '%s\n' "$got"
    fail=1
fi

# --- vyto/cli/ask under a pipe. The interactive path cannot be golden-tested
#     (prompting exists to react to a human), but the piped one can, and it is
#     the path scripts, cron and CI take: every helper must return its fallback
#     instead of blocking, and fill() must leave the original error alone. A
#     regression here hangs a pipeline forever with no output saying why ---
got=$(./vytoc run tests/fixtures/cli_ask.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/cli_ask.expected)" ]; then
    echo "PASS cli_ask"
else
    echo "FAIL cli_ask"
    echo "--- expected ---"; cat tests/fixtures/cli_ask.expected
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
# SEQUENTIAL, deliberately. Every case writes and then mmaps the SAME file,
# tests/tmp/mmap_g.bin (mmap_guards.vt:14), so two of them running at once
# truncate the mapping out from under each other — mmap_guard_store failed that
# way exactly once in a parallel run. The per-entry cache makes concurrent
# BUILDS safe; it says nothing about a fixture that shares a data file.
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
    # GfxPainter finds medium/bold siblings for both naming schemes. Inter's
    # "-Regular" scheme used to load one face and draw every weight as regular.
    got=$(VS_HEADLESS=1 ./vytoc run tests/fixtures/gfx_font_weights.vt 2>&1)
    if [ "$got" = "$(cat tests/fixtures/gfx_font_weights.expected)" ]; then
        echo "PASS gfx_font_weights"
    else
        echo "FAIL gfx_font_weights"
        echo "--- expected ---"; cat tests/fixtures/gfx_font_weights.expected
        echo "--- got ---"; echo "$got"
        fail=1
    fi
    # A shadow's colour alpha is its opacity (it was ignored until 2026-11).
    got=$(VS_HEADLESS=1 ./vytoc run tests/fixtures/gfx_shadow_alpha.vt 2>&1)
    if [ "$got" = "$(cat tests/fixtures/gfx_shadow_alpha.expected)" ]; then
        echo "PASS gfx_shadow_alpha"
    else
        echo "FAIL gfx_shadow_alpha"
        echo "--- expected ---"; cat tests/fixtures/gfx_shadow_alpha.expected
        echo "--- got ---"; echo "$got"
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
    # vyto/media/image/decode: encode with stb, decode with blend2d, compare
    # every pixel. Gated here because decoding is the half that needs blend2d;
    # the encoder is covered by examples/111_image.vt, which runs everywhere.
    # This is also the only check on gfx_image_copy_pixels, which un-premultiplies
    # blend2d's PRGB32 — get that wrong and every translucent pixel darkens.
    got=$(./vytoc run tests/fixtures/image_decode.vt 2>&1)
    if [ "$got" = "$(cat tests/fixtures/image_decode.expected)" ]; then
        echo "PASS image_decode"
    else
        echo "FAIL image_decode"
        echo "--- expected ---"; cat tests/fixtures/image_decode.expected
        echo "--- got ---"; printf '%s\n' "$got"
        fail=1
    fi
    # Canvas.resize must REUSE the parsed typefaces. Rebuilding the canvas
    # instead re-parses every TTF on every step of a drag-resize, which no
    # geometry golden can see — they assert what was drawn, never what it cost.
    got=$(./vytoc run tests/fixtures/gfx_resize_fonts.vt 2>&1)
    if [ "$got" = "ok" ]; then
        echo "PASS gfx_resize_fonts"
    else
        echo "FAIL gfx_resize_fonts (got: $got)"
        fail=1
    fi
else
    echo "SKIP gfx_canvas_blit (no libblend2d — run lib/vyto/gfx/native/build-blend2d.sh)"
    echo "SKIP image_decode (no libblend2d — the encoder half runs in 111_image)"
    echo "SKIP gfx_resize_fonts (no libblend2d)"
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

# --- WinManifest: validation and render, no mingw needed (pure Vyto) ---
got=$(./vytoc run tests/fixtures/win_manifest_validate.vt 2>&1)
if [ "$got" = "$(cat tests/fixtures/win_manifest_validate.expected)" ]; then
    echo "PASS win_manifest_validate"
else
    echo "FAIL win_manifest_validate"
    echo "--- expected ---"; cat tests/fixtures/win_manifest_validate.expected
    echo "--- got ---"; echo "$got"
    fail=1
fi

# --- Window.set_icon reaches the surface and is a safe no-op headless ---
got=$(VS_HEADLESS=1 ./vytoc run tests/fixtures/set_icon_headless.vt 2>&1)
if [ "$got" = "set_icon ok" ]; then
    echo "PASS set_icon_headless"
else
    echo "FAIL set_icon_headless (got: $got)"
    fail=1
fi

# --- Windows exe groundwork: PE headers, resources, the --manifest program --
# Cross-built here and read back with mingw objdump, so every property of a
# Windows exe that does not need Windows to observe is checked on every run:
# subsystem, ASLR/DEP, the embedded manifest, icon and version info, strip,
# relink on a link-only change, DLL naming and export. Needs mingw; SKIPs
# without it.
if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1 &&
   command -v x86_64-w64-mingw32-objdump >/dev/null 2>&1 &&
   command -v x86_64-w64-mingw32-windres >/dev/null 2>&1; then
    wd=tests/tmp/winpe
    rm -rf "$wd"; mkdir -p "$wd"
    OD=x86_64-w64-mingw32-objdump
    W="--target windows-x64"
    GM="--manifest tests/fixtures/win_gui_manifest.vt"
    win_check() {
        if [ "$2" = ok ]; then echo "PASS $1"; else echo "FAIL $1 ($2)"; fail=1; fi
    }
    pe_subsys() { $OD -p "$1" 2>/dev/null | awk '/^Subsystem/{print $2}'; }
    # top-level resource type IDs: 3 ICON, e GROUP_ICON, 10 VERSION, 18 MANIFEST
    pe_rtypes() {
        $OD -p "$1" 2>/dev/null | awk '/^0[0-9a-f][0-9a-f]   Entry: ID: /{print $4}' |
            tr -d ',' | tr '\n' ' '
    }
    gui=$wd/gui.exe; con=$wd/con.exe
    # shellcheck disable=SC2086
    ./vytoc build tests/fixtures/win_gui_app.vt $W $GM -o "$gui" >/dev/null 2>&1
    # shellcheck disable=SC2086
    ./vytoc build tests/fixtures/win_console_app.vt $W -o "$con" >/dev/null 2>&1

    r=$(pe_subsys "$gui"); [ "$r" = 00000002 ] && r=ok
    win_check win_pe_gui_subsystem "$r"
    r=$(pe_subsys "$con"); [ "$r" = 00000003 ] && r=ok
    win_check win_pe_console_subsystem "$r"
    dc=$($OD -p "$con" 2>/dev/null | awk '/^DllCharacteristics/{print $2}')
    # 0x40 DYNAMIC_BASE (ASLR) | 0x100 NX_COMPAT (DEP)
    r="DllCharacteristics=$dc"; [ -n "$dc" ] && [ $((0x$dc & 0x140)) -eq 320 ] && r=ok
    win_check win_pe_aslr_dep "$r"
    r=$(pe_rtypes "$gui")
    case "$r" in *0x000003*0x00000e*0x000010*0x000018*) r=ok ;; esac
    win_check win_pe_gui_resources "$r"
    r=$(pe_rtypes "$con"); [ "$r" = "0x000018 " ] && r=ok
    win_check win_pe_console_manifest_only "$r"
    r=missing
    grep -aq dpiAware "$gui" && grep -aq asInvoker "$gui" && grep -aq activeCodePage "$con" && r=ok
    win_check win_pe_manifest_content "$r"
    vi=$(strings -el "$gui")
    r=missing
    echo "$vi" | grep -qx "Vyto Test App" && echo "$vi" | grep -qx "1.2.3" &&
        echo "$vi" | grep -qxF '(c) 2026 Vyto "tests"' && r=ok
    win_check win_pe_version_info "$r"

    # --console flips the subsystem and back with no --clean: the link stamp
    # must relink on a link-only change, both ways.
    # shellcheck disable=SC2086
    ./vytoc build tests/fixtures/win_gui_app.vt $W $GM --console -o "$gui" >/dev/null 2>&1
    a=$(pe_subsys "$gui")
    # shellcheck disable=SC2086
    ./vytoc build tests/fixtures/win_gui_app.vt $W $GM -o "$gui" >/dev/null 2>&1
    b=$(pe_subsys "$gui")
    r="$a then $b"; [ "$a" = 00000003 ] && [ "$b" = 00000002 ] && r=ok
    win_check win_pe_relink_on_flag "$r"

    # --release strips: the debug build has debug sections, the release none.
    rel=$wd/rel.exe
    # shellcheck disable=SC2086
    ./vytoc build tests/fixtures/win_console_app.vt $W --release -o "$rel" >/dev/null 2>&1
    nd=$($OD -h "$con" 2>/dev/null | grep -c '\.debug')
    nr=$($OD -h "$rel" 2>/dev/null | grep -c '\.debug')
    r="debug sections: debug build $nd, release $nr"; [ "$nd" -gt 0 ] && [ "$nr" -eq 0 ] && r=ok
    win_check win_pe_release_strip "$r"

    # --shared names a .dll and exports exactly the loader entry.
    # shellcheck disable=SC2086
    dll=$(./vytoc build tests/fixtures/win_console_app.vt $W --shared 2>/dev/null | tail -1)
    ex=$($OD -p "$dll" 2>/dev/null | sed -n '/\[Ordinal\/Name Pointer\] Table/,/^$/p' |
         grep -c 'vyto_app_main')
    r="$dll exports vyto_app_main: $ex"
    case "$dll" in *.dll) [ "$ex" = 1 ] && r=ok ;; esac
    win_check win_pe_shared_dll "$r"

    # .dpi("unaware") drops dpiAware; no --manifest still embeds the manifest.
    un=$wd/unaware.exe; plain=$wd/plain.exe
    # shellcheck disable=SC2086
    ./vytoc build tests/fixtures/win_gui_app.vt $W \
        --manifest tests/fixtures/win_unaware_manifest.vt -o "$un" >/dev/null 2>&1
    # shellcheck disable=SC2086
    ./vytoc build tests/fixtures/win_gui_app.vt $W -o "$plain" >/dev/null 2>&1
    r=bad
    ! grep -aq dpiAware "$un" && grep -aq asInvoker "$un" && grep -aq dpiAware "$plain" &&
        [ "$(pe_rtypes "$plain")" = "0x000018 " ] && r=ok
    win_check win_pe_dpi_and_default_manifest "$r"

    # A manifest program that refuses fails the build, naming the problem.
    # shellcheck disable=SC2086
    out=$(./vytoc build tests/fixtures/win_gui_app.vt $W \
          --manifest tests/fixtures/win_bad_manifest.vt -o "$wd/bad.exe" 2>&1)
    st=$?
    r="status $st"
    [ "$st" -ne 0 ] && echo "$out" | grep -q 'version "1.x"' && r=ok
    win_check win_manifest_refusal_fails_build "$r"

    # --- the Windows 7 import audit (src/peaudit.c) ---
    # A clean exe passes; an exe importing a Windows 8 function and a DLL
    # built against the Universal CRT must each fail with their own reason.
    # The two failures are the audit's fault injection: an audit stubbed out to
    # PASS everything fails both, and one that only checks the exit status
    # would not tell the two rules apart.
    # shellcheck disable=SC2086
    out=$(./vytoc build tests/fixtures/win_console_app.vt $W --audit -o "$wd/aud.exe" 2>&1)
    st=$?
    r="status $st: $out"
    [ "$st" -eq 0 ] && echo "$out" | grep -q "^PASS pe_audit aud.exe" && r=ok
    win_check win_audit_pass "$r"
    # shellcheck disable=SC2086
    out=$(./vytoc build tests/fixtures/win_win8_import.vt $W --audit -o "$wd/w8.exe" 2>&1)
    st=$?
    r="status $st"
    [ "$st" -ne 0 ] &&
        echo "$out" | grep -q "kernel32.dll!GetSystemTimePreciseAsFileTime does not exist on Windows 7" &&
        r=ok
    win_check win_audit_win8_fails "$r"
    r="no ucrt dll"
    if x86_64-w64-mingw32-gcc -shared -nostdlib -o "$wd/ucrt.dll" tests/fixtures/win_ucrt.c \
            -lucrt -lkernel32 >/dev/null 2>&1; then
        out=$(./vytoc audit "$wd/ucrt.dll" 2>&1)
        st=$?
        r="status $st"
        [ "$st" -ne 0 ] && echo "$out" | grep -q "imports the Universal CRT (api-ms-win-crt-" && r=ok
    fi
    win_check win_audit_ucrt_fails "$r"

    # --- vyto/net on Windows: curl + mbedTLS from vendored source ---
    # No curl DLL beside the exe any more, the CA bundle staged instead, and
    # an import table that passes the Windows 7 audit; then the same with
    # --bundle, which the DLL never allowed.
    nd=$wd/net; mkdir -p "$nd/b"
    # shellcheck disable=SC2086
    ./vytoc build examples/47_http.vt $W -o "$nd/h.exe" >/dev/null 2>&1
    r=bad
    [ -f "$nd/h.exe" ] && [ -f "$nd/cacert.pem" ] && ! ls "$nd"/*.dll >/dev/null 2>&1 &&
        ./vytoc audit "$nd/h.exe" >/dev/null 2>&1 && r=ok
    win_check win_net_static "$r"
    # shellcheck disable=SC2086
    ./vytoc build examples/47_http.vt $W --bundle -o "$nd/b/h.exe" >/dev/null 2>&1
    r=bad
    [ -f "$nd/b/h.exe" ] && [ -f "$nd/b/cacert.pem" ] && ./vytoc audit "$nd/b/h.exe" >/dev/null 2>&1 &&
        r=ok
    win_check win_net_bundle "$r"

    # --- --installer: a per-machine setup asks for administrator rights and
    # carries the payload as RCDATA (type 0x0a), a per-user one does not ask;
    # both, and the uninstaller inside, pass the Windows 7 audit. A service
    # with --per-user is refused: a service needs a per-machine install.
    idir=$wd/inst; mkdir -p "$idir/m" "$idir/u"
    # shellcheck disable=SC2086
    ./vytoc build tests/fixtures/win_console_app.vt $W $GM --installer -o "$idir/m/app.exe" >/dev/null 2>&1
    ms=$(ls "$idir"/m/*-setup.exe 2>/dev/null | head -1)
    r="no setup.exe"
    if [ -n "$ms" ]; then
        r="resources: $(pe_rtypes "$ms")"
        grep -aq requireAdministrator "$ms" && pe_rtypes "$ms" | grep -q "0x00000a" &&
            ./vytoc audit "$ms" >/dev/null 2>&1 && r=ok
    fi
    win_check win_installer_machine "$r"
    # shellcheck disable=SC2086
    ./vytoc build tests/fixtures/win_console_app.vt $W $GM --installer --per-user -o "$idir/u/app.exe" >/dev/null 2>&1
    us=$(ls "$idir"/u/*-setup.exe 2>/dev/null | head -1)
    r="no per-user setup.exe"
    if [ -n "$us" ]; then
        r="manifest level wrong"
        ! grep -aq requireAdministrator "$us" && grep -aq asInvoker "$us" && r=ok
    fi
    win_check win_installer_per_user "$r"
    # shellcheck disable=SC2086
    out=$(./vytoc build tests/win/service_demo.vt $W --manifest tests/win/service_demo_manifest.vt \
          --installer --per-user -o "$idir/u/svc.exe" 2>&1)
    r="status ok: $out"
    echo "$out" | grep -q "needs a per-machine" && r=ok
    win_check win_installer_service_needs_machine "$r"

    # --- --sign: Authenticode via osslsigncode, with a throwaway self-signed
    # certificate made here. The signature must be real (a non-empty
    # certificate table that osslsigncode verifies against that certificate),
    # must not disturb the import audit, and the password must reach
    # osslsigncode without appearing in --verbose output.
    if command -v osslsigncode >/dev/null 2>&1 && command -v openssl >/dev/null 2>&1; then
        sd=$wd/sign; mkdir -p "$sd"
        pw="vyto-test-$$-secret"
        openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj "/CN=Vyto Test Signer" \
            -keyout "$sd/k.pem" -out "$sd/c.pem" >/dev/null 2>&1 &&
        openssl pkcs12 -export -inkey "$sd/k.pem" -in "$sd/c.pem" -out "$sd/c.pfx" \
            -passout "pass:$pw" >/dev/null 2>&1
        # shellcheck disable=SC2086
        out=$(VYTO_SIGN_PASS=$pw ./vytoc build tests/fixtures/win_console_app.vt $W --verbose \
              --sign "$sd/c.pfx" -o "$sd/s.exe" 2>&1)
        st=$?
        r="status $st"
        secdir=$($OD -p "$sd/s.exe" 2>/dev/null | awk '/Security Directory/{print $4}')
        [ "$st" -eq 0 ] && [ -n "$secdir" ] && [ "$secdir" != "00000000" ] &&
            osslsigncode verify -CAfile "$sd/c.pem" -in "$sd/s.exe" >/dev/null 2>&1 &&
            ./vytoc audit "$sd/s.exe" >/dev/null 2>&1 && r=ok
        win_check win_sign "$r"
        r=ok
        echo "$out" | grep -q "$pw" && r="password appeared in --verbose output"
        ls "$wd"/sign/*.pass* tests/fixtures/.vyto-cache/sign.pass* >/dev/null 2>&1 &&
            r="password file left behind"
        win_check win_sign_no_pass_leak "$r"
        # dropping --sign relinks to an unsigned exe rather than keeping the
        # signed one (the flag is part of the link stamp)
        # shellcheck disable=SC2086
        ./vytoc build tests/fixtures/win_console_app.vt $W -o "$sd/s.exe" >/dev/null 2>&1
        secdir=$($OD -p "$sd/s.exe" 2>/dev/null | awk '/Security Directory/{print $4}')
        r="still signed ($secdir)"
        { [ -z "$secdir" ] || [ "$secdir" = "00000000" ]; } && r=ok
        win_check win_sign_dropped_relinks "$r"
        # A failed signing (wrong password) must fail the build AND leave no
        # exe: the stamp already records --sign, so an unsigned exe left
        # behind would survive the next unchanged rebuild looking finished.
        # The retry with the right password must then produce a signed one.
        # shellcheck disable=SC2086
        VYTO_SIGN_PASS=wrong ./vytoc build tests/fixtures/win_console_app.vt $W \
            --sign "$sd/c.pfx" -o "$sd/f.exe" >/dev/null 2>&1
        st=$?
        r="status $st"
        if [ "$st" -ne 0 ] && [ ! -e "$sd/f.exe" ]; then
            # shellcheck disable=SC2086
            VYTO_SIGN_PASS=$pw ./vytoc build tests/fixtures/win_console_app.vt $W \
                --sign "$sd/c.pfx" -o "$sd/f.exe" >/dev/null 2>&1
            osslsigncode verify -CAfile "$sd/c.pem" -in "$sd/f.exe" >/dev/null 2>&1 && r=ok ||
                r="retry not signed"
        elif [ -e "$sd/f.exe" ]; then
            r="unsigned exe left behind"
        fi
        win_check win_sign_failure_leaves_nothing "$r"
    else
        echo "SKIP win_sign (no osslsigncode: apt install osslsigncode)"
    fi
else
    echo "SKIP win_pe_headers (no mingw gcc, objdump and windres)"
fi
# --manifest is Windows-only: on any other target it is an error, not ignored.
out=$(./vytoc build tests/fixtures/win_console_app.vt \
      --manifest tests/fixtures/win_gui_manifest.vt 2>&1)
if [ $? -ne 0 ] && echo "$out" | grep -q "describes a Windows exe"; then
    echo "PASS manifest_rejected_off_windows"
else
    echo "FAIL manifest_rejected_off_windows (got: $out)"
    fail=1
fi

# --- vyto/surface binding: regenerate with vytobind, golden-check ---
./vytobind lib/vyto/surface/native/src/vsurf.h \
    --lib X11@linux --lib dl@linux --lib X11@macos \
    --lib gdi32@windows --lib user32@windows --lib imm32@windows \
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
# Parallel: per-test .events/.expected, no shared writes. See par_run above.
ui_d=$(par_begin ui)
: > "$ui_d/.queue"
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
    printf '%s\n' "$name" >> "$ui_d/.queue"
done
par_run ui <<'WORKER'
#!/bin/sh
name="$1"; out="tests/tmp/par_ui/$name"
got=$(VS_HEADLESS=1 VS_EVENTS="tests/ui/$name.events" ./vytoc run "tests/ui/$name.vt" 2>&1)
if [ "$got" = "$(cat "tests/ui/$name.expected")" ]; then
    echo "PASS ui_$name" > "$out"
else
    { echo "FAIL ui_$name"; echo "--- expected ---"; cat "tests/ui/$name.expected"
      echo "--- got ---"; printf '%s\n' "$got"; } > "$out"
fi
WORKER
par_finish ui

# --- vyto/ui named golden tests: TextArea + menus (no env needed) ---
uin_d=$(par_begin uinamed)
for name in textarea menu gallery scale spring disabled motion layout_anim transition dark_theme ripple; do
    printf '%s\n' "$name" >> "$uin_d/.queue"
done
par_run uinamed <<'WORKER'
#!/bin/sh
name="$1"; out="tests/tmp/par_uinamed/$name"
got=$(VS_HEADLESS=1 VS_EVENTS="tests/ui/$name.events" ./vytoc run "tests/ui/$name.vt" 2>&1)
if [ "$got" = "$(cat "tests/ui/$name.expected")" ]; then
    echo "PASS ui_$name" > "$out"
else
    { echo "FAIL ui_$name"; echo "--- expected ---"; cat "tests/ui/$name.expected"
      echo "--- got ---"; printf '%s\n' "$got"; } > "$out"
fi
WORKER
par_finish uinamed

# --- render snapshots: rasterize real widget trees through the rich tier
#     (blend2d) and assert on a hash of the pixels. The goldens above cover
#     widget state; these cover what the widgets actually look like.
#
#     On failure the frames are dumped as PPMs under tests/tmp/snap so the
#     change can be inspected — a hash says a frame moved, not how.
mkdir -p tests/tmp
# Parallel: the PPM dump on failure goes to a per-test dir, so workers never
# collide even when several fail at once. See par_run above.
snap_d=$(par_begin uisnap)
: > "$snap_d/.queue"
for src in tests/ui/snap_*.vt; do
    [ -e "$src" ] || continue
    printf '%s\n' "$(basename "$src" .vt)" >> "$snap_d/.queue"
done
par_run uisnap <<'WORKER'
#!/bin/sh
name="$1"; src="tests/ui/$name.vt"; out="tests/tmp/par_uisnap/$name"
got=$(VS_HEADLESS=1 VS_EVENTS="tests/ui/$name.events" ./vytoc run "$src" 2>&1)
if [ "$got" = "$(cat "tests/ui/$name.expected")" ]; then
    echo "PASS ui_$name" > "$out"
else
    rm -rf "tests/tmp/snap/$name"; mkdir -p "tests/tmp/snap/$name"
    VS_HEADLESS=1 VS_EVENTS="tests/ui/$name.events" \
        VYTO_SNAP_DIR="tests/tmp/snap/$name" ./vytoc run "$src" >/dev/null 2>&1
    { echo "FAIL ui_$name"; echo "--- expected ---"; cat "tests/ui/$name.expected"
      echo "--- got ---"; printf '%s\n' "$got"
      echo "--- frames written to tests/tmp/snap/$name ---"; } > "$out"
fi
WORKER
par_finish uisnap

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
# dir_readable: the guard that makes listdir safe on a user-chosen path.
# listdir PANICS (exit 101) on a directory it cannot open, and isdir/file_exists
# both answer true for one — so a clean exit here is the property under test,
# not just the printed lines.
got=$(./vytoc run tests/fixtures/dir_readable.vt 2>&1)
code=$?
if [ $code -ne 0 ]; then
    echo "FAIL dir_readable (exit $code)"; printf '%s\n' "$got"; fail=1
elif printf '%s\n' "$got" | grep -q "^tmp: true$" \
  && printf '%s\n' "$got" | grep -q "^file: false$" \
  && printf '%s\n' "$got" | grep -q "^missing: false$" \
  && printf '%s\n' "$got" | grep -q "^survived$"; then
    echo "PASS dir_readable"
else
    echo "FAIL dir_readable"; printf '%s\n' "$got"; fail=1
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
if [ -f "examples/.vyto-cache/09_prebuilt_so/libgreeter.so" ]; then
    echo "PASS so_deployed_next_to_exe"
else
    echo "FAIL so_deployed_next_to_exe"
    fail=1
fi

exit $fail
