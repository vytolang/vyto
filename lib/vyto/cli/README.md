# vyto/cli — flags, options, operands & subcommands

Pure Vyto with **no imports at all**. Help columns use the `pad_end` string
builtin and help text is assembled with `string[].join`, so nothing here pulls
in `vyto/util/fmt` (native-shim-backed) or `vyto/os`. It runs wherever the
runtime runs, including a freestanding target.

It exists because every app in this repo hand-rolled the same `args()` loop, and
they were all wrong in the same four ways: unknown flags silently ignored,
unknown values silently falling back to a default, mistyped numbers killing the
process through `to_int`'s panic, and no usage text.

```vyto
import { Cli } from "vyto/cli";
import { exit } from "vyto/os";

let cli = new Cli("httpd", "a static file server");
cli.intOption("port", "port to listen on").short("p").dfltInt(8080);
cli.flag("quiet", "suppress the request log").short("q");
cli.option("screen", "screen to open at start")
   .choices(["home", "settings", "photos"]).dflt("home");
cli.option("tag", "extra tag, may repeat").many();
cli.positional("root", "directory to serve").dflt(".");

if (!cli.parse(args())) { print(cli.report()); exit(cli.exitCode()); }

cli.getInt("port")        // 9000   from  --port 9000 | --port=9000 | -p 9000
cli.on("quiet")           // true   from  --quiet | -q | --quiet=yes
cli.get("screen")         // "settings"      --screen=bogus is an ERROR
cli.all("tag")            // ["a", "b"]      --tag a --tag=b
cli.get("root")           // "/srv"  first operand, or "." if absent
cli.extra()               // ["-x"]  everything after a bare  --
```

`parse` returns `false` for **both** a bad command line and `--help`;
`report()` and `exitCode()` tell them apart. Those four lines are the whole
idiom, and they are the only ones an app needs to get right.

## Modules

| Module | What it gives you |
|--------|-------------------|
| `vyto/cli` | `Cli`, `Flag`, the `CLI_*` kind constants, and the standalone `parse_int` / `parse_float` / `parse_bool` |

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

The validators are exported, because they are the repo's first non-panicking
numeric parses and are useful with no `Cli` in sight:

```vyto
let out: int[] = [0];
if (parse_int(text, out)) { use(out[0]); } else { complain(); }
```

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
> the mistake is far more common than the intent. A bare `-` is accepted (the
> stdin convention); for anything else, use the inline form: `--out=-x`.

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
root command and a subcommand. `examples/72_cli.vt` is the runnable tour.

Fixtures feed hand-built `string[]`s rather than `args()`, both because the
example harness runs with no trailing arguments and because it makes the parser
testable at all.

> **Watch the evaluation order when writing fixtures.** Vyto evaluates binary
> operands right-to-left, so `print("ok=" + c.parse(av) + " help=" +
> c.helpWanted())` reads `helpWanted()` *before* `parse()` runs. Call `parse`
> on its own line and keep the result in a `let`.
