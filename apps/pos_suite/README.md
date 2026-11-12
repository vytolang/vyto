# Vyto Café POS Suite

The till from `apps/pos`, grown into a suite: the GUI plus two background
services it talks to, all installed into one folder by one setup.exe.
(`apps/pos` itself is left as the single-exe proof of concept.)

```sh
# Windows: the till, payments.exe and stock.exe, one installer
./vytoc build apps/pos_suite/pos_win.vt --target windows-x64 --release --audit \
    --manifest apps/pos_suite/pos_manifest.vt --installer -o pos.exe
#   -> Vyto-Café-POS-Suite-0.2.0-setup.exe

# Linux, for development: the services from a terminal each, then the till
./vytoc run apps/pos_suite/payments.vt
./vytoc run apps/pos_suite/stock.vt
./vytoc run apps/pos_suite/pos.vt
```

| File | |
|---|---|
| `posapp.vt` | the till: model, widgets, the screen, and its two channels |
| `menu.vt` | the menu and the suite's names, shared by the till and the stock service |
| `payments.vt` | service `VytoCafePayments`: simulated M-Pesa |
| `stock.vt` | service `VytoCafeStock`: what is left of each item |
| `pos_win.vt` / `pos.vt` | Windows (Direct2D) / Linux (blend2d) entries |
| `pos_manifest.vt` | identity, the two programs, their services, Payments needs Stock |

## What the services do

**Payments.** On the pay screen, **M-Pesa (simulated)** sends `pay.simulate`
with the sale's total; the service answers with the payment record —

```json
{"id":"PAY-000001","method":"M-Pesa","ref":"WAQUN7TQ9K","amount_cents":89000,
 "phone":"2547****678","status":"confirmed","time":"2026-11-12T19:01:24","till":"till-1"}
```

— and the till completes the sale with that ref on the receipt and in its CSV.
The record also goes to every connected till as `payment.received` (another
till's payment shows on the status line), and to the service's own
`payments-YYYY-MM-DD.csv`.

**Stock.** Counts per item (`stock.json`, seeded at 30). Tiles show "N left"
and grey out at 0; a completed sale sends `stock.sell`; the service pushes
`stock.changed` to every till and `stock.low` when an item drops below 5.

Service data lives in `sharedDataDir("vyto-cafe-suite")` — `%ProgramData%` on
Windows; the till's own sales log in `dataDir("vyto-pos-suite")`. Nothing is
shared with `apps/pos`, so both can be installed side by side.

## When a service is down

The header pills show each service, green when connected. Stop either one —
`services.msc`, or Ctrl+C in its terminal — and its pill goes grey: M-Pesa
reads "Payments offline", tiles stop showing counts. Cash sales carry on.
Start it again and the till reconnects by itself, even though the service is
on a new port.

## Ownership

The till's model (`Pos`) holds the window and screen through `weak` fields.
Its channels' callbacks capture the model, which owns the channels: a cycle,
but one that lives exactly as long as the app and never grows.
