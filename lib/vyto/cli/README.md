# vyto/cli — flags, options, operands & subcommands

Pure Vyto. Help columns use the `pad_end` string builtin and help text is
assembled with `string[].join`, so nothing here pulls in `vyto/util/fmt`
(native-shim-backed) or `vyto/os`. Its one import is `vyto/util/parse`, which is
itself import-free and shim-free, so this still runs wherever the runtime runs —
including a freestanding target. Both halves of that are verified by building
against `--freestanding`.

It exists because every app in this repo hand-rolled the same `args()` loop, and
they were all wrong in the same four ways: unknown flags silently ignored,
unknown values silently falling back to a default, mistyped numbers killing the
process through `to_int`'s panic, and no usage text.

```vyto
import { cli } from "vyto/cli";
import { exit } from "vyto/os";

let app = cli("httpd", "a static file server");
app.intOption("port", "port to listen on").short("p").dfltInt(8080);
app.flag("quiet", "suppress the request log").short("q");
app.option("screen", "screen to open at start")
   .choices(["home", "settings", "photos"]).dflt("home");
app.option("tag", "extra tag, may repeat").many();
app.positional("root", "directory to serve").dflt(".");

if (!app.parse(args())) { print(app.report()); exit(app.exitCode()); }

app.getInt("port")        // 9000   from  --port 9000 | --port=9000 | -p 9000
app.on("quiet")           // true   from  --quiet | -q | --quiet=yes
app.get("screen")         // "settings"      --screen=bogus is an ERROR
app.all("tag")            // ["a", "b"]      --tag a --tag=b
app.get("root")           // "/srv"  first operand, or "." if absent
app.extra()               // ["-x"]  everything after a bare  --
```

`cli(name, about)` is shorthand for `new Cli(name, about)`, the way
`stringBuilder` stands in for `new StringBuilder`; `about` may be omitted for a
throwaway parser. Either spelling works — `new Cli` reads better where the
variable is called `cli`.

> **Do not name the variable `cli` if you import the function.** `let cli =
> cli("grid", "...")` shadows the function it is calling. The migrated apps in
> this repo use `new Cli(...)` and keep the variable name.

`parse` returns `false` for **both** a bad command line and `--help`;
`report()` and `exitCode()` tell them apart. Those four lines are the whole
idiom, and they are the only ones an app needs to get right.

## Modules

| Module | What it gives you |
|--------|-------------------|
| `vyto/cli` | `Cli` and its `cli()` shorthand, `Flag`, the `CLI_*` kind constants |
| `vyto/util/parse` | `parse_int` / `parse_float` / `parse_bool` — re-exported here, so the old `from "vyto/cli"` path still resolves |
| `vyto/cli/ask` | `ask` / `askSecret` / `confirm` / `choose`, and `fill(cli)` to prompt for whatever the command line left out. Opt-in: see below |

## Declaring

| Call | Accepts |
|---|---|
| `flag(name, about)` | `--name`, `-n`, `--name=true\|false\|yes\|no\|on\|off\|1\|0` |
| `option(name, about)` | `--name=V`, `--name V`, `-n=V`, `-n V` |
| `intOption` / `floatOption` | the same, with the value validated |
| `positional(name, about)` | one operand, filled in declaration order |
| `rest(name, about)` | the tail operand — takes everything left |
| `command(name, about)` | a subcommand; returns its own `Cli` |

Each returns its `Flag`, so refinements chain: `.short("p")`, `.meta("PATH")`,
`.need()`, `.many()`, `.choices([...])`, `.dflt()` / `.dfltInt()` /
`.dfltFloat()` / `.dfltBool()`.

## Why the program name is an argument

> **`new Cli("httpd", ...)` takes the program name because Vyto cannot discover
> it.** The `args()` builtin excludes `argv[0]` — the runtime skips it, and
> nothing else exposes it. Every other language's flag library reads it from the
> process, so this is the one place `vyto/cli` will surprise you.

## Values, and why nothing panics on user input

`to_int()` and `to_float()` **abort the process** on bad text — `"to_int: not
an integer"`, `"to_int: integer overflow"`, `"to_int: trailing characters"` —
which turns a typo'd port number into a crash. Every value therefore goes
through a validator first, and a rejected one becomes a message naming the flag:

```
httpd: --port: '80a0' is not an integer
try 'httpd --help' for usage
```

**The validators now live in `vyto/util/parse`.** They are the repo's first
non-panicking numeric parses and are useful with no `Cli` in sight — which is
exactly why they moved on 2026-09-16: twelve of this module's importers took
only the parsers and never touched `Cli`, and one of them was `vyto/validator`,
which has nothing to do with command lines.

```vyto
import { parse_int } from "vyto/util/parse";

let out: int[] = [0];
if (parse_int(text, out)) { use(out[0]); } else { complain(); }
```

A Vyto import is a re-export, so `from "vyto/cli"` still resolves all three and
no existing code broke. New code should take the direct path.

`parse_int` accepts `[+-]?digits` and **rejects int64 overflow**, which `to_int`
only discovers by aborting. It deliberately rejects surrounding whitespace that
`to_int` tolerates: `--port " 8080"` is a quoting mistake, not an intent. No
hex — `0x10` is rejected rather than silently read as `0`.

`parse_float` accepts `[+-]?digits[.digits][(e|E)[+-]?digits]` and nothing else:
no `inf`, no `nan`, no bare `.5` or trailing `1.`.

`parse_bool` accepts `true|false`, `1|0`, `yes|no`, `on|off`, case-insensitively.

> **Your own mistakes still panic.** `cli.get("serface")` on a name that was
> never declared, or `cli.getInt("screen")` on a string option, aborts with a
> `vyto/cli:` message. Those are source typos — exactly the bug class this
> module exists to kill — so returning `""` would reintroduce it one layer up.
> Declaring the same flag twice, mixing operands with subcommands, or declaring
> an operand after the variadic tail panic at declaration time for the same
> reason.

## Generated help

`--help` and `-h` are handled automatically unless you declare your own, and
they fire *during* the walk — before required-argument checks, so `prog --help`
works on a command that has required options. `prog <sub> --help` documents the
subcommand, because the subcommand parses its own tail.

```
datagrid — a spreadsheet-grade data table demo

usage: datagrid [options] [PATH]

Arguments:
  PATH                 CSV file to load

Options:
  -s, --surface        draw through the raw surface painter, not gfx
      --screen=<name>  screen to open at start
                         (one of: home, settings, photos) [default: home]
  -h, --help           show this help
```

The short-flag column is reserved even for flags that have no short form, so the
`--` names stay aligned. Labels wider than 28 columns put their text on the next
line rather than indenting every other row. No colour and no terminal-width
detection: the output is golden-tested, so it must be byte-stable, and there is
no `isatty` binding to detect a terminal with anyway.

## Asking for what was not typed

`vyto/cli/ask` turns a usage error into a short interview:

```vyto
import { Cli } from "vyto/cli";
import { fill } from "vyto/cli/ask";

let app = cli("deploy", "ship it");
app.option("env", "Environment").choices(["dev", "prod"]).need();
app.intOption("port", "Port to listen on").need();

if (!app.parse(args()) && !fill(app)) { print(app.report()); exit(app.exitCode()); }
```

```
$ deploy
Environment: prod          <- a choices() list becomes an arrow-key menu
Port to listen on: 9000
```

`fill()` asks only for values that are **required and missing**. An optional
flag with a default is not a question — interrogating someone about six things
they never needed to set is worse than the error it replaces. A `choices()` list
becomes a menu, an int option re-asks on bad input (three attempts), and a
required subcommand becomes a menu over the declared commands.

The four helpers stand alone, for prompting that has nothing to do with parsing:

```vyto
let name = ask("Project name", "hello");   // "" and Esc both give the default
let pw   = askSecret("Password");          // echoes nothing at all
let ok   = confirm("Overwrite?", false);   // y/yes/true/1/on and negatives
let i    = choose("Colour", ["red", "green"], 0);   // index, or -1 if cancelled
```

> **`choose()` returns -1 when cancelled, not the default.** Esc has to be
> distinguishable from deliberately picking the default, or a caller cannot tell
> a refusal from a choice and carries on as though the user agreed. `ask()` can
> safely return its default, because the caller supplied that string as the
> fallback — an index into someone else's list is not the same thing.

### It stays out of the way when nobody is there

Every function returns its fallback **immediately** when stdin is not a
terminal, and `fill()` returns false leaving the original error untouched. A CLI
is piped, cron'd and CI'd far more often than it is typed at, and a prompt that
blocks a pipeline forever is a worse failure than the error it replaced. That
path is golden-tested (`tests/fixtures/cli_ask.vt`); the interactive one cannot
be, since prompting exists to react to a human.

`fill()` also refuses errors that prompting cannot fix — an unknown flag, a bad
value that *was* given — because asking questions would bury the real mistake.
And it returns true when nothing was missing, so calling it unconditionally is
safe.

### Why a separate module

`vyto/cli` imports only `vyto/util/parse`, which is itself import-free and
shim-free, so it builds on a **freestanding** target — `src/vytopack` and eight
other importers get that for free. `vyto/cli/ask` imports `vyto/terminal`, whose
shim reaches termios, so folding it into `cli.vt` would cost every one of them
that guarantee. The dependency points one way: `cli.vt` has never heard of
`ask.vt`. Verified by building a plain `Cli` program with `--freestanding` and
checking it links zero terminal symbols.

## Recipes

**Environment variable as the default.** No API needed — `.dflt()` takes an
ordinary string, so the app keeps control of precedence:

```vyto
cli.positional("rom", "CHIP-8 ROM to load").dflt(getenvOr("CHIP8_ROM", ""));
cli.flag("debug", "trace each opcode").dfltBool(getenvOr("CHIP8_DEBUG", "") != "");
```

**Optional subcommand.** Subcommands are optional unless you call
`needCommand()`, so a bare invocation can still do something useful:

```vyto
if (cli.chosen() == "monitor") { watch(cli.sub().get("iface")); }
else { scanOnce(); }
```

**Values that start with a dash.**

> **A separated value may not look like a flag.** `--out --verbose` is an error
> (`--out needs a value`) rather than quietly setting `out` to `"--verbose"` —
> the mistake is far more common than the intent. Three things are still
> accepted: a bare `-` (the stdin convention), and a negative number for an
> `intOption`/`floatOption`, so `--rows -5` works. For anything else, use the
> inline form: `--out=-x`.

## Deliberately out of scope

Bundled short flags (`-abc`, `-ovalue`) · negatable `--no-foo` · `--version` ·
prefix matching (`--surf` → `--surface`) · did-you-mean suggestions · counting
flags (`-vvv`) · mutually-exclusive groups · config-file merging · nested
subcommands · multi-value options (`--pos X Y`).

None of these has a user in this repo, and each one costs a corner of the parser
that would then need its own goldens. A short token is exactly `-k`, `-k=V` or
`-k V`; anything else is an unknown flag, which keeps the fiddliest part of a
getopt out of the module entirely.

**Shell completions** are the interesting omission — `MODULES.md` scopes them
for this module. Nothing can consume them yet: there is no install step, no
`share/` directory, and a program runs as `./vytoc run app.vt -- args`, so the
name a completion script would attach to is `vytoc`, not the program. They are a
pure function of the declaration tree, which is why `Flag` keeps `choiceList`,
`about` and `kind` as readable fields — a later `vyto/cli/complete` is purely
additive and needs no change here.

`vytoc`'s own flag loop stays C. Making the compiler self-host its argument
parsing is not a goal of a stdlib module.

## Tests

`tests/fixtures/cli_parse.vt` covers every accepted syntax; `cli_errors.vt`
covers every rejection, including the int64 overflow boundary
(`9223372036854775807` accepted, `…808` rejected, and the same either side of
the negative bound); `cli_help.vt` pins the generated text byte-for-byte for a
root command and a subcommand; `cli_ask.vt` pins what `vyto/cli/ask` does under
a pipe — every fallback, and `fill()` leaving the error alone. `examples/72_cli.vt`
is the runnable tour.

`ask`'s interactive half is not golden-tested and cannot be: prompting exists to
react to a human. It was verified by driving the binary through a real pty —
the arrow-key menu, the retry on a bad number, Esc cancelling back to the
original error, and `askSecret` keeping its input out of the byte stream
entirely (grepped for, not eyeballed).

Fixtures feed hand-built `string[]`s rather than `args()`, both because the
example harness runs with no trailing arguments and because it makes the parser
testable at all.

> **Watch the evaluation order when writing fixtures.** Vyto evaluates binary
> operands right-to-left, so `print("ok=" + c.parse(av) + " help=" +
> c.helpWanted())` reads `helpWanted()` *before* `parse()` runs. Call `parse`
> on its own line and keep the result in a `let`.
