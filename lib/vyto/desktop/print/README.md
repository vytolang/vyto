# vyto/desktop/print

The printers installed in the OS, and **raw** printing through their queues:
the bytes go to the printer untouched. It is the connection for a device that
speaks its own language: an ESC/POS receipt printer (most USB thermal printers
install a Windows driver and no COM port), a ZPL or TSPL label printer.

```vyto
import { printers, printer_state, print_raw, job_state, cancel_job, print_error,
         state_text, PS_READY, JOB_DONE } from "vyto/desktop/print";

for (let p in printers()) { print(p.name + "  " + state_text(p.state)); }

if (printer_state("XP-80") == PS_READY) {
    let job = print_raw("XP-80", escpos, "Receipt T1-000042");
    if (job <= 0) { print(print_error()); }
    // queued is not printed: follow the job, and cancel it if it hangs
}
```

Nothing is drawn: there is no page, no font, no driver rendering. Drawing a
document through the driver (A4 invoices) is a later phase: a `PrintPainter`
over GDI on Windows, a PDF painter + `lp` on Linux.

A sibling of [`vyto/desktop`](../README.md), not part of it, so an app that
never prints does not link winspool.

## API

| Function | Returns |
|---|---|
| `printers(): PrinterInfo[]` | every printer the OS knows: `name`, `port` (Windows: USB001, COM3, an IP port; CUPS: the device URI), `is_default`, `state` |
| `printer_state(name): int` | `PS_READY`, `PS_BUSY`, `PS_OFFLINE`, `PS_ERROR`, `PS_PAPER_OUT`, `PS_PAUSED`, `PS_UNKNOWN`, `PS_MISSING`, `PS_UNAVAILABLE` |
| `print_raw(name, data: byte[], title): int` | the job id (> 0), or ≤ 0 with `print_error()` set |
| `job_state(name, job): int` | `JOB_QUEUED`, `JOB_PRINTING`, `JOB_DONE` (printed, or no longer in the queue), `JOB_ERROR`, `JOB_UNKNOWN` |
| `cancel_job(name, job): bool` | true when the OS took the cancel |
| `print_error(): string` | why the last call failed |
| `state_text(state)`, `job_text(state)` | a few words for a status line |
| `print_available(): bool` | false on freestanding and Android builds |

## How it works

| | Windows (winspool) | Linux, macOS (CUPS) |
|---|---|---|
| list | `EnumPrintersW` level 2, `GetDefaultPrinterW` | `lpstat -p`, `lpstat -d`, `lpstat -v` |
| state | `GetPrinterW` level 2: status bits and *work offline* | `lpstat -l -p <name>` (idle, printing, disabled; the Alerts line for paper and offline), then `lpstat -a <name>` (not accepting → error) |
| print | `OpenPrinterW` → `StartDocPrinterW` (datatype `RAW`) → `WritePrinter` → `EndDocPrinter` | the bytes to a `mkstemp` file, `lp -d<name> -o raw -t<title> -- <file>`, the job id from "request id is", the file removed |
| job | `GetJobW` level 1; a job no longer there is done | `lpstat -W not-completed -o <name>`; absent is done |
| cancel | `SetJobW(JOB_CONTROL_DELETE)` | `cancel <name>-<job>` |

On Linux the three programs are started from an **argument list, never a
shell**, with `LC_ALL=C` so their output parses in every locale, and each is
given **5 seconds**: a printing system that hangs must not hang the app. A
printer name that starts with `-` or holds a space, `/` or `#` is refused
before anything runs. `VYTO_PRINT_BIN_DIR` takes the programs from another
directory (the tests' fakes).

## Caveats

- **A queue accepts a job when the printer is off**, and prints it when the
  printer comes back. CUPS holds jobs for a paused queue; Windows does the
  same for an offline printer. This package reports that faithfully and
  decides nothing: an app that must not print late (a till whose job carries
  a cash-drawer kick) checks the state first, follows the job, and cancels it
  if it has not printed in time. Nothing should be left queued.
- **Many USB printer drivers never tell the spooler they are offline.** The
  state check is a fast path, not a guarantee; following the job is what
  catches a printer that is off.
- **The Windows 7 audit list does not have winspool.drv yet.**
  `lib/vyto/win/win7_exports_provisional.txt` lists the 12 functions used, by
  hand, so a program can be audited with it as an overlay
  (`cat win7_exports.txt win7_exports_provisional.txt > x; WIN7_EXPORTS=x`).
  `tests/win/dump_exports.c` now dumps winspool.drv: run `run.ps1` on the
  Win7 box, bring `win7_exports.txt` back, and delete the provisional file.
- CUPS calls raw queues deprecated; they still work in 2.4, and a printer's
  own driver queue also accepts `-o raw`.

## Tests

- `make test`:
  - `print_cli`: the CUPS arm against fake `lpstat`/`lp`/`cancel`
    (`tests/fixtures/print/fake_cups.sh`): the exact argv of every call, the
    parsing of each answer (states, alerts, job ids, a refusal), a title that
    tries to run a command arriving as one argument (no `pwned`), the temp
    file removed, and a hung `lpstat` given up on after 5 s
  - `freestanding_print_stub`: the stub arm builds with no libc and its object
    references nothing (`nm`)
- `make test-print`: against the real CUPS on this machine, through a
  temporary raw queue on `socket://127.0.0.1` captured by socat: bytes arrive
  identical; a paused queue holds the job and `cancel_job` empties it with
  nothing printed; and the winspool arm under Wine (which sees CUPS queues)
  delivers the same bytes. Needs lpadmin and socat; SKIPs otherwise.
- `make test-win` stages `print_demo.exe` (list; print a file raw, follow
  it, cancel after 10 s) for a real Windows box and printer.
