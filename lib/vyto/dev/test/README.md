# vyto/dev/test — unit tests that assert on values

The `examples/` suite diffs a program's stdout against a golden. That is a good
way to test deterministic output, and a bad way to test three things the stdlib
needs:

- **a refusal** — did this fail, and for the *right* reason;
- **a non-deterministic result** — a fresh keypair differs every run;
- **a property over generated input** — do these two implementations agree on
  all 256 bytes.

This package covers those. It does not replace `examples/`, which is
documentation that happens to be executable and should stay that way.

```vyto
import { suite } from "vyto/dev/test";
import { exitWith } from "vyto/dev/test/exit";

fn main() {
    let t = suite("hex");

    t.section("known vectors");
    t.eqInt("digit_0", hex_val(48), 0);
    t.eqInt("upper_A", hex_val(65), 10);

    t.section("agreement over the whole domain");
    for (let c in 0..256) {
        t.eqInt(`url_vs_mime_{c}`, hex_url(c), hex_mime(c));
    }

    exitWith(t.done());
}
```

Put the file in `tests/unit/` and it runs under `make test-dev`. Nothing else
is needed — the runner globs the directory.

## Modules

| Module | What |
|---|---|
| `vyto/dev/test` | `suite()` and class `T` — every assertion, and `done()` |
| `vyto/dev/test/exit` | `exitWith(code)`, the only thing that touches `vyto/os` |

## The assertions

| Method | Notes |
|---|---|
| `ok(label, cond)` | the general one; no got/want block, a bool has nothing to show |
| `eq(label, got, want)` | strings |
| `eqInt` · `eqBool` | |
| `eqFloat(label, got, want, eps = 1e-9)` | a tolerance, never `==`. NaN fails against everything, including itself |
| `contains(label, hay, needle)` | substring, for messages and rendered output |
| `eqIntArr` · `eqStrArr` | reports the first differing index, or the length |
| `section(name)` | a divider; carries no verdict |
| `skip(label, why)` | counted apart from a pass, so a suite that skips everything cannot read green |
| `fail(label, why)` | unconditional, for "control reached here" |
| `done()` | prints the summary, **returns** the exit code |

Every assertion also returns its own verdict, for the case where continuing is
not merely noisy but fatal:

```vyto
if (!t.ok("parsed", r != null)) { return; }   // next line would deref null
t.eqInt("rows", r.rows.len, 3);
```

Use it only for that. Wrapping every assertion in an `if` buys nothing.

Parameterized tables need no API — a `for` loop with a template-literal label
reads better than any fixture DSL, as the agreement example above shows.

## Why it is shaped this way

**An assertion prints and continues; it never panics.** `panic()` cannot be
caught — it writes to stderr and calls `vt_host_abort()`, which is `exit(101)`
(`runtime/vyto_rt.c:140`, `:553`). An assertion that panicked would report
exactly one failure per run.

**There is no global registry, and no auto-discovery.** The top level of a Vyto
file holds only import/const/fn/struct/enum/class/extern (`src/parse.c:1040`),
classes have static methods but not static fields (`src/parse.c:932`), and
there is no reflection. So the suite is an ordinary object threaded through
`main()`, and discovery is the runner globbing files.

**Four typed `eq*` rather than one generic.** Generics are monomorphized with no
bounds, so `eq<A>` would emit a copy per type and still could not format
got/want — there is no printable bound to call through.

**`done()` returns the code instead of exiting.** It keeps this module free of
`vyto/os`, and it keeps `done()` inspectable: the selftest asserts on what it
returns, which is impossible if the call ends the process.

**`exitWith` is a separate module** because there is no `exit` builtin, so
setting an exit status means `vyto/os`, which carries `native/src/os_shim.c` —
and a `native/src` compiles once per package *directory* for every module in it.
Splitting it means importing `vyto/dev/test` alone pulls in no native code. For
the record, so the split is not re-litigated: that shim is one small libc-only
translation unit with no `#link` directives, content-addressed into the shared
object cache (`src/main.c:245-258`). Nothing like the link cost that justified
splitting `vyto/crypto/ecc` or `vyto/validator/pattern`.

## How this is kept from going vacuous

A test framework that passes when broken is worse than none.

`tests/unit/selftest.vt` exercises every assertion **in both directions** — once
passing, once deliberately failing — and is diffed byte for byte against
`selftest.expected`. The failing half is the load-bearing half: a framework
whose `eqInt()` returned true unconditionally would sail through a suite made
only of true assertions. The witness is `diff`, which has no relationship to
this code.

Four more files are negative tests **of the runner**, each reported as passing
only when the runner scores it FAILING. The runner's verdict is
`exit status 0` **and** a summary line matching `^# .* ok$`, and that grep asks
two questions at once — is there a summary, and does it say green:

| Fixture | Caught by | Why it is separate |
|---|---|---|
| `red_suite` | the grep (summary ends `FAILED`) | the ordinary case |
| `lying_suite` | **the exit status only** | prints a well-formed *green* summary and exits 1 anyway; nothing else reaches that clause |
| `silent_exit` | **the summary line only** | returns from `main()` without `exitWith(t.done())`, so it exits 0 having asserted almost nothing |
| `aborts_midway` | either | panics, so it exits 101 *and* prints no summary — which is why it cannot stand in for the other two |

All four run through the **same** verdict script the ordinary suites use.
Reimplementing the logic in the check would make it vacuous — a hand-written
copy keeps passing when the real check breaks, because the two paths never meet.
That is not hypothetical; see row 5 below.

### Fault-injection record

Performed 2026-09-04, when the framework was written. Each fault was injected,
`make test-dev` run, and the fault reverted. **Redo this if the verdict logic or
the selftest is ever refactored** — a check of this shape passes trivially once
it stops finding anything.

| Injected fault | Caught by |
|---|---|
| 1. `eqInt` returns true unconditionally | `selftest` golden, `red_suite` |
| 2. `done()` returns 0 unconditionally | `selftest` (exit 0, expected 1) |
| 3. `failed` counter never incremented | `selftest` golden, `red_suite` |
| 4. `fail()` prints `ok` instead of `not ok` | `selftest` golden |
| 5. runner's summary-line grep removed | **`silent_exit` alone** |
| 6. runner's exit-status check removed | **`lying_suite` alone** |

Rows 5 and 6 are the reason there are four negative fixtures rather than one.
The first attempt at row 5 passed with the grep deleted, because the abort check
had been written as its own copy of the logic and never touched the code being
broken; sharing one verdict script fixed it. Row 6 then still passed, because
every fixture at that point was already caught by the grep — `lying_suite`
exists specifically to depend on the exit status and nothing else.

## What this cannot do

- **Assert that something panics.** Deferred. It needs a subprocess, since a
  panic cannot be caught, and two traps are already known: `os.capture` is
  `popen(cmd, "r")` — **stdout only** (`lib/vyto/os/native/src/os_shim.c:200`),
  so the command needs `2>&1` baked in or the message is never seen; and
  `vt_panic_c` formats into a `char[256]` (`runtime/vyto_rt.c:554`), so a long
  message truncates and the match must be a substring, never an equality. Assert
  exit code 101 **and** the message — either alone is vacuous. Compile-time
  refusals are already covered by `tests/errors/*.expected-error`.
- **Assert allocation counts or detect leaks.** Nothing in the runtime exposes
  a counter — `vt_weak_live` (`runtime/vyto_rt.c:431`) is `static` and counts
  registered weak slots, not live objects. This would need new runtime C, which
  should be proposed on its own merits rather than smuggled in for a dev tool.
- **Benchmark.** Timings cannot be golden-diffed, which is the property
  everything here rests on. That is a separate package.
- **Set up and tear down fixtures.** There is no `defer` and no `catch`, so a
  teardown cannot be guaranteed to run. An API that silently fails to clean up
  is worse than code at the top of `main()`.
