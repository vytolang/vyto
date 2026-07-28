# vyto/util — small utilities and the data formats

Two kinds of thing live here. The older half is cross-cutting primitives —
`text`, `fmt`, `sort`, `array`, `time`, `date`, `json`, `helper`. The newer half
is the data formats: `csv`, `xml`, `toml`, `ini`, `markdown`, `html`, `url`,
`mime`, plus `log` and `uuid`.

Everything in the second group is pure Vyto with **one exception**: `uuid`
carries a native shim, because a language with no OS entropy call cannot produce
unpredictable bytes for itself.

```vyto
import { csv_parse }      from "vyto/util/csv";
import { xml_parse }      from "vyto/util/xml";
import { toml_parse }     from "vyto/util/toml";
import { md_render }      from "vyto/util/markdown";
import { html_escape }    from "vyto/util/html";
import { url_parse, url } from "vyto/util/url";
import { base64_encode }  from "vyto/util/mime";
import { logger }         from "vyto/util/log";
import { uuid4 }          from "vyto/util/uuid";

csv_parse("a,\"x,y\"\n")                  // [["a", "x,y"]]  — a real RFC 4180 reader
xml_parse("<a x='1'/>").root().attr("x")  // "1"
toml_parse("port = 8080").get("port")     // a JsonValue holding 8080
md_render("# Hi *there*")                 // "<h1>Hi <em>there</em></h1>\n"
html_escape("<script>")                   // "&lt;script&gt;"
url_parse("https://h:8443/p").port        // 8443
url().scheme("https").host("h").segment("a")
   .param("k", "v").toString()             // "https://h/a?k=v"
base64_encode(mime_bytes("hi"))           // "aGk="
logger("api").json().info("started")      // one JSON object per line
uuid4()                                   // "5f2b0c18-6a3e-4d71-9c2a-1e7f3b8d4a06"
```

## Modules

| Module | What it gives you |
|--------|-------------------|
| `vyto/util/text` ⚙ | `StringBuilder` (O(n) building) and small string helpers |
| `vyto/util/fmt` ⚙ | printf-style formatting: `fmt`, `fixed`, `hex`, `commas` |
| `vyto/util/sort` · `vyto/util/array` | `sort_by`, `binary_search` · array constructors |
| `vyto/util/time` ⚙ · `vyto/util/date` ⚙ | monotonic and wall clocks, `Timer` · calendar dates |
| `vyto/util/json` | `JsonValue`, `json_parse`, `json_encode`, `json_quote` |
| `vyto/util/csv` | RFC 4180 reader/writer, `Dialect`, `csv_sniff`, streaming over files |
| `vyto/util/xml` | `XmlReader` (pull) and `XmlNode` (DOM), namespaces, entities |
| `vyto/util/toml` | TOML v1.0, parsed into a `JsonValue` |
| `vyto/util/ini` | INI/conf, parsed into a `JsonValue` |
| `vyto/util/markdown` | `md_parse` to an `MdNode` tree, `md_to_html` to render it |
| `vyto/util/html` | `html_escape`/`html_escape_attr`, `html_unescape`, `HtmlBuilder` |
| `vyto/util/url` | `Url` and the `url()` builder, `url_join`, `QueryParams`, `url_encode`/`url_decode` |
| `vyto/util/mime` | `MediaType`, base64, quoted-printable, multipart |
| `vyto/util/log` | `Logger` — levels, structured fields, text or JSON lines |
| `vyto/util/uuid` ⚙ | UUID v4/v7, `Uuid7` for ordered keys, `rand_bytes`/`rand_int` |
| `vyto/util/helper` | `is_http_url`, UTF-8 caret movement |

⚙ marks a module backed by a native shim.

## Two conventions the whole directory follows

**Parsers fail softly; encoders panic.** `csv_parse`, `xml_parse`, `toml_parse`,
`ini_parse`, `url_parse`, `mime_parse` and `json_parse` never panic on bad
input. Each returns a value carrying the error — `error()` on a reader,
`parseError()` on a `JsonValue`, `err` on a `Url`. Encoders are the other way
round: writing a null to TOML or an array to INI is a bug in the caller, and it
panics with a file and line. This is the library-wide "panic hard, sentinel
soft" rule stated in `fmt.vt`.

```vyto
let cfg = toml_parse(readfile("app.toml"));
if (cfg.parseError() != "") { print(cfg.parseError()); exit(1); }
```

**Accessors are forgiving.** A missing key, a wrong kind, an out-of-range index
all return an empty value rather than crashing, so walking an uncertain document
needs no guard at every hop:

```vyto
doc.get("server").get("tls").get("cert").asString()   // "" if any hop is absent
xmlDoc.find("channel").find("title").text()           // "" likewise
```

## Things worth knowing before you use these

> **TOML and INI produce a `JsonValue`, not a type of their own.** Their data
> model is JSON's, so they reuse the tree, its accessors, and `json_encode`. Two
> consequences: TOML datetimes arrive as **strings** (a `JsonValue` has no
> datetime kind), and every INI value is a **string** (INI has no types at all —
> read a port with `parse_int` from `vyto/cli`, not `asInt`).

> **`vyto/util/markdown` escapes raw HTML rather than forwarding it.** A
> renderer that passes arbitrary HTML through is an XSS hole wherever the source
> is not fully trusted, and a library cannot assume that it is. `<b>x</b>` in
> markdown source renders as visible text.

> **`vyto/util/html` generates; it does not parse.** A real HTML5 tree builder
> is a different project. For input that is genuinely well-formed, use
> `vyto/util/xml`. `HtmlBuilder.raw()` is the one deliberate hole in the
> escaping — which makes "where does unescaped markup enter this page" a grep
> for `.raw(`.

> **`vyto/util/uuid` lives in its own directory for a build reason.** A
> `native/src` is compiled once per *package directory*, for every module in it.
> Putting the entropy shim in `lib/vyto/util/native/src` would compile it — and
> force the Windows `bcrypt` link — into every program that imports
> `vyto/util/fmt`.

> **The URL builder is a separate type from `Url`.** A Vyto method may not share
> a name with a field, and `Url`'s fields are already `scheme`, `host`, `port`
> and so on — `u.host` on a parse result is the better use of those names. So
> `url()` returns a `UrlBuilder` whose setters are `builder` methods, and
> `urlOf(s)` seeds one from an existing URL. Its `segment()` percent-encodes, so
> a value from elsewhere cannot climb out of the path it was meant to sit in.

> **`url_encode` and `base64_encode` moved here.** They used to live in
> `vyto/net/http` and `vyto/net/websocket`, which re-export them, so the old
> import paths still resolve. New code should import them from `vyto/util/url`
> and `vyto/util/mime`.
