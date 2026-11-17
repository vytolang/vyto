# vyto/media/qr — QR Code encoder

Pure Vyto, no native code, imports nothing: freestanding-safe and identical on
every target.

```vyto
import { qrEncode, QR_ECC_M } from "vyto/media/qr";

let q = qrEncode("https://example.com", QR_ECC_M);   // null if too long
for (let y in 0..q.size) {
    for (let x in 0..q.size) {
        if (q.get(x, y)) { /* fill one module */ }
    }
}
```

| API | |
|---|---|
| `qrEncode(text, ecc = M): QrCode` | the string's bytes; null past version 40's capacity |
| `qrEncodeBytes(data: byte[], ecc = M): QrCode` | any binary payload |
| `QrCode.size` | modules per side (`version * 4 + 17`); no quiet zone included |
| `QrCode.get(x, y): bool` | true = dark; out of range reads light |
| `QrCode.modules: bool[]` | the grid, one flat row-major buffer |
| `QR_ECC_L/M/Q/H` | ~7 / 15 / 25 / 30 % of codewords recoverable |

Byte mode only, versions 1–40 picked automatically, all eight masks scored by
the standard's penalty rules (Project Nayuki's reference algorithm). Numeric,
alphanumeric and kanji segments are not implemented — they save a version or
two, and every reader accepts byte mode.

Leave a **4-module light border** when you draw it; readers need the quiet zone.

Drawing is the caller's job on purpose: a Painter fills rects, a thermal
printer wants a 1-bit raster, a PDF wants a path.

## Tests

- `tests/unit/qr_encode.vt` (`make test-dev`): published known answers
  (Reed–Solomon, format/version words, capacities) and a read-back of every
  version 1–40 through an independent walk.
- `tests/fixtures/qr_scan.vt`: writes PNGs to `tests/tmp/qr/` for a real reader
  — `zbarimg` (`zbar-tools`) or a phone. Self-agreement cannot prove a reader
  accepts the symbol; only a reader can.
