# vyto/validator — validation for input and literals

Checking the text that arrives from outside your program — CLI flags, query
strings, form fields, INI/TOML settings, environment variables — before you
trust it.

```vyto
import { validation } from "vyto/validator";

let v = validation();
v.check(form.get("name"), field: "name").required().trim().betweenChars(2, 40);
let age  = v.check(form.get("age"),  field: "age").asInt().between(13, 120).value();
let port = v.check(cfg.get("port"),  field: "port").asInt().between(1, 65535).valueOr(8080);

if (!v.ok()) {
    for (let m in v.messages()) { print(m); }   // "name: is required"
}
```

## Scope

**Values and literals. Not structs, classes, objects or nested trees.**

That is deliberate, not a gap. The compiler already checks your types — what
needs checking is what crosses the boundary, and at the boundary the data is
flat text. Nested/tree validation may come later; the rules are built as
standalone objects so it can be added without rewriting them.

## Modules

| Module | What it gives you |
|---|---|
| `validator.vt` | `Validation`, the check chain, all the regex-free rules, coercion |
| `pattern.vt` | `email()`, `ipv4()`, `pattern()`, `matches()`, `notMatches()` |

## Coercion changes the type of the chain

```vyto
v.check("42").asFloat().min(10.0).max(100.0);
```

`asFloat()` converts, records an error if it cannot, and hands the rest of the
chain to a float-typed check. So `.min()` takes a float, and `.email()` is not
in scope at all — **misuse is a compile error, not a runtime one**.

`asInt()`, `asFloat()` and `asBool()` all work this way, all use the
non-panicking parsers from `vyto/cli`, and all leave the value readable with
`.value()` or `.valueOr(default)`. Nothing downstream re-parses.

## One failing value gives one error

A failing rule stops the chain:

```vyto
v.check("", field: "email").required().minChars(6);   // 1 error, not 2
```

`required` fails, and `minChars` becomes a no-op. A form showing three messages
under one empty box is worse than showing one. It is also what stops a failed
`asInt()` from running `between()` against a garbage value.

## required vs optional

| | Empty value | Rest of the chain | Recorded? |
|---|---|---|---|
| `required()` | fails | skipped | yes |
| `optional()` | allowed | skipped | **no** |

So an absent optional setting is not an error, and the rules behind it do not
fire on emptiness:

```vyto
let workers = v.check("", field: "workers").optional().asInt().positive().valueOr(4);
// workers == 4, and v.ok() is still true
```

`absent()` tells the two apart after the fact.

## Characters, not bytes

`minChars`/`maxChars`/`exactChars`/`betweenChars` count **Unicode code points**.
`"José"` is 5 bytes and 4 characters; a byte-based limit would reject perfectly
reasonable names and fail on the first CJK input.

```vyto
v_str("José").maxChars(4).ok();   // true
v_str("José").maxBytes(4).ok();   // false
```

`minBytes`/`maxBytes` exist for when the budget really is bytes — a database
column, a protocol field, a fixed buffer.

Counting needs no ICU. Note that code points are not graphemes: `"é"` written as
`e` + U+0301 counts as 2. For grapheme-exact counting use `unicode.graphemes`
from `vyto/intl`, at the cost of an ICU dependency.

## Rules

**Any check** — `required` · `optional` · `custom(Rule)` · `customFn(f, code, msg)`

**Strings** — `notBlank` · `trim` · `minChars` `maxChars` `exactChars`
`betweenChars` · `minBytes` `maxBytes` · `oneOf` · `startsWith` `endsWith`
`contains` · `url` · `uuid` · `isoDate` · `asInt` `asFloat` `asBool` · `value`
`valueOr`

**Integers** — `min` `max` `between` `positive` `nonNegative` `oneOfInt`

**Floats** — `min` `max` `between` `positive` `finite`

**Booleans** — `isTrue` `isFalse`

**From `pattern.vt`** — `email` · `ipv4` · `pattern` (anchored) · `matches`
(unanchored) · `notMatches`

Two of these are stricter than the stdlib function they might have used, on
purpose:

- **`url()` requires an *absolute* URL** — scheme and host both present.
  `url_is_valid` parses RFC 3986 *references*, so it accepts `"abc"` as a valid
  relative path. When a form asks for a URL it means an absolute one.
- **`isoDate()` range-checks the real calendar.** `date_parse` is
  strptime-backed and lenient: it reads `2026-02-30` as February 30th without
  complaint and `28-07-2026` as the year 28. Lenient is right for parsing input
  you have decided to trust; it is wrong for deciding whether to trust it.

## Why the pattern rules live apart

Importing `vyto/regex` compiles its 30 vendored PCRE2 translation units into
your program. Measured on a release build:

| Program | Binary |
|---|---|
| core validator only | **23 KB** |
| core + `validator/pattern` | **664 KB** |

Vyto compiles only what you import, so keeping `email`/`ipv4`/`pattern` in a
sibling module makes that cost opt-in. A form that needs "required, 2 to 40
characters" should not carry a regex engine.

The consequence is that they are free functions taking and returning a
`StringCheck`, not methods — Vyto has no extension methods, and putting them on
`StringCheck` would force the core to import `vyto/regex`. They still chain,
just inside-out:

```vyto
email(v.check(s, field: "email").required().maxChars(254));
```

`url`, `uuid` and `isoDate` need no regex and stay in the core.

## Errors

Bad input is **data**, never a panic — the library-wide "panic hard, sentinel
soft" rule. A `ValidationError` carries `field`, `code`, `message` and the
`value` as it stood when the rule ran.

The **codes are the stable API**; the English messages are a convenience layer,
so a translated table can be layered behind them later without breaking callers
who branch on the code.

```
V_REQUIRED V_BLANK V_TOO_SHORT V_TOO_LONG V_NOT_IN_SET V_NO_MATCH
V_NOT_INT V_NOT_FLOAT V_NOT_BOOL V_TOO_SMALL V_TOO_LARGE
V_NOT_EMAIL V_NOT_URL V_NOT_UUID V_NOT_IPV4 V_NOT_DATE
V_NOT_TRUE V_NOT_FALSE V_CUSTOM
```

`describe()` renders `"field: message"`, or just the message when the check was
unlabelled.

## Custom rules

A `Rule` subclass, because a closure in v0.1 cannot capture `this`, cannot be
assigned to and cannot nest — so a rule carrying configuration is not
expressible as one:

```vyto
class MaxWords extends Rule {
    limit: int;
    fn init(n: int) { this.limit = n; }
    override fn test(s: string): bool { return s.split(" ").len <= this.limit; }
    override fn code(): int { return V_CUSTOM; }
    override fn message(): string { return "must be at most " + this.limit + " words"; }
}

let r: Rule = new MaxWords(3);
v.check(bio, field: "bio").custom(r);
```

`customFn(f, code, msg)` covers the stateless cases. The closure must be
assigned to a typed target first — an inline arrow cannot be inferred at the
call site.

## Standalone checks

No accumulator when there is only one thing to check:

```vyto
v_str("hello").minChars(3).ok();
v_int(42).between(1, 100).ok();
v_float(0.5).positive().ok();
v_bool(flag).isTrue().ok();
```

Read the outcome with `.ok()`, `.message()` or `.error()`.

## Dependencies

| Import | Native cost |
|---|---|
| `vyto/cli` (the parsers) | none — pure Vyto |
| `vyto/util/url` | 4 small shims, shared with every other `vyto/util` module |
| `vyto/regex` (via `pattern.vt` only) | 30 PCRE2 translation units |

`uuid()` is implemented here rather than calling `uuid_is_valid`, because
`vyto/util/uuid` carries a CSPRNG shim — and a `bcrypt` link on Windows — that a
pure format check has no use for. Use `vyto/util/uuid` when you need to parse,
generate or inspect a UUID's version.

## Design notes

- Chains are `builder` methods, which return the **receiver's static type**
  (`src/check.c:1818`), so rules declared once on the base class chain correctly
  from every subclass without duplication.
- The coercions are *not* builders — a builder returns the receiver and cannot
  declare a return type. They are ordinary methods returning the next check.
- `failed` and `skipped` are separate states. Both stop the chain; only one is
  an error. That is what lets an optional field be absent without failing while
  still suppressing the rules behind it.
- `fail()` is idempotent, which is what makes short-circuiting produce exactly
  one error rather than relying on every caller to check.
- Rules are standalone objects with a virtual `test()`, so a future nested
  schema can compose the same rule objects unchanged.

## Tests

- `tests/fixtures/validator.vt` — exhaustive, including every soft-failure path:
  the coercion chain, short-circuiting, required vs optional, characters vs
  bytes, every format predicate accept *and* reject, leap years, both custom
  rule forms, the accumulator, and `valueOr` on passed/failed/absent checks.
- A second suite check builds a **core-only** program into an isolated object
  cache and asserts no PCRE2 object is produced — the split is a test, not a
  promise.
- `examples/85_validator.vt` is the runnable tour.
