#!/bin/sh
# pe_audit.sh — will these Windows binaries load on Windows 7?
#
#   tests/win/pe_audit.sh app.exe [more.exe some.dll ...]
#
# Reads each binary's import table with mingw objdump and checks every
# imported function against tests/win/win7_exports.txt, the exports of a real
# Windows 7 install (dumped there by dump_exports.exe; set WIN7_EXPORTS to use
# another list). A function missing from that list is one Windows 7 does not
# have: the exe would refuse to start with "entry point not found".
#
# Also fails on:
#   - any api-ms-win-crt-* import: the Universal CRT, absent from Windows 7
#     unless KB2999226 is installed (a mingw UCRT toolchain or a third-party
#     DLL can pull it in)
#   - a DLL that is neither a Windows 7 system DLL nor shipped next to the
#     binary
# A DLL shipped next to the binary (libblend2d.dll, libcurl-x64.dll) is the
# app's own and is not checked here; audit it as an argument of its own.
#
# Exit status: 0 clean (or no allowlist yet: prints SKIP), 1 problems found.
set -u
OBJDUMP=${OBJDUMP:-x86_64-w64-mingw32-objdump}
LIST=${WIN7_EXPORTS:-$(dirname "$0")/win7_exports.txt}

if [ $# -eq 0 ]; then
    echo "usage: $0 file.exe [file.dll ...]" >&2
    exit 2
fi
if [ ! -f "$LIST" ]; then
    echo "SKIP pe_audit (no $LIST yet: run dump_exports.exe on Windows 7, see run.ps1)"
    exit 0
fi
if ! command -v "$OBJDUMP" >/dev/null 2>&1; then
    echo "SKIP pe_audit (no $OBJDUMP)"
    exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
# A dump copied straight off Windows has CRLF line ends; match on the names.
tr -d '\r' < "$LIST" | grep -v '^#' | sort -u > "$tmp/exports"
cut -d: -f1 "$tmp/exports" | sort -u > "$tmp/dlls"

status=0
for bin in "$@"; do
    if [ ! -f "$bin" ]; then
        echo "FAIL pe_audit $bin (no such file)"
        status=1
        continue
    fi
    # "dll:function" per import. An import by ordinal has no name to check
    # and is reported as such.
    "$OBJDUMP" -p "$bin" | awk '
        /^[ \t]*DLL Name:/ { dll = tolower($3); inlist = 0; next }
        dll != "" && /Member-Name/ { inlist = 1; next }
        inlist && NF == 0 { inlist = 0; dll = ""; next }
        inlist && NF >= 3 { print dll ":" $3; next }
        inlist && NF == 2 { print dll ":<ordinal " $2 ">"; next }
    ' | sort -u > "$tmp/imports"
    # DLLs shipped beside the binary, lowercased
    ls -1 "$(dirname "$bin")" | tr 'A-Z' 'a-z' | grep '\.dll$' > "$tmp/shipped" || true
    problems=0
    name=$(basename "$bin")
    : > "$tmp/reported"
    while IFS= read -r imp; do
        dll=${imp%%:*}
        fn=${imp#*:}
        if grep -qx "$dll" "$tmp/shipped"; then continue; fi
        if grep -qx "$dll" "$tmp/reported"; then continue; fi
        case "$dll" in
            api-ms-win-crt-*)
                echo "  $name: imports the Universal CRT ($dll), which Windows 7 lacks without KB2999226"
                echo "$dll" >> "$tmp/reported"
                problems=1; continue ;;
        esac
        if ! grep -qx "$dll" "$tmp/dlls"; then
            echo "  $name: $dll is not a Windows 7 system DLL and is not shipped beside the binary"
            echo "$dll" >> "$tmp/reported"
            problems=1; continue
        fi
        case "$fn" in
            "<ordinal"*)
                echo "  $name: $dll imported by ordinal ($fn), cannot be checked by name"
                problems=1; continue ;;
        esac
        if ! grep -qxF "$imp" "$tmp/exports"; then
            echo "  $name: $dll!$fn does not exist on Windows 7"
            problems=1
        fi
    done < "$tmp/imports"
    if [ "$problems" -eq 0 ]; then
        echo "PASS pe_audit $name ($(wc -l < "$tmp/imports") imports)"
    else
        echo "FAIL pe_audit $name"
        status=1
    fi
done
exit $status
