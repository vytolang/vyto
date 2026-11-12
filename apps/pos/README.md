# Vyto Café POS

A one-screen point of sale, as a proof of concept for Windows apps in Vyto:
big touch buttons, a running ticket, cash payment, receipts and a daily
sales log. About 1 MB, one exe, no DLLs, runs on Windows 7 and later.

```sh
# Windows (Direct2D, Segoe UI); --audit checks it would load on Windows 7
./vytoc build apps/pos/pos_win.vt --target windows-x64 --release --audit \
    --manifest apps/pos/pos_manifest.vt -o pos.exe

# Linux, for development (blend2d, Inter)
./vytoc run apps/pos/pos.vt
```

| File | |
|---|---|
| `posapp.vt` | the whole app: model, widgets, the screen |
| `pos_win.vt` | Windows entry: `D2DPainter`, falling back to the built-in renderer where Direct2D cannot draw |
| `pos.vt` | Linux entry: `GfxPainter` |
| `pos_manifest.vt` | the exe's name, version and company (`--manifest`) |

## What it does

- **Sell:** category tabs over product tiles (a badge shows how many are on
  the ticket); tap a ticket line to select it, then **−**/**+**/**Void line**;
  **Clear** empties the ticket; discounts of 5, 10 or 15%.
- **Totals:** prices include 16% VAT, so the VAT shown is the share of the
  total (16/116), not an addition. Money is integer cents throughout.
- **Pay:** cash keypad, quick-cash buttons (exact, 500, 1,000, 2,000), change
  due; **Complete sale** only enables once the cash covers the total.
- **Receipt:** previewed on screen; saved as text and appended to a per-day
  CSV under `vyto/desktop`'s `dataDir("vyto-pos")` (`%LOCALAPPDATA%\vyto-pos`
  on Windows, `~/.local/share/vyto-pos` on Linux). **Save copy…** uses the
  OS save dialog; **Open sales folder** opens it in Explorer. Today's count and
  total are reloaded from the CSV on start.

## How it is built

One custom root widget (`PosScreen`) places every region itself in
`arrange()` and paints panels and text in `draw_chrome()`; the interactive
pieces — tiles, buttons, the ticket list — are child widgets, so hit-testing,
press feedback and partial repaints come from `vyto/ui`. Changing mode
(sell / pay / receipt) rebuilds the children and relayouts.

The model (`Pos`) refers to the window and the screen only through `weak`
fields; the widgets reach the model through closures. No strong cycle.

Not done, deliberately, for a proof of concept: card/M-Pesa payment, a
product editor, users and shifts, a printer. Prices and the menu are in
`Pos.init`.
