# Strings & regular expressions in Vyto

> **Status: experimental.** The language and standard library are still moving.
> The operations below are stable enough to build on, but names and edge-case
> behaviour can still change.

Everything Vyto can do to text, in one place: the built-in `string` methods, the
helpers in `vyto/util/text`, the regex engine in `vyto/regex`, and the
Unicode-aware operations in `vyto/intl/unicode`.

## 1. What a string is

A Vyto `string` is an **immutable, reference-counted, UTF-8 byte sequence**. It
carries its own length, so it can hold NUL bytes — but the `cstr()` view handed
to C cannot represent them, which is why `str()` and `strbytes()` differ (§4).

**One rule causes most string bugs: everything below counts bytes, not
characters.**

```js
"héllo".len              // 6 — six bytes, five characters
"héllo".char_at(1)       // a broken half of "é", not "é"
"héllo"[1]               // 195 — the first byte of é's two-byte encoding
"héllo".reverse()        // mojibake: the bytes of é get swapped
"café".to_upper()        // "CAFé" — the ASCII rule does not know about é
```

That is a deliberate trade: byte operations are fast and allocation-free, and
most text handling (parsing, delimiters, protocol work) is genuinely byte
work. When you need characters, use `vyto/intl/unicode` (§7) — `charCount`,
`graphemes`, `toUpper(s, locale)` — or `vyto/regex`, which is Unicode-aware by
default (§6).

## 2. Built-in methods

No import needed. `s` is the receiver throughout.

### Inspecting

| Method | Returns | Notes |
|---|---|---|
| `s.len` | `int` | **Bytes**, not characters. A field, not a call. |
| `s.is_empty()` | `bool` | `s.len == 0` |
| `s[i]` | `int` | Byte value 0–255. **Panics** out of bounds. |
| `s.char_at(i)` | `string` | One-**byte** string. **Panics** out of bounds. |
| `s.contains(sub)` | `bool` | |
| `s.starts_with(p)` / `s.ends_with(p)` | `bool` | |
| `s.index_of(sub)` | `int` | First byte offset, or `-1`. Empty `sub` → `0`. |
| `s.last_index_of(sub)` | `int` | Last byte offset, or `-1`. |
| `s.count(sub)` | `int` | **Non-overlapping**: `"aaa".count("aa") == 1`. Empty `sub` → `0`. |

### Comparing and ordering

`==` and `!=` compare strings by value. **There is no `<` on strings** — `a < b`
is a type error, not a byte comparison. Ordering comes from `byte_comp`:

```js
byte_comp(a, alo, ahi, b, blo, bhi): int
```

It compares the byte ranges `a[alo, ahi)` and `b[blo, bhi)` lexicographically and
returns a negative number, `0`, or a positive one. A prefix sorts before the
string that extends it (`"fig"` before `"figs"`). Offsets are **clamped**, not
checked — out-of-range or inverted bounds give a defined answer rather than a
panic or an out-of-buffer read.

For whole strings, reach for the wrapper instead of spelling out the bounds:

```js
import { str_cmp } from "vyto/util/sort";

names.sort((a, b) => str_cmp(a, b));      // the way to sort a string[]
```

The range form earns its keep when the text you are comparing already lives
inside a larger buffer. Slicing it out first would allocate two strings *per
comparison* — and a sort makes `n log n` of them — whereas `byte_comp` reads the
bytes where they are:

```js
// names held as (offset, len) into one big response buffer
idx.sort((x, y) => byte_comp(raw, off[x], off[x] + len[x],
                             raw, off[y], off[y] + len[y]));
```

Case-insensitive comparison is a different question: `text.equals_ignore_case`
for ASCII, `unicode.foldCase` or a `Collator` for real text (§7, §9). `byte_comp`
is bytes, so `"Z"` sorts before `"a"` and `"é"` sorts by its UTF-8 encoding.

### Slicing and shaping

| Method | Returns | Notes |
|---|---|---|
| `s.slice(lo, hi)` | `string` | Half-open `[lo, hi)`. **Panics** if out of bounds — it does *not* clamp. For clamping use `text.left`/`text.right`. |
| `s.repeat(n)` | `string` | `n <= 0` → `""`. Panics if the result would overflow. |
| `s.reverse()` | `string` | **Reverses bytes.** Safe for ASCII, corrupts anything else. |
| `s.pad_start(w, ch)` / `s.pad_end(w, ch)` | `string` | Pads to exactly `w` bytes by cycling `ch`. Returns `s` unchanged if `s.len >= w` or `ch` is empty — it never truncates. |

### Case and whitespace — ASCII only

| Method | Notes |
|---|---|
| `s.to_upper()` / `s.to_lower()` | Maps `a–z`/`A–Z` only. Non-ASCII is left alone. |
| `s.trim()` / `s.trim_start()` / `s.trim_end()` | Strips ` `, `\t`, `\n`, `\r`, `\f`, `\v`. Not Unicode whitespace. |

For locale-correct case use `vyto/intl/unicode`'s `toUpper(s, loc)`,
`toLower(s, loc)` and `foldCase(s)`.

### Splitting

| Method | Returns | Notes |
|---|---|---|
| `s.split(sep)` | `string[]` | Empty `sep` → `[s]`, one element. Keeps empty pieces: `"a,b,".split(",")` → `["a", "b", ""]`. Never returns an empty array. |
| `s.lines()` | `string[]` | Splits on `\n` and strips one trailing `\r`, so CRLF and LF both work. **No trailing empty element** — `"a\nb\n".lines()` is `["a", "b"]`, unlike `split`. |

The `split`/`lines` difference on a trailing separator is the one to remember:
`split` reports it, `lines` does not.

### Replacing

| Method | Notes |
|---|---|
| `s.replace(old, neu)` | Replaces **all** non-overlapping occurrences. Empty `old` returns `s` unchanged. |

For first-only, use `text.replace_first` (§5). For pattern replacement, `vyto/regex` (§6).

### Numbers

| Method | On bad input |
|---|---|
| `s.to_int()` | **Panics.** Allows surrounding whitespace and a leading `+`/`-`; panics on trailing characters and on overflow. |
| `s.to_float()` | **Panics** on anything unparseable. |
| `s.to_float_at(lo, hi)` | Parses the slice `[lo, hi)` without allocating. |

Those panic because a malformed number in a literal is a bug. When the input is
*data* — a config file, a CLI flag, a form field — use the non-panicking
versions in `vyto/cli`, which follow the library-wide "panic hard, sentinel
soft" rule:

```js
import { parse_int, parse_float, parse_bool } from "vyto/cli";

let out: int[] = [0];                          // note: one element, not []
if (parse_int(userInput, out)) { use(out[0]); } else { complain(); }
```

Two things to get right here:

- **The out-array must already have an element.** These write `out[0]`, so
  `[0]` is the correct initialiser; `[]` panics with an out-of-bounds index.
- **Don't read `out[0]` in the same expression that calls the parser.** Vyto
  evaluates binary operands **right to left**, so
  `print(parse_int(s, out) + " " + out[0])` reads `out[0]` *before* the call
  fills it. Sequence them, as above.

`parse_int` also rejects int64 overflow (which `to_int` discovers by aborting),
rejects surrounding whitespace, and rejects hex rather than silently reading
`0x10` as `0`.

## 3. Concatenation and building

### Template literals

A **backtick** string interpolates: `{...}` inside one holds any expression, and
its value is converted to string exactly as `+` would convert it.

```js
let msg = `Hello {name}, from {city}`;
let dim = `{w}x{h} at {fps} fps`;
let sum = `total {a + b}, {xs.len} items`;
```

Prefer it over `+` for anything with more than two parts. A template lowers to a
**single allocation** no matter how many parts it has, while a chain of `+`
allocates once per `+` and recopies the whole prefix each time — measured 2.9x
on a six-part string.

Four things to know:

- **A `"..."` string never interpolates.** Braces in a double-quoted string mean
  nothing special, which is what keeps JSON, regex quantifiers like `\d{3}`, and
  `vyto/intl`'s MessageFormat working unchanged.
- **A backtick string is raw.** Backslash is an ordinary byte — `` `C:\Users` ``
  and `` `\d+\.\d+` `` need no doubling — and there are no `\n`/`\t`/`\xHH`
  escapes. Use `"..."` when you want those, or a real newline:
  ```js
  let sql = `SELECT id, name
  FROM users
  WHERE age > {min}`;          // the newlines are literal
  ```
  A consequence: a literal backtick cannot appear inside one. Use `"..."`, which
  can hold a backtick, and concatenate.
- **`{{` and `}}` are the brace escapes**, and both are required — a bare `}` is
  an error, so the two stay symmetric and a typo is caught rather than accepted.
  `` `{{"id": {id}}}` `` produces `{"id": 42}`.
- **Holes evaluate strictly left to right.** This is a *stronger* guarantee than
  `+`, whose operands evaluate right to left (§2, "Numbers"), so
  `` `{a()}{b()}` `` calls `a` then `b` while `a() + b()` does not promise that.

A hole accepts what `+` accepts: strings, any numeric type, and `bool`. A
`cstring` needs an explicit `str(p)`, as everywhere else.

### `+` and `StringBuilder`

`+` concatenates, and mixes types directly — `"n=" + 42 + " ok=" + true` works
with no formatting call. Each `+` allocates, so a loop that concatenates is
quadratic. For that, use `StringBuilder` from `vyto/util/text`:

```js
import { stringBuilder } from "vyto/util/text";

let sb = stringBuilder(4096);                 // initial capacity in bytes
for (let i in 0..200000) {
    sb.append("item").appendInt(i).append(",");
}
let s = sb.toString();
```

`append`, `appendInt`, `appendFloat` and `appendByte` all return the builder, so
they chain. `len()`, `clear()`, `toString()` and `cstr()` round it out.

### Choosing between a template and `fmt()`

`vyto/util/fmt` walks its format string at runtime; a template is resolved at
compile time. So when the format is **known where you write it**, reach for a
template — measured 2.3x faster than the equivalent `fmt()` call, and a bad
argument becomes a compile error instead of a runtime panic.

The catch: a hole stringifies a value exactly one way, the way `+` does. There
is no width, precision, alignment or radix in the hole itself, so this is *not*
a mechanical rewrite of existing `fmt()` calls — `fmt("%.2f", [ff(x)])` gives
`3.14` where `` `{x}` `` gives `3.14159`.

Reach for `fmt`'s scalar helpers inside the hole instead, and you get both:

```js
import { fixed, hex, pad, commas } from "vyto/util/fmt";

print(`{name} scored {fixed(x, 2)} in hex {hex(n)}, padded [{pad(name, 8)}]`);
print(`{commas(total)} rows`);
```

That produces byte-identical output to the `fmt()` spelling and still measured
2x faster, because the literal text never gets re-scanned.

**Keep `fmt()` for a format string that is data** — an i18n catalog entry, a
config value, anything read at runtime. A template cannot help there: it is
parsed by the compiler, so a string that only exists at runtime interpolates
nothing. That is also what `vyto/intl`'s MessageFormat is for.

## 4. Bytes ⟷ strings

| Call | Direction | Notes |
|---|---|---|
| `bytes(n)` | — | A zeroed `byte[]` of length `n`. |
| `str(cstr)` | C → Vyto | `strlen` scan; **stops at the first NUL**. |
| `strbytes(buf, n)` | C → Vyto | Takes exactly `n` bytes from a `byte[]`. **Keeps embedded NULs.** Use this when the length is known. |
| `s.cstr()` | Vyto → C | Borrowed `const char*`. Pair it with `s.len` when the callee is length-aware. |
| `chr(code)` | — | One-byte string from 0–255. `chr(0)` returns `""`. From `vyto/util/text`. |
| `ord(s, i)` | — | Byte value at `i`, same as `s[i]`. From `vyto/util/text`. |

For full Unicode scalars rather than single bytes, use `vyto/intl/unicode`'s
`encode(cps)` and `decode(s)`.

## 5. `vyto/util/text` helpers

Pure Vyto over the builtins, plus the native `StringBuilder`.

| Function | What it does |
|---|---|
| `strip_prefix(s, p)` / `strip_suffix(s, p)` | Drop `p` if present, else return `s` unchanged. |
| `replace_first(s, old, neu)` | Replace only the first occurrence (contrast `s.replace`, which replaces all). |
| `capitalize(s)` | Uppercase the first byte, leave the rest. |
| `left(s, n)` / `right(s, n)` | First / last `n` bytes, **clamped** to `[0, len]` — the non-panicking `slice`. |
| `center(s, width, fill)` | Pad both sides; extra padding goes right. |
| `is_blank(s)` | Empty or whitespace-only. |
| `equals_ignore_case(a, b)` | ASCII case-insensitive equality. |
| `index_of_from(s, sub, at)` | First index at or after `at`, or `-1`. (`from` is a keyword.) **Copies the tail** — it is `s.slice(at, s.len).index_of(sub)`, so on a large `s` this allocates the remainder of the string on every call. Scan bytes yourself over big buffers. |
| `chr(code)` / `ord(s, i)` | See §4. |
| `stringBuilder(cap)` | See §3. |

## 6. Regular expressions — `vyto/regex`

### The engine

`vyto/regex` binds **PCRE2 10.45**, vendored in-tree under
`lib/vyto/regex/native/src/pcre2/` and compiled from source on every build.
Nothing to install, on any platform, Windows included.

- **JIT-compiled** via sljit. Where executable memory is unavailable — a
  hardened macOS runtime, a W^X kiosk, an architecture with no sljit backend —
  it falls back to PCRE2's interpreter with identical results. `VYTO_REGEX_JIT=0`
  forces the interpreter.
- **Unicode by default.** `\w`, `\b`, `\d` follow Unicode rules and `\p{...}`
  works, because Vyto strings are UTF-8. `RX_BYTES` opts out for binary data.
- **Bounded backtracking.** Every pattern carries a match limit, so a hostile
  regex returns an error in milliseconds instead of hanging the process.

Full syntax is Perl-compatible: alternation, greedy/lazy/possessive quantifiers,
character classes, backreferences, lookahead and lookbehind, named groups,
atomic groups, conditionals, inline flags, `\p{Script}`, `\X`, and the rest.
PCRE2's own [pattern documentation](https://www.pcre.org/current/doc/html/pcre2pattern.html)
is the reference.

### Two ways in

**Hold a `Regex`** when the pattern is used more than once — compiling is the
expensive half, matching is cheap:

```js
import { Regex } from "vyto/regex";

let re = new Regex("(?<user>\\w+)@(?<host>[\\w.]+)", 0);
if (!re.isValid()) { return; }                 // compiling is soft — check it
let m = re.find("mail bob@example.com now");
m.text();                                      // "bob@example.com"
m.named("user");                               // "bob"
m.start();                                     // 5
```

**Use the `rx_*` functions** when you don't want to hold anything. They compile
through a 64-entry process-local cache keyed on (pattern, flags), so a pattern
in a loop is still compiled once:

```js
import { rx_find, rx_test, rx_replace_all, rx_split } from "vyto/regex";

rx_test("^\\d+$", "4711");                     // true
rx_find("shard (\\d+)", log).group(1);         // "3"
rx_replace_all("\\s+", "a  b   c", " ");       // "a b c"
rx_split(",\\s*", "a, b,c").len;               // 3
```

### `Regex`

| Method | Notes |
|---|---|
| `new Regex(pattern, flags)` | **Never panics.** A bad pattern leaves an invalid `Regex`. |
| `isValid()` / `error()` / `errorOffset()` | The sentinel. `errorOffset()` is `-1` on success. |
| `groupCount()` / `names()` | Capturing groups; names sorted by PCRE2. |
| `test(s)` | `bool`. |
| `find(s)` / `findFrom(s, start)` | A `Match`, matched or not. |
| `findAll(s)` | `Match[]`, non-overlapping, left to right. |
| `replace(s, repl)` / `replaceAll(s, repl)` | Replacement syntax is `$1`, `${name}`, `$$` — **never `\1`**. |
| `replaceAllExt(s, repl, sflags)` | Adds `RX_SUB_EXTENDED` / `RX_SUB_UNSET_EMPTY`. |
| `split(s)` / `splitN(s, limit)` | `limit <= 0` means unlimited; the last piece keeps the remainder. |
| `setLimits(match, depth, heapKB)` | `0` leaves one alone. |
| `jitEnabled()` | Whether *this* pattern got JIT'd. |

### `Match`

| Method | Notes |
|---|---|
| `matched()` / `errorCode()` / `error()` | |
| `text()` / `start()` / `end()` | Group 0, and its byte offsets. |
| `group(i)` / `groupStart(i)` / `groupEnd(i)` / `hasGroup(i)` | Unset groups give `""` and `-1`. Out-of-range is safe, not a panic. |
| `groups()` | `string[]`, index 0 is the whole match. |
| `named(n)` / `hasNamed(n)` / `namedGroups()` | `namedGroups()` returns a `Map<string, string>` of the groups that participated. |
| `groupCount()` | Groups in this match, excluding group 0. |

### The convenience layer

The tables above are the engine. These are the shortcuts, and they are where
most day-to-day work happens. **Every `rx_*` function takes a trailing `flags`
that defaults to `0`.**

| Call | Gives you |
|---|---|
| `rx_find_all(pat, s)` / `re.findAllText(s)` | every match's text |
| `rx_find_all_group(pat, s, i)` / `re.findAllGroup(s, i)` | one capture across every match |
| `rx_count(pat, s)` / `re.count(s)` | how many, counted in C with no allocation |
| `rx_grep(pat, s)` / `rx_grep_v(pat, s)` | the lines that do / don't match |
| `rx_find_or(pat, s, dflt)` / `re.findOr(s, dflt)` | first match, or a default |
| `rx_group(pat, s, i)` / `rx_named(pat, s, name)` | one capture from the first match |
| `rx_quote(s)` | escape a literal for use in a pattern |
| `rx_full_match(pat, s)` / `re.fullMatch(s)` | must match the whole subject |
| `rx_index_of` / `rx_last_index_of` | byte offset of the first / last match |
| `rx_trim` / `rx_strip_prefix` / `rx_strip_suffix` | pattern twins of the `text` helpers |
| `rx_replace_first(pat, s, repl)` | first only — twin of `text.replace_first` |
| `re.replaceFn(s, f)` / `re.replaceN(s, repl, n)` / `re.each(s, f)` | callback and bounded editing |
| `re.partition(s)` / `re.splitKeep(s)` | `[before, match, after]` / split keeping delimiters |
| `re.expand(m, template)` | apply `$1`/`${name}` to a `Match` you already have |
| `rx_test_any(pats, s)` | true if any pattern matches |

Two of these are worth calling out because getting them wrong is silent.

**`rx_quote` when the pattern came from a user.** Without it, `a.*b` typed into
a search box is a wildcard and `(a+)+$` is a denial of service:

```js
rx_count(rx_quote(userInput), haystack);      // literal, and safe
```

**`fullMatch`, not `^…$`.** `$` also matches before a final newline, so
`rx_test("^abc$", "abc\n")` is `true` — almost never what was meant.
`rx_full_match("abc", "abc\n")` is `false`. It is also not the same as checking
`find`'s offsets: matching is leftmost-first, so `a|ab` on `"ab"` finds `"a"`
and an offset test would wrongly report no full match.

`replaceFn`'s callback must be assigned to a typed target before the call and
cannot capture `this` — both are v0.1 language limits:

```js
let redact: fn(Match): string = (m) => m.named("user") + "@***";
mail.replaceFn(log, redact);
```

### Validators

| Call | |
|---|---|
| `rx_is_email_loose(s)` / `RX_P_EMAIL_LOOSE` | a shape check, nothing more |
| `rx_is_ipv4(s)` / `RX_P_IPV4` | dotted quad, range-checked, rejects leading zeros |

**Deliberately only two.** UUID, URL, ISO-date and numbers already have
hand-written validators that beat a regex on accuracy and error reporting — use
`uuid_is_valid`, `url_is_valid`, `date_parse` + `date_is_valid`, and
`cli.parse_int` / `parse_float` instead.

`rx_is_email_loose` accepts `first.last@sub.example.co.uk`, `user+tag@x.io` and
internationalised domains; rejects a missing `@`, whitespace, a domain with no
dot, a trailing dot and consecutive dots in the domain. It does not accept
quoted local parts or IP-literal domains, which are legal RFC 5322 and
vanishingly rare. **It is not an RFC 5322 validator and not a deliverability
check** — the only way to know an address works is to send to it. Use it to
catch a typo in a form field, not to make a decision.

### Flags

Compile flags, OR them together: `RX_CASELESS`, `RX_MULTILINE`, `RX_DOTALL`,
`RX_EXTENDED`, `RX_UNGREEDY`, `RX_ANCHORED`, `RX_DOLLAR_ENDONLY`,
`RX_FIRSTLINE`, `RX_NO_AUTO_CAPTURE`, `RX_BYTES`.

Substitution flags for `replaceAllExt`: `RX_SUB_EXTENDED`, `RX_SUB_UNSET_EMPTY`.

### The error model

The library-wide rule is **panic hard, sentinel soft**, and for regex it lands
in a place worth stating outright:

| Situation | Behaviour |
|---|---|
| Bad pattern | **Soft.** A regex is usually data — a config value, a `--filter`, a search box. Check `isValid()`. |
| Matching with an invalid `Regex` | **Panics.** Ignoring the sentinel is the bug; answering "no match" would be a wrong answer rather than a loud one. |
| No match | **Soft.** `errorCode() == RX_NO_MATCH`. |
| Invalid UTF-8 subject | **Soft.** `RX_ERR_UTF`. Use `RX_BYTES` for binary. |
| Match limit exceeded | **Soft in `find`** (`RX_ERR_LIMIT`), **panics in `test`, `findAll`, `replace*`, `split*`** — those return plain values with nowhere to put an error, and a truncated result is indistinguishable from a real one. |

Codes are `RX_OK`, `RX_NO_MATCH`, `RX_ERR_LIMIT`, `RX_ERR_UTF`,
`RX_ERR_INTERNAL`. PCRE2's own numbering never reaches Vyto, so a PCRE2 upgrade
cannot change a value your code branches on.

### Limits and ReDoS

Every `Regex` gets a backtracking budget: `RX_DEFAULT_MATCH_LIMIT` (1 000 000),
`RX_DEFAULT_DEPTH_LIMIT` (10 000), `RX_DEFAULT_HEAP_KB` (16 384). The classic
exponential blowup fails fast instead of hanging:

```js
let evil = new Regex("(a+)+$", 0);
evil.find("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa!").errorCode();  // RX_ERR_LIMIT
```

`setLimits` adjusts them per pattern. One caveat: **`depth_limit` has no effect
while the JIT is active**, because the JIT uses its own stack rather than the
interpreter's recursion. `match_limit` binds either way, which is why the
guarantee above holds with JIT on and off.

### Performance notes

- Hoist the `Regex` out of your loop, or use `rx_*` — compiling dominates.
- Offsets are byte offsets. Group text is sliced from the Vyto subject, never
  copied back from C, so extraction is one `memcpy` with no `strlen`.
- A `Regex` allocates its match data once, not per call, so `findAll` over
  10 000 matches allocates nothing on the C side. The cost: **a `Regex` is not
  reentrant** — don't interleave two scans over the same object.
- UTF-8 validation is cached per scan, so `findAll` over a large subject is
  linear rather than quadratic.
- `rx_cache_count()` and `rx_cache_clear()` expose the pattern cache;
  `rx_jit_available()` reports whether this build has JIT at all.

### Byte-safety

Unlike the `cstr()`-based paths elsewhere, `vyto/regex` passes explicit lengths
throughout, so **patterns and subjects containing NUL bytes work correctly**. It
does not inherit the "truncates at the first NUL" caveat.

More detail — vendoring rationale, build cost, refresh procedure — is in
[`lib/vyto/regex/README.md`](../lib/vyto/regex/README.md). The runnable tour is
`examples/83_regex.vt`.

## 7. Unicode-aware text — `vyto/intl/unicode`

When bytes are not the right unit. Backed by ICU, which — unlike `vyto/regex` —
needs a one-time setup; see **Native dependencies** in the
[root README](../README.md) and [`lib/vyto/intl/README.md`](../lib/vyto/intl/README.md).

| Function | What it does |
|---|---|
| `decode(s)` / `encode(cps)` | `string` ⟷ `int[]` of code points. |
| `charCount(s)` | Code points, not bytes. |
| `graphemes(s)` | User-perceived characters — the right unit for cursors and truncation. |
| `words(s)` / `wordsIn(s, loc)` | Locale-aware word segmentation. |
| `lineBreaks(s)` | Legal break offsets for wrapping. |
| `normalize(s, form)` | NFC / NFD / NFKC / NFKD. |
| `toUpper(s, loc)` / `toLower(s, loc)` | Locale-correct case (Turkish dotted İ, and so on). |
| `foldCase(s)` | Case-insensitive comparison key. |
| `collator(loc)` | Locale-correct sorting: `compare(a, b)`, `sortKey(s)`. |

Use `foldCase` or a `Collator` for case-insensitive comparison of real text;
`equals_ignore_case` is ASCII-only.

## 8. Escaping, encoding, formatting

| Need | Use |
|---|---|
| HTML text / attribute | `html_escape`, `html_escape_attr`, `html_unescape` — `vyto/util/html` |
| URL components | `url_encode`, `url_encode_path`, `url_encode_form`, `url_decode`, `url_decode_form` — `vyto/util/url` |
| Base64 / quoted-printable | `base64_encode`, `base64url_encode`, `base64_decode`, `qp_encode`, `qp_decode` — `vyto/util/mime` |
| JSON string quoting | `json_quote` — `vyto/util/json` |
| printf-style formatting | `fmt(f, args)` with `fi`/`fl`/`ff`/`fs`/`fb` — `vyto/util/fmt` |
| Numbers as text | `fixed(x, prec)`, `hex`, `oct`, `bin`, `commas` — `vyto/util/fmt` |

## 9. Which one do I reach for?

| Task | Use |
|---|---|
| Fixed delimiter | `s.split(sep)` — no regex needed, and faster |
| Pattern delimiter | `rx_split(pat, s)` |
| Literal substring present? | `s.contains(sub)` |
| Pattern present? | `rx_test(pat, s)` |
| Replace a literal | `s.replace(old, neu)` / `text.replace_first` |
| Replace a pattern | `rx_replace_all(pat, s, repl)` |
| Iterate lines | `s.lines()` |
| Keep only the lines that match | `rx_grep(pat, s)` |
| Count occurrences of a literal | `s.count(sub)` |
| Count matches of a pattern | `rx_count(pat, s)` — allocates nothing |
| All matches as strings | `rx_find_all(pat, s)` |
| One capture from every match | `rx_find_all_group(pat, s, i)` |
| Search for text a user typed | `rx_quote` it first — always |
| "Is this string exactly …?" | `rx_full_match`, **never** `^…$` |
| Split into before/match/after | `re.partition(s)` |
| Count characters for display | `unicode.graphemes(s).len` — **not** `s.len` |
| Truncate for display | slice on a `graphemes` boundary, never on a byte index |
| Order two strings | `sort.str_cmp(a, b)` — there is no `<` on strings |
| Sort a `string[]` | `xs.sort((a, b) => str_cmp(a, b))` |
| Order two ranges inside a buffer | `byte_comp(buf, alo, ahi, buf, blo, bhi)` — no allocation |
| Case-insensitive compare, ASCII | `text.equals_ignore_case` |
| Case-insensitive compare, real text | `unicode.foldCase` or a `Collator` |
| Parse a number you trust | `s.to_int()` |
| Parse a number from a user | `cli.parse_int(s, out)` |
| Check *and* convert user input | `vyto/validator` — `v.check(s).asInt().between(1, 99)` |
| "Is this an email / URL / date?" | `vyto/validator` — and see its notes on why `url()` and `isoDate()` are stricter than the parsers |
| Build a string in a loop | `text.stringBuilder(cap)` |
