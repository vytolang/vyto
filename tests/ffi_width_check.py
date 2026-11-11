#!/usr/bin/env python3
"""Check that every Vyto `extern "C"` declaration agrees with the C function
it binds on integer width, on every target.

The bug this exists for: a shim written as `long f(...)` and declared in Vyto
as `fn f(...): i64`. On Linux and macOS `long` is 64 bits (LP64) and the two
agree; on Windows it is 32 bits (LLP64), so the call returns half a value and
-1 arrives as 4294967295. That shipped in a score of shims and was found only
by running the suite on a real Windows machine. Nothing in a Linux build can
show it, which is why this compares declarations instead of behaviour.

Rules, per parameter and return:
  Vyto i64/u64  needs a C type that is 64-bit everywhere (int64_t, uint64_t,
                long long, size_t, ssize_t, intptr_t, ptrdiff_t). C `long`
                is wrong: 32-bit on Windows.
  Vyto i32/u32  needs a C type that is 32-bit everywhere. C `long` is wrong:
                64-bit on Linux.
  Vyto clong/culong  is C long exactly; a fixed-width C type is wrong.
Pointers, floats and anything the script cannot classify are not checked.

Only functions defined in this repo's package shims (native/src, vendored
trees excluded) are checked; externs into system libraries are their headers'
business. Exit 0 clean, 1 with a list of mismatches.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCAN_VT = ["lib", "apps", "examples", "tests"]
# Vendored code: not ours to edit, and it never declares Vyto externs.
VENDORED = ("pcre2", "sqlite", "zlib", "blend2d", "stb", "uECC", "micro-ecc", "/icu", "/curl")

C64 = {"int64_t", "uint64_t", "long long", "unsigned long long", "long long int",
       "size_t", "ssize_t", "intptr_t", "uintptr_t", "ptrdiff_t"}
C32 = {"int", "unsigned", "unsigned int", "int32_t", "uint32_t", "signed int"}
CLONG = {"long", "unsigned long", "long int", "unsigned long int", "signed long"}
V64 = {"i64", "u64"}
V32 = {"i32", "u32"}
VLONG = {"clong", "culong"}


def norm_c(t):
    t = re.sub(r"\b(const|volatile|static|inline|extern|register)\b", " ", t)
    t = re.sub(r"__attribute__\s*\(\(.*?\)\)", " ", t)
    t = re.sub(r"\s+", " ", t).strip()
    return t


def classify_c(t):
    t = norm_c(t)
    if "*" in t or "[" in t:
        return None
    if t in C64:
        return "64"
    if t in C32:
        return "32"
    if t in CLONG:
        return "long"
    return None


def classify_v(t):
    t = t.strip()
    if t in V64:
        return "64"
    if t in V32:
        return "32"
    if t in VLONG:
        return "long"
    return None


def c_params(s):
    s = s.strip()
    if s in ("", "void"):
        return []
    out = []
    for p in s.split(","):
        p = p.strip()
        # drop the parameter name: the last identifier, unless the whole thing
        # is a bare type
        m = re.match(r"^(.*?)([A-Za-z_]\w*)\s*$", p)
        if m and m.group(1).strip() and not m.group(1).strip().endswith(("struct", "enum")):
            out.append(m.group(1))
        else:
            out.append(p)
    return out


DEF_RE = re.compile(
    r"^(?!static\b)([A-Za-z_][\w \t\*]*?)\b([A-Za-z_]\w*)\s*\(([^;{}()]*)\)\s*\{",
    re.M)


def c_index():
    defs = {}
    for top in ("lib", "apps", "examples", "tests"):
        for dirpath, _, files in os.walk(os.path.join(ROOT, top)):
            if "/native/src" not in dirpath.replace(os.sep, "/") + "/":
                continue
            if any(v in dirpath for v in VENDORED):
                continue
            for f in files:
                if not f.endswith(".c"):
                    continue
                path = os.path.join(dirpath, f)
                text = open(path, encoding="utf-8", errors="replace").read()
                text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
                text = re.sub(r"//[^\n]*", " ", text)
                for m in DEF_RE.finditer(text):
                    ret, name, params = m.group(1), m.group(2), m.group(3)
                    if name in ("if", "while", "for", "switch", "return", "sizeof"):
                        continue
                    defs.setdefault(name, []).append((ret, c_params(params), path))
    return defs


EXT_RE = re.compile(r'extern\s+"C"\s*\{(.*?)\n\}', re.S)
FN_RE = re.compile(r"\bfn\s+([A-Za-z_]\w*)\s*\(([^)]*)\)\s*(?::\s*([\w\[\]]+))?\s*;", re.S)


def vt_externs():
    for top in SCAN_VT:
        for dirpath, _, files in os.walk(os.path.join(ROOT, top)):
            if ".vyto-cache" in dirpath or "/tests/tmp" in dirpath.replace(os.sep, "/"):
                continue
            for f in files:
                if not f.endswith(".vt"):
                    continue
                path = os.path.join(dirpath, f)
                text = open(path, encoding="utf-8", errors="replace").read()
                text = re.sub(r"//[^\n]*", " ", text)
                for blk in EXT_RE.finditer(text):
                    for m in FN_RE.finditer(blk.group(1)):
                        name, params, ret = m.group(1), m.group(2), m.group(3)
                        ptypes = []
                        for p in params.split(","):
                            p = p.strip()
                            if not p:
                                continue
                            ptypes.append(p.split(":", 1)[1].strip() if ":" in p else p)
                        yield path, name, ptypes, ret


def why(v, c):
    if v == "64" and c == "long":
        return "C long is 32-bit on Windows"
    if v == "32" and c == "long":
        return "C long is 64-bit on Linux"
    if v == "long" and c in ("64", "32"):
        return "clong/culong must bind C long"
    if v == "64" and c == "32":
        return "32-bit C type"
    if v == "32" and c == "64":
        return "64-bit C type"
    return None


def main():
    defs = c_index()
    problems = []
    checked = 0
    for vpath, name, ptypes, ret in vt_externs():
        if name not in defs:
            continue
        for cret, cparams, cpath in defs[name]:
            checked += 1
            pairs = [("return", ret or "", cret)]
            if len(cparams) == len(ptypes):
                pairs += [(f"param {i + 1}", ptypes[i], cparams[i]) for i in range(len(ptypes))]
            for where, vt, ct in pairs:
                v, c = classify_v(vt), classify_c(ct)
                if v is None or c is None or v == c:
                    continue
                reason = why(v, c)
                if reason:
                    problems.append(
                        f"{os.path.relpath(cpath, ROOT)}: {name} {where}: Vyto {vt} "
                        f"vs C '{norm_c(ct)}' ({reason}); declared in {os.path.relpath(vpath, ROOT)}")
    for p in sorted(set(problems)):
        print(p)
    print(f"ffi width check: {checked} bindings checked, {len(set(problems))} mismatches")
    # A check of this shape passes trivially once a regex stops matching. 1124
    # bindings were seen when it was written; far fewer means it went blind.
    if checked < 800:
        print(f"ffi width check: only {checked} bindings found, expected ~1100; "
              "the parser no longer sees the externs or the shims")
        return 1
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
