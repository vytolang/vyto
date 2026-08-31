#!/bin/sh
# vytopack: fetch, hash, cache, lock — and the resolver picking up what it wrote.
#
# Split out of `make test` to keep that target's runtime down; this one shells
# out to git repeatedly. Nothing here needs the network: a throwaway git repo
# in tests/tmp is a real remote for `git clone`, which is vytopack's only
# transport.
set -u
cd "$(dirname "$0")/.."
fail=0

T=tests/tmp/pack
rm -rf "$T"
mkdir -p "$T"
# Absolute, because vytopack is run from inside the project dirs below.
TA=$(cd "$T" && pwd)
ROOT=$(pwd)
export VYTO_PKG_CACHE="$TA/cache"

if ! ./vytoc build src/vytopack/vytopack.vt -o "$TA/vytopack" --modpath src >"$TA/build.log" 2>&1; then
    echo "FAIL vytopack_build"
    head -20 "$TA/build.log"
    exit 1
fi
echo "PASS vytopack_build"

# git is a documented prerequisite, but say so plainly rather than failing
# fifteen checks with an opaque message if it is somehow missing.
if ! command -v git >/dev/null 2>&1; then
    echo "SKIP vytopack_fetch_tests (git is not on PATH)"
    exit 0
fi
PACK="$TA/vytopack"

# --- a throwaway remote ------------------------------------------------------
mkdir -p "$TA/remote/store" "$TA/remote/util"
cat > "$TA/remote/store/store.vt" <<'EOF'
export fn kv_get(k: string): string { return "value of " + k; }
EOF
echo 'export fn helper(): int { return 7; }' > "$TA/remote/util/helper.vt"
(
    cd "$TA/remote"
    git init -q .
    git config user.email t@example.com
    git config user.name test
    git add -A
    git commit -qm v1
    git tag v0.1.0
) >/dev/null 2>&1
URL="file://$TA/remote"

check() { # check <name> <expected> <actual>
    if [ "$2" = "$3" ]; then echo "PASS $1"
    else echo "FAIL $1"; echo "  expected: $2"; echo "  actual:   $3"; fail=1; fi
}

# --- install, then have vytoc resolve what it wrote --------------------------
mkdir -p "$TA/proj"
( cd "$TA/proj" && "$PACK" install --url="$URL" --rev=v0.1.0 ) > "$TA/install.log" 2>&1
if [ -d "$TA/proj/vyto_modules/remote" ] && [ -f "$TA/proj/vyto.lock" ]; then
    echo "PASS vytopack_install"
else
    echo "FAIL vytopack_install"; cat "$TA/install.log"; fail=1
fi

# .git must not survive into the package: it is transport bookkeeping, and it
# would differ between a clone and a tarball of the same commit.
if [ -d "$TA/proj/vyto_modules/remote/.git" ]; then
    echo "FAIL vytopack_strips_git"; fail=1
else
    echo "PASS vytopack_strips_git"
fi

# The whole point: no --modpath, because vyto_modules/ is found by the walk-up.
cat > "$TA/proj/main.vt" <<'EOF'
import { kv_get } from "remote/store";
import { helper } from "remote/util/helper";
fn main() {
    print(kv_get("k"));
    print("helper: " + str(helper()));
}
EOF
got=$(cd "$TA/proj" && "$ROOT/vytoc" run main.vt 2>&1)
check vytopack_resolves "value of k
helper: 7" "$got"

# ...and named by package root, which is what steps 1-2 bought.
if [ -f "$TA/proj/.vyto-cache/main/mod_remote_store.c" ] &&
   [ -f "$TA/proj/.vyto-cache/main/mod_remote_util_helper.c" ]; then
    echo "PASS vytopack_scoped_names"
else
    echo "FAIL vytopack_scoped_names (expected mod_remote_store.c, mod_remote_util_helper.c)"
    fail=1
fi

# --- verify catches tampering ------------------------------------------------
got=$(cd "$TA/proj" && "$PACK" verify 2>&1)
check vytopack_verify_clean "ok       remote" "$got"

echo "// tampered" >> "$TA/proj/vyto_modules/remote/store/store.vt"
if (cd "$TA/proj" && "$PACK" verify >/dev/null 2>&1); then
    echo "FAIL vytopack_verify_tamper (exit 0 on a modified tree)"; fail=1
else
    echo "PASS vytopack_verify_tamper"
fi

# --- the cache is content-addressed and actually hit -------------------------
mkdir -p "$TA/proj2"
cp "$TA/proj/vyto.lock" "$TA/proj2/"
got=$(cd "$TA/proj2" && "$PACK" install --url="$URL" --rev=v0.1.0 2>&1 | grep -c "using cached")
check vytopack_cache_hit "1" "$got"

# --offline must succeed from the cache and fail without it
mkdir -p "$TA/proj3"
cp "$TA/proj/vyto.lock" "$TA/proj3/"
if (cd "$TA/proj3" && "$PACK" install --url="$URL" --rev=v0.1.0 --offline >/dev/null 2>&1); then
    echo "PASS vytopack_offline_hit"
else
    echo "FAIL vytopack_offline_hit"; fail=1
fi
mkdir -p "$TA/proj4"      # no lock, so no cache key: must refuse
if (cd "$TA/proj4" && "$PACK" install --url="$URL" --rev=v0.1.0 --offline >/dev/null 2>&1); then
    echo "FAIL vytopack_offline_miss (fetched anyway)"; fail=1
else
    echo "PASS vytopack_offline_miss"
fi

# A corrupted cache entry must not install. The cached path trusts the lock's
# hash to FIND the entry, so the only thing standing between a tampered cache
# and the project is the post-install re-hash — which makes this the check that
# lets the cache be trusted at all. Nothing may be left behind either.
_ce=$(find "$TA/cache/sha256" -mindepth 2 -maxdepth 2 -type d | head -1)
if [ -n "$_ce" ]; then
    echo "// injected into the cache" >> "$_ce/store/store.vt"
    mkdir -p "$TA/proj5"
    cp "$TA/proj/vyto.lock" "$TA/proj5/"
    _out=$(cd "$TA/proj5" && "$PACK" install --url="$URL" --rev=v0.1.0 2>&1)
    # The package tree must be gone; the empty vyto_modules/ parent may remain.
    if printf '%s' "$_out" | grep -q "hash mismatch" &&
       [ ! -d "$TA/proj5/vyto_modules/remote" ]; then
        echo "PASS vytopack_cache_tamper"
    else
        echo "FAIL vytopack_cache_tamper"; printf '%s\n' "$_out"; fail=1
    fi
    rm -rf "$_ce"      # do not leave a poisoned entry for later cases
else
    echo "FAIL vytopack_cache_tamper (no cache entry to corrupt)"; fail=1
fi

# --- vyto.json: a package naming itself --------------------------------------
#
# Only `name` is read today. It earns its place early because without it the
# name comes from the URL's last component, so a FORK at a different URL
# installs under a different directory and every import into the package
# breaks. The remote below is deliberately named `forked` while its manifest
# says `remote`.
rm -rf "$TA/forked"
cp -a "$TA/remote" "$TA/forked"
rm -rf "$TA/forked/.git"
# The bogus "version" is deliberate: it must be IGNORED, so a value that could
# never come from anywhere else makes a reader immediately visible.
printf '{ "name": "remote", "version": "9.9.9" }\n' > "$TA/forked/vyto.json"
(
    cd "$TA/forked"
    git init -q .
    git config user.email t@example.com
    git config user.name test
    git add -A
    git commit -qm v1
    git tag v0.1.0
) >/dev/null 2>&1

mkdir -p "$TA/mf"
_out=$(cd "$TA/mf" && "$PACK" install --url="file://$TA/forked" --rev=v0.1.0 2>&1)
if [ -d "$TA/mf/vyto_modules/remote" ] && [ ! -d "$TA/mf/vyto_modules/forked" ]; then
    echo "PASS vytopack_manifest_name"
else
    echo "FAIL vytopack_manifest_name (installed under the URL's name, not the manifest's)"
    printf '%s\n' "$_out"; fail=1
fi
# The manifest's own "version" is IGNORED — the git ref is the version, and a
# manifest field would be a second source of truth needing manual sync with the
# tag. It is written as 9.9.9 above precisely so a reader would be visible.
if printf '%s' "$_out" | grep -q "9.9.9"; then
    echo "FAIL vytopack_manifest_version_ignored (read a version from vyto.json)"
    printf '%s\n' "$_out"; fail=1
else
    echo "PASS vytopack_manifest_version_ignored"
fi

# The VERSION IN THE LOCK is the requested ref, which is what answers "which
# version is installed?". Without it the lock keeps only a sha and every
# installed package is unidentifiable at a glance.
if grep -q '"version": "v0.1.0"' "$TA/mf/vyto.lock" 2>/dev/null; then
    echo "PASS vytopack_lock_version"
else
    echo "FAIL vytopack_lock_version"; cat "$TA/mf/vyto.lock" 2>&1; fail=1
fi
got=$(cd "$TA/mf" && "$PACK" list 2>&1 | tr -s ' ')
case "$got" in
    *"remote v0.1.0 "*) echo "PASS vytopack_list_version" ;;
    *) echo "FAIL vytopack_list_version"; printf '%s\n' "$got"; fail=1 ;;
esac

# A branch is a version too — the case a manifest field could never express.
mkdir -p "$TA/mfbranch"
( cd "$TA/mfbranch" && "$PACK" install --url="file://$TA/forked" --rev=master \
    >/dev/null 2>&1 ) || ( cd "$TA/mfbranch" && "$PACK" install \
    --url="file://$TA/forked" --rev=main >/dev/null 2>&1 )
if grep -qE '"version": "(master|main)"' "$TA/mfbranch/vyto.lock" 2>/dev/null; then
    echo "PASS vytopack_lock_version_branch"
else
    echo "FAIL vytopack_lock_version_branch"; cat "$TA/mfbranch/vyto.lock" 2>&1; fail=1
fi

# A lock written before `version` existed must still load and verify.
mkdir -p "$TA/oldlock"
cp -a "$TA/mf/vyto_modules" "$TA/oldlock/"
sed '/"version":/d' "$TA/mf/vyto.lock" > "$TA/oldlock/vyto.lock"
got=$(cd "$TA/oldlock" && "$PACK" verify 2>&1)
check vytopack_lock_no_version "ok       remote" "$got"
# --name is the human in front of the terminal and outranks the manifest
mkdir -p "$TA/mfname"
_out=$(cd "$TA/mfname" && "$PACK" install --url="file://$TA/forked" --rev=v0.1.0 \
        --name=chosen 2>&1)
if [ -d "$TA/mfname/vyto_modules/chosen" ]; then
    echo "PASS vytopack_manifest_name_override"
else
    echo "FAIL vytopack_manifest_name_override"; printf '%s\n' "$_out"; fail=1
fi

# A manifest that EXISTS but is broken is an error, not a silent fallback: the
# package made a mistake worth surfacing, and falling back would hide it until
# the import paths came out wrong. A path-traversal name goes through the same
# check as --name.
mfbad() { # mfbad <label> <json> <expected substring>
    rm -rf "$TA/mfb"
    cp -a "$TA/forked" "$TA/mfb"
    rm -rf "$TA/mfb/.git"
    printf '%s' "$2" > "$TA/mfb/vyto.json"
    (
        cd "$TA/mfb"
        git init -q .
        git config user.email t@example.com
        git config user.name test
        git add -A
        git commit -qm v1
        git tag v0.1.0
    ) >/dev/null 2>&1
    rm -rf "$TA/mfbapp"
    mkdir -p "$TA/mfbapp"
    if _o=$(cd "$TA/mfbapp" && "$PACK" install --url="file://$TA/mfb" --rev=v0.1.0 2>&1); then
        echo "FAIL vytopack_manifest_$1 (accepted)"; fail=1
    elif ! printf '%s' "$_o" | grep -q "$3"; then
        echo "FAIL vytopack_manifest_$1 (wrong reason)"; printf '%s\n' "$_o"; fail=1
    else
        echo "PASS vytopack_manifest_$1"
    fi
}
mfbad bad_json    '{ "name": '            "not valid JSON"
mfbad bad_type    '{ "name": 42 }'        "must be a string"
mfbad bad_name    '{ "name": "a/../b" }'  "not allowed"
mfbad not_object  '["nope"]'              "must be a JSON object"

# --- a hand-installed package is a first-class one ---------------------------
#
# vytopack has ONE transport (git), and this is why that is enough: somebody
# without git downloads a tarball and unzips it into vyto_modules/<name>/, and
# both the resolver and `verify` treat it exactly like a fetched one. A second
# forge-tarball transport would have bought convenience for a case that already
# works, and charged libcurl in the toolchain build for it.
#
# It works because the digest is over the TREE rather than an archive: a
# `git archive` of a commit and a clone of it hash the same. If that ever stops
# holding, hand-installing silently degrades to "verify always fails".
mkdir -p "$TA/manual/vyto_modules/remote"
git -C "$TA/remote" archive --format=tar v0.1.0 \
    | tar -x -C "$TA/manual/vyto_modules/remote" 2>/dev/null
cp "$TA/proj/vyto.lock" "$TA/manual/"
cp "$TA/proj/main.vt" "$TA/manual/"
got=$(cd "$TA/manual" && "$PACK" verify 2>&1)
check vytopack_manual_verify "ok       remote" "$got"
got=$(cd "$TA/manual" && "$ROOT/vytoc" run main.vt 2>&1)
check vytopack_manual_builds "value of k
helper: 7" "$got"

# --- refusals ----------------------------------------------------------------
#
# vyto/os's capture() is popen(3), so an unvalidated URL or rev would be
# EXECUTED rather than fetched.
#
# Each case asserts the REASON, not merely that the command failed. That
# distinction is the whole test: an install carrying `--rev='v1;touch x'` fails
# either way, because with validation off `git ls-remote` simply finds no such
# ref. Fault injection proved it — stubbing urlProblem/revProblem to return ""
# left an exit-status-only check passing on all four cases, so it was
# verifying nothing. Matching the refusal text is what makes it real.
mkdir -p "$TA/bad"
rm -f "$TA/PWNED"
reject() { # reject <name> <expected-substring> <arg>...
    _name=$1; _want=$2; shift 2
    # Capture status separately: `_out=$(...)` makes $? the assignment's.
    if _out=$(cd "$TA/bad" && "$PACK" install --url="$URL" --rev=v0.1.0 "$@" 2>&1); then
        echo "FAIL vytopack_reject_$_name (accepted)"; fail=1
    elif ! printf '%s' "$_out" | grep -q "$_want"; then
        echo "FAIL vytopack_reject_$_name (rejected, but not by the check under test)"
        echo "  wanted substring: $_want"
        echo "  got:              $_out"
        fail=1
    else
        echo "PASS vytopack_reject_$_name"
    fi
}
reject url_inject   "not allowed" "--url=$URL;touch $TA/PWNED"
reject rev_inject   "not allowed" "--rev=v1;touch $TA/PWNED"
reject rev_subshell "not allowed" "--rev=v\$(touch $TA/PWNED)"
reject dir_absolute "must be relative" "--dir=/etc/vyto"
reject dir_escape   "outside the project" "--dir=../../../evil"
if [ -e "$TA/PWNED" ]; then
    echo "FAIL vytopack_no_shell_execution (payload ran)"; fail=1
else
    echo "PASS vytopack_no_shell_execution"
fi
# Nothing may have been written outside the project by any of those.
if [ -e "$TA/evil" ] || [ -e /etc/vyto ]; then
    echo "FAIL vytopack_no_escape (wrote outside the project)"; fail=1
else
    echo "PASS vytopack_no_escape"
fi

if [ "$fail" -eq 0 ]; then echo "all vytopack tests passed"; else echo "vytopack tests FAILED"; fi
exit $fail
