# vyto/terminal — raw keys, terminal state, and a live-filter prompt

The controlling terminal: one keypress at a time, the window size, unbuffered
output, and ANSI escapes. It is the layer a CLI needs to do anything a plain
`print()` loop cannot — which, before this, was everything.

```vyto
import { raw_mode, KEY_UP, KEY_ESC, emit } from "vyto/terminal";

let t = raw_mode();                 // null when stdin is not a tty
if (t != null) {
    let k = t.key(-1);              // blocks until a key arrives
    if (k == KEY_UP) { emit("up\n"); }
}                                   // t drops -> terminal restored
```

Three modules:

| Module | What |
|---|---|
| `vyto/terminal` | raw mode, `key()`, `size()`, `isatty()`, `emit()` |
| `vyto/terminal/ansi` | colour, cursor movement, clearing, escape-aware `width` |
| `vyto/terminal/prompt` | a query field that filters as you type |

## The prompt

The reason the package exists. A lookup tool wants the user to type and watch
candidates narrow, which line input cannot do:

```vyto
import { prompt } from "vyto/terminal/prompt";

fn matches(q: string): string[] { return search(q); }

let p = prompt("how to ");
p.on_filter(matches);               // a NAMED fn — see below
let picked = p.run();               // "" if they escaped out
```

The filter has to be a **named** function: an inline `fn (q: string) ...`
literal is not an expression in Vyto and is a parse error at the call site.

Type to filter, `↑↓` to move, `Enter` to commit, `Esc` to clear and `Esc` again
to leave, `Ctrl+D` on an empty query to give up. It redraws in place and erases
itself on exit, so scrollback is untouched and it works as one step inside an
ordinary session rather than taking the screen over.

`Enter` with nothing matching returns `""` rather than doing nothing — ignoring
the key looks exactly like a hang to whoever just mistyped.

> **`Ctrl+D` is not EOF in raw mode.** Clearing `ICANON` stops the line
> discipline translating it, so it arrives as the ordinary control byte 4. The
> prompt handles it; anything reading `key()` directly must test for `4` itself
> rather than waiting for `EOF`. A terminal that actually goes away shows up as
> `SIGHUP`, not as `EOF`.

## Restoring the terminal is the hard part

Raw mode turns off echo. A program that exits without restoring leaves the user
typing blind into a shell that shows nothing back — they have to know to run
`reset`, from memory, with no feedback while they type it.

So there is no `close()`. The restore runs from three places, because each
covers an exit the one before it cannot. All three were found by measuring, not
by reasoning:

| Exit path | Covered by | Why the previous one misses it |
|---|---|---|
| `return`, dropping the ref | `deinit` | — |
| `panic()` | `atexit` | `panic` is `vt_host_abort()` → `exit(101)` (`runtime/vyto_rt.c:553`), which unwinds nothing. Measured: left the tty at `-echo`. |
| **Ctrl+C**, `kill`, terminal closed | signal handlers | `SIGINT` terminates without running `atexit` handlers at all. Measured: left the tty at `-echo -icanon`. |

The third is the one that matters in practice — **Ctrl+C is how people quit an
interactive program**, not an edge case. `SIGINT`, `SIGTERM`, `SIGQUIT` and
`SIGHUP` are handled; each restores the terminal, puts the previous disposition
back, and re-raises, so the process still dies *by that signal* rather than
exiting 0 and the parent shell's job control still reads correctly.

Handlers are installed **only where the application has not already set one**
(the previous disposition is `SIG_DFL`), and are removed again when raw mode
ends. A program that installs its own `SIGINT` handler keeps it — verified
directly, along with the removal, so a program that leaves raw mode and keeps
running is exactly as it was.

The restore itself is idempotent and async-signal-safe: `tcsetattr` only, no
`free`, no stdio.

`SIGKILL` and `SIGSTOP` remain uncoverable by anything, by design.

**Hold the `Term` while you want raw mode:**

```vyto
raw_mode().key(-1);       // DO NOT — restored before the key is read
```

The `Term` is unreferenced the moment that expression ends. It looks like a
reasonable one-liner, which is why it is worth stating.

## Not a terminal is a normal state

Under a pipe, a CI runner, or a freestanding target: `isatty()` is false,
`raw_mode()` returns null, `prompt.run()` returns `""` immediately rather than
blocking a pipeline, and `size()` reports the conventional 80×24.

Every caller needs that path anyway, and it is the path the test suite
exercises — `examples/100_terminal.vt` runs with stdin redirected precisely so
the golden captures it.

## Colour is gated on stdout, not stdin

Every styling function in `ansi` returns `""` when stdout is not a terminal, and
honours [`NO_COLOR`](https://no-color.org). That is what keeps `prog > file` and
`prog | grep` free of escape bytes — the same constraint that has kept
`vyto/cli` monochrome (`lib/vyto/cli/README.md`).

The gate reads **stdout** deliberately: `prog | less` has an interactive stdin
and a piped stdout, so deciding on stdin would write escapes into the pipe.

Cursor movement and clearing are *not* disabled by `NO_COLOR` — they are layout,
not decoration, and a full-screen program still has to work for someone who
turned colour off. Measured on a terminal with `NO_COLOR=1`: `paint()` drops
from 10 bytes to 1, while `clear_line()` and `to_col()` stay at 4.

Both gates are answered once in C and cached, which is why `ansi.vt` calls into
the shim at all rather than being pure Vyto. Every helper here consults one of
them, so a redraw emitting a few hundred styled spans was making a few hundred
`isatty(2)` calls per frame: 200k `paint()` calls went from 562 ms to 54 ms
(2.81 µs → 0.27 µs each, ~10×). Caching is sound because a descriptor cannot
become a terminal mid-process and the environment is read at startup — and it
lives in C because Vyto has no module-level `let` and no static class fields
(`lib/vyto/dev/test/test.vt:42`), so a module has nowhere to keep one.

### Measuring styled text

`s.len` counts escape bytes, so padding styled text into columns on it indents
by however many bytes the colour happened to cost. Use `width()`:

```vyto
width(paint("hello", RED))    // 5, not 14
truncate(s, 40)               // 40 printable columns, escapes preserved
```

Both count bytes rather than grapheme clusters, so a multi-byte UTF-8 character
still over-counts — the same limit as the rest of the stdlib's column handling.

## Key codes match `vyto/surface`

`KEY_ENTER` is 1000 here and 1000 there. A printable character is its own byte
value, so there is no constant for it — test `k >= 32 && k < 127` and use
`chr(k)`.

The numbering is shared so that a terminal backend for `vyto/ui`
(`local/docs/MODULES.md`, Tier C) can consume either event source without a
translation table. **This package is not that backend** — it deliberately ships
no widgets, no `Painter`, and no layout. It is the layer underneath one.

## Escape sequences are decoded in C

An arrow key is three bytes on POSIX (`ESC [ A`) and a single virtual-key record
on Win32. Decoding in the shim means `key()` returns one code per call on both,
so the Vyto side carries no platform conditionals.

Telling a real `Escape` keypress from the start of an arrow sequence is a
timing question — both begin with `0x1b`, and the only difference is that a
terminal sends the rest immediately while a human cannot. The budget is **20 ms**:
shorter and arrows register as Escape over a slow link, longer and Escape feels
sticky.

`Ctrl+C` still interrupts. `ISIG` is left enabled deliberately — a
beginner-facing tool that swallows `Ctrl+C` is a trap — which is exactly why the
`SIGINT` handler above has to exist: leaving `ISIG` on means the signal really
is delivered, and the terminal has to be put back on the way out.

## Testing

| What | Where |
|---|---|
| The not-a-terminal path, byte-for-byte | `examples/100_terminal.vt` + `.expected` |
| Escape construction and `width`/`truncate` | `tests/unit/terminal_ansi.vt` (`make test-dev`) |
| The `VT_NO_LIBC` arm still compiles | `tests/fixtures/terminal_freestanding.vt` |
| **The terminal is restored when the process is killed** | `tests/terminal_signal_check.py` + `tests/fixtures/terminal_signal_check.c` |

The last one forks a pty, lets the child enter raw mode, kills it with
`SIGINT`/`SIGTERM`/`SIGHUP`, and then reads the *master's* termios — a dead
process cannot report on its own terminal, which is why it is a Python helper
rather than a shell loop. It also checks that an application's own `SIGINT`
handler survives and that the shim's handlers are removed on restore.

It was fault-injected: removing the signal hook turns 4 of its 5 cases red with
`terminal left in raw mode`. Worth redoing if it is ever refactored — a check of
this shape passes trivially once it stops finding anything, which is why the
child must hand-shake that it really reached raw mode before the kill.

The rest of the interactive path is not covered and cannot be — raw mode exists
to react to a human. It was verified by driving the binary through a real pty:
live filtering, `↑↓` clamping at both ends, backspace, the two-stage Escape,
Enter on an empty list, and rendering at the bottom of a 6-row screen (replayed
through a VT model, so the assertion is on the resulting *screen* rather than on
a byte stream).

> **The freestanding check asserts on symbols, not just exit status — and it has
> to.** Fault injection showed that bypassing the `VT_NO_LIBC` guard entirely
> still builds green: the output is an **archive**, and `ar` resolves nothing, so
> the POSIX arm's undefined `tcgetattr`/`poll`/`ioctl` sit in the `.a`
> unnoticed. An exit-status-only check proves only that the compiler ran.
> `tests/run_tests.sh` therefore greps `nm` output for hosted-libc symbols, and
> that version was re-injected to confirm it goes red.
>
> The other freestanding checks in the suite (mmap, openssl, image, compress) are
> exit-status-only and share this blind spot.
