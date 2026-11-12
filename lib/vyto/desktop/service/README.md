# vyto/desktop/service

Run a Vyto program as a **Windows service** or a **systemd service** from the
same code, and install or remove one.

```vyto
import { Service } from "vyto/desktop/service";

fn main() {
    let svc = new Service("VytoSync");
    svc.run(() => {
        svc.log("started");
        while (!svc.stopping()) {
            syncOnce();
            svc.wait(5000);          // sleeps, but wakes at once on a stop
        }
        svc.log("stopped");
    });
}
```

| | Started by | Stop request | `log()` goes to |
|---|---|---|---|
| Windows service | Service Control Manager | Stop, or system shutdown | `%ProgramData%\<name>\<name>.log` |
| systemd unit | systemd (`Type=notify`: `READY=1`, `STOPPING=1` are sent) | SIGTERM | stderr → journald |
| terminal | you | Ctrl+C (SIGINT) | stderr |

The terminal row is how a service is debugged: it is the same program,
running the same loop. `svc.isService()` says which case it is.

The body runs on the program's main thread. On Windows the SCM dispatcher
lives on a helper thread in C and only sets events, so nothing ever calls
into Vyto from a foreign thread.

## Installing

Needs administrator rights (Windows) or root (Linux). Every call returns
false with `serviceError()` saying why.

```vyto
import { serviceInstall, serviceStart, START_AUTO } from "vyto/desktop/service";

serviceInstall("VytoSync", "Vyto Sync", "Keeps the shop's data in sync",
               "", "", START_AUTO);     // "" = this program
serviceStart("VytoSync");
```

- **Windows:** registered with the Service Control Manager, its path quoted
  (no unquoted-service-path hole), restart-on-failure set (5 s, 30 s, then
  every 60 s). `START_AUTO_DELAYED` starts it after boot settles.
- **Linux:** a unit in `/etc/systemd/system` (`systemdUnit(...)` returns its
  text), then `systemctl daemon-reload` and, unless `START_MANUAL`, `enable`.
  `Restart=on-failure`.

`serviceStart`, `serviceStop`, `serviceState` (`SVC_NOT_INSTALLED`,
`SVC_STOPPED`, `SVC_RUNNING`) and `serviceUninstall` manage it afterwards.

For a Windows app, the usual way is to let the installer do it: declare the
service in the app's `--manifest` and build with `--installer`, which
registers it on install, stops it on upgrade and removes it on uninstall:

```vyto
new WinManifest("Vyto Sync").version("1.0.0")
    .service("VytoSync", "Vyto Sync", "Keeps the shop's data in sync", "auto")
    .emit();
```

## Tests

- `make test`: `service_sigterm`, `service_sd_notify` (a datagram listener
  stands in for systemd), `service_unit` (unit text and install into a temp
  dir via `VYTO_SYSTEMD_DIR`), `freestanding_service_stub`.
- `make test-wine`: `wine_service_lifecycle` (install, start by the SCM, log,
  stop, uninstall) and `wine_installer_machine` (the same through
  `--installer`, including an upgrade).
