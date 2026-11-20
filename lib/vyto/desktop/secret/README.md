# vyto/desktop/secret

Keeps a small secret, such as an API key or a passkey, in a file of the app's
own. Only the user who saved the secret can read it.

```vyto
import { secret_save, secret_load, secret_found } from "vyto/desktop/secret";
import { dataDir } from "vyto/desktop";

let path = dataDir("myapp") + "/api.sec";
secret_save(path, key);

let k = secret_load(path);
if (!secret_found()) { /* ask for it again */ }
```

## Why a file of its own

A secret must stay out of the app's database, its settings table, its logs
and anything a backup copies. The app picks the file, and keeps it beside
those things, never inside them. A restored backup then needs the secret
typed in again, which is the intent: a copied backup is not a copied
password.

## Protection

| OS | How |
|---|---|
| Windows | DPAPI (`CryptProtectData`, user scope). The file holds an encrypted blob that only the same Windows account can open. A copy taken to another PC or another account reads as nothing. |
| Linux, macOS | No keyring: libsecret would pull in D-Bus and GLib for one string. The file is created mode 0600, and `secret_private()` checks it still is. |

`crypt32.dll` is loaded at run time, as `vyto/net`'s CA code loads it. It is
not in the audited Windows 7 export list.

Writes go to `<path>.tmp` and are renamed over the old file. A crash
mid-write leaves the old secret, never half of a new one.

## API

| Call | Returns | |
|---|---|---|
| `secret_save(path, value)` | `bool` | replaces what was there; false if it could not be written |
| `secret_load(path)` | `string` | the secret, or `""` when the file is missing, damaged, saved by another Windows account, or saved on another OS |
| `secret_found()` | `bool` | whether the last `secret_load` found a secret, which tells a saved `""` apart from nothing |
| `secret_private(path)` | `bool` | POSIX: the file exists and only its owner can read or write it. Windows: the file exists |
| `secret_remove(path)` | `bool` | deletes the file; false if there was none |
| `secret_encrypted()` | `bool` | true where the contents are encrypted (Windows DPAPI); false where the file's permissions are the protection |

Binary operands evaluate right to left. Call `secret_load` on its own line
before asking `secret_found()`.

A freestanding build (`--freestanding`) compiles, and every call returns its
failure value.

Tested by `examples/117_secret.vt`, and by `tests/win/secret_win.vt` (DPAPI,
staged by `make test-win`, run by `make test-wine`).
