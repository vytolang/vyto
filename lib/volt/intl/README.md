# volt/intl — internationalization & localization

ICU-backed i18n for Volt: Unicode text operations, locale-aware number /
currency / date formatting, and an ICU-MessageFormat-subset catalog with CLDR
plurals. Volt strings stay UTF-8 everywhere; the native shim converts to/from
ICU's UTF-16 at the boundary, so you never touch a `UChar`.

```volt
import { locale, defaultLocale, NFC, DT_MEDIUM, DT_SHORT } from "volt/intl";
import { normalize, graphemes, toUpper, collator } from "volt/intl/unicode";
import { formatNumber, formatCurrency, formatPercent } from "volt/intl/number";
import { formatDate } from "volt/intl/datefmt";
import { loadCatalog, message, argSet } from "volt/intl/message";

let de = locale("de-DE");
formatNumber(1234567.5, de)               // "1.234.567,5"
formatCurrency(9.9, "EUR", de)            // "9,90 €"
formatPercent(0.25, locale("en"))         // "25%"
toUpper("straße", de)                     // "STRASSE"
graphemes("👨‍👩‍👧").len                     // 1
collator(locale("sv")).compare("z", "ö")  // -1  (Swedish sorts z before ö)

let d = date_from_unix(1700000000);
formatDate(d, locale("ja-JP"), DT_MEDIUM, DT_SHORT)   // "2023/11/14 22:13"

message("{n, plural, one{# item} other{# items}}", locale("en"),
        argSet().whole("n", 3))                        // "3 items"
```

## Modules

| Module | What it gives you |
|--------|-------------------|
| `volt/intl` | `Locale`, `locale(tag)`, `defaultLocale()`, and the shared enum consts (`NFC…`, `NUM_*`, `DT_*`, `PLURAL_*`). |
| `volt/intl/unicode` | `decode`/`encode`/`charCount`, `normalize`, `toUpper`/`toLower`/`foldCase`, `graphemes`/`words`/`lineBreaks`, `Collator`. |
| `volt/intl/number` | `formatNumber`/`formatInt`/`formatPercent`/`formatCurrency`, reusable `NumberFormat`. |
| `volt/intl/datefmt` | `formatDate`, reusable `DateFormat` (bridges `volt/util/date`'s `DateTime`). |
| `volt/intl/message` | `message`, `Catalog` (`loadCatalog`/`catalogFromJson`), `argSet()`, `PluralRules`. |

## MessageFormat subset

`volt/intl/message` parses a practical subset of ICU MessageFormat in Volt
(ICU's C `umsg_format` is varargs and can't be driven from a dynamic argument
set in portable C). The CLDR-hard parts — plural keyword selection and number
formatting — are still ICU. Supported:

- literal text, with `''` → `'` and `'…'` quoting of `{ } #`
- `{name}` — substitute an argument (numbers are locale-formatted)
- `{name, number}` — locale number format of a numeric argument
- `{name, plural, =N{…} one{…} few{…} other{…}}` — cardinal plural; `#` = the count
- `{name, selectordinal, …}` — ordinal plural
- `{name, select, male{…} female{…} other{…}}` — choose by a string argument

Arguments are named and supplied fluently: `argSet().whole("n", 3).text("who", "Ada")`
(`whole` for integer counts, `num` for decimals, `text` for strings — the free
helper is `argSet()` because `args` is a Volt builtin, the CLI argv).

## Provisioning ICU (hybrid)

**Default — system ICU.** `intl.vt` emits `#link "icuuc"` / `#link "icui18n"`,
so a normal `voltc build`/`voltc run` links the host's shared ICU. Install the
dev package if it's missing:

```sh
sudo apt install libicu-dev     # Debian/Ubuntu
sudo dnf install libicu-devel   # Fedora
```

Nothing is vendored in this mode — ICU's data lives in the system `libicudata`.

**Vendored — for `--bundle` or no-system-ICU targets.** Run the vendor script
once per target triple to populate `native/<triple>/`:

```sh
lib/volt/intl/native/build-icu.sh linux-x64        # or linux-arm64, windows-x64…
voltc build --bundle myapp.vt                       # static ICU baked into the exe
```

`build-icu.sh` builds ICU (shared + static) and installs headers to
`native/src/unicode/` and libraries + `.deps` sidecars to `native/<triple>/`.
After that, a default build still uses the system ICU; only `--bundle` picks up
the vendored static archives.

## Portability

- **Linux** — system `#link` works out of the box with `libicu-dev`.
- **macOS / Windows** — there is no public *system* ICU with headers, so those
  targets require the vendored path (`build-icu.sh <triple>` + `--bundle`). The
  `#link` pragmas are guarded `if "linux"` and are inert elsewhere; wire up the
  vendored binaries before building for those triples.

## Design notes

- The shim (`native/src/intl_shim.c`) is **pure C over ICU's C API** — no C++,
  so voltc's link line needs only `-licuuc -licui18n` (libstdc++ is pulled
  transitively by the ICU shared objects). See its header for the buffer
  protocol (grow-and-retry on the returned needed-length).
- Stateful ICU objects (collators, break iterators, formatters, plural rules)
  are opaque `rawptr` handles wrapped in Volt classes with `deinit` cleanup;
  ICU types never cross into Volt.
- `decode`/`encode` are pure Volt (UTF-8 is simple); everything requiring
  Unicode tables or CLDR data goes through ICU.
