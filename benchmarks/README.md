# Benchmarks

Cross-language micro-benchmarks comparing **Vyto** against C and (where present)
Python on the same task. Each benchmark `NN_name` ships up to three sources:

| File | How to build / run |
|------|--------------------|
| `NN_name.vt` | `../vytoc build NN_name.vt --release -o NN_name_vyto && ./NN_name_vyto` |
| `NN_name.c`  | `cc -O2 NN_name.c -o NN_name_c && ./NN_name_c` |
| `NN_name.py` | `python3 NN_name.py` |

Build products (`*_vyto`, `*_c`, `logs.db`, large sample logs) are git-ignored —
only the sources are tracked. Run from **inside** `benchmarks/` (a couple of the
tasks read/write paths like `benchmarks/logs.db` relative to the repo root — check
the individual source if in doubt).

Rough recipe to run one across all three languages and time them:

```sh
cd benchmarks
../vytoc build 01_fibonacci.vt --release -o 01_fibonacci_vyto
cc -O2 01_fibonacci.c -o 01_fibonacci_c
for b in ./01_fibonacci_vyto ./01_fibonacci_c "python3 01_fibonacci.py"; do
    printf '%-28s ' "$b"; /usr/bin/time -f '%e s' $b >/dev/null
done
```

## Notes per benchmark

- **08_sqlite** — needs SQLite. Install the system dev package
  (`sudo apt install libsqlite3-dev`); the C build links `-lsqlite3` and the Vyto
  binding (`sqlite3.vt`) links the system library too. The large SQLite
  amalgamation is **not** vendored here — install the package instead.
- **09_threads** — ⚠️ **do not run `09_threads.py`.** The Python variant pins the
  machine (thread thrash); it exists only for source comparison. The `.c`/`.vt`
  variants are fine.
- **11_widget_stress** — exercises the `vyto/ui` toolkit; needs the UI stack
  (X11), so it is a Vyto-only stress test with no C/Python counterpart.

These are indicative micro-benchmarks, not a rigorous suite — numbers vary with
CPU, libc, and compiler. Use `--release` for the Vyto builds and `-O2` for C to
compare like with like.
