# vyto/desktop/channel

A local message channel between the programs of one app suite: a GUI and the
background services it relies on (`vyto/desktop/service`). Requests with
replies, and events the service pushes without being asked.

```vyto
// a service
let srv = new ChannelServer("vyto-cafe-suite", "payments");
srv.handle("pay.simulate", (r) => {
    if (r.data.get("amount_cents").asInt() <= 0) { return r.fail("amount must be more than zero"); }
    let rec = jobj();
    ...
    srv.push("payment.received", rec);     // to every connected client
    return rec;                            // the reply
});
while (!svc.stopping()) { srv.poll(250); }
srv.close();

// the GUI
let ch = new Channel("vyto-cafe-suite", "payments", "till-1");
ch.onState((up) => { ... });              // show "Payments online / offline"
ch.onPush("payment.received", (d) => { ... });
ch.request("pay.simulate", body, cb);     // cb(r): r.ok, r.data, r.err — exactly once
attachChannel(win, ch);                   // vyto/desktop/channel/gui

// a script, or one service calling another
let r = ch.call("pay.today", jobj(), 5000);
```

Working example: `apps/pos_suite` — the till plus a payments and a stock
service, installed together by one setup.exe.

## Behaviour

- **Requests** time out after 10 s; a request made while offline fails at once
  with `"offline"`; one in flight when the connection drops fails with
  `"disconnected"`. The callback runs exactly once either way.
- **Reconnects** happen by themselves, backing off from 0.5 s to 5 s. A
  restarted service is found again even on a new port.
- **The GUI never blocks:** `attachChannel` makes the channel's socket a Window
  poll source. The loop sleeps until a message arrives (X11, and Win32 — which
  waits on the socket with `WSAEventSelect`; measured at 0 CPU idle), and
  callbacks run on the GUI thread between events.
- **The server never blocks on a slow client:** writes are queued per client
  and a client more than 8 MiB behind is dropped.

## Ports: no conflicts, no fixed numbers

A service listens on **127.0.0.1 only** — never reachable from the network —
on **port 0**, which asks the OS for a port that is free right now (the
ephemeral range). Nothing in a suite ever names a port, so it cannot collide
with another program, another service, or a second copy of the suite. The port
is held **exclusively** (`socket_listen_exclusive`: `SO_EXCLUSIVEADDRUSE` on
Windows, where `SO_REUSEADDR` would otherwise let another process bind it too),
so nothing can take it over while the service runs.

Clients find the port in the service's discovery file,
`<dir>/<suite>/<channel>.chan` = `{"port","token","pid","name"}`, read on every
connect. A file whose `pid` is no longer running is ignored (offline, not a
connection to whatever got that port next).

`<dir>`: `$VYTO_CHANNEL_DIR` if set; else `%ProgramData%` on Windows; on Linux
`/run/vyto` (system services), `$XDG_RUNTIME_DIR/vyto`, `/tmp/vyto-<uid>`, in
that order. A `/tmp` directory must be ours, not a symlink and not
world-writable.

## Trust

A client must open with the token from the discovery file (compared in
constant time) within 5 s, or the connection is dropped. The file is readable
by signed-in users and writable only by SYSTEM and Administrators (Windows,
set by security descriptor) / mode 0644 (POSIX).

So the channel keeps out web pages poking at localhost ports and stray
processes, and is reachable by **any program run by a user signed in to that
machine**. That is the right level for a till; a service that must also guard
against other local users should check `r.client` and its own permissions on
top.

## Wire format

Frames are a 4-byte big-endian length and UTF-8 JSON, at most 1 MiB:

```
{"t":"hello","token","client"}      {"t":"welcome","server"}
{"t":"req","id","op","data"}        {"t":"res","id","ok","data"|"err"}
{"t":"push","topic","data"}
```

## Writing callbacks

Vyto closures do not nest: a callback made *inside* another closure is a
compile error. Make it first and pass it by name, or keep it in a field:

```vyto
let ignore: fn(ChannelReply) = (r) => { };
ch.onState((up) => { if (up) { ch.request("stock.all", jobj(), ignore); } });
```

## Tests

`make test`: `channel_basic`, `channel_push`, `channel_abuse` (wrong token,
oversized frame, malformed JSON, a silent client — each dropped, the server
keeps serving), `channel_loopback_only`, `channel_token_file`,
`channel_restart`, `channel_ports`, `channel_gui` (an idle X11 window receives a
push). `make test-wine`: `wine_channel_gui` (idle wakeups of the Win32 loop,
~64 per 2 s against ~480 for 16 ms polling) and `wine_installer_suite`.
