# Hacker News — an Android app in Vyto

A native HN reader: a feed with six tabs, a story screen with its full comment
tree, links opened in the browser, pull-to-refresh, pagination and a working
system Back key. Roughly 1000 lines of Vyto, no Java in the app itself, no
Gradle, no Kotlin, no AndroidX. It builds to a 1.6 MB apk.

It runs on a real device — verified on a Redmi Note 9S (Android 12, arm64-v8a).
Screens are drawn by `vyto/ui` through `android.graphics.Canvas`; nothing here
is an `android.widget`.

There is a second, unrelated Hacker News app in this repo: `apps/hackernews` is
an HTTP server that renders HTML. The two share an API design and nothing else.
This one lifts that app's Algolia client and changes how it fetches.

```
vytoapp.vt     entry point — window, painter, the feed screen's frame
hnapp.vt       all state and all logic; every widget callback is one call into it
hnapi.vt       the Algolia HN client: request builders, JSON → objects, TTL cache
hnrows.vt      the five row widgets, each drawing its own text
hnfmt.vt       wrap/elide, relative time, HTML → text
manifest.vt    a host program that prints AndroidManifest.xml
preview.vt     desktop preview of the story screen, for iterating without a phone
build-apk.sh   the 12-step apk pipeline
```

## Build

Needs an NDK and an SDK. Nothing in `make test` touches this app — the host
suite stays toolchain-free so CI never has to download either.

```sh
make                                  # build vytoc first
sh apps/hn_android/build-apk.sh       # -> apps/hn_android/build/hn.apk
```

Toolchains are found at `$ANDROID_NDK_HOME` / `$ANDROID_SDK_HOME`, falling back
to `~/Android/Ndk` and `~/Android/Sdk`. Developed against NDK r27d. The script
builds and verifies; it installs nothing.

Step 2 passes `--release`, which is not optional for something that ships:
`vytoc` compiles at `-O0` without it, so the apk would carry an unoptimized
`.so` (1.9 MB against 1.6 MB here). It also keys the object cache separately,
so a release build never reuses a debug build's objects.

```sh
adb install -r apps/hn_android/build/hn.apk
adb shell am start -n dev.vyto.hn/dev.vyto.android.VytoActivity
```

To iterate on the look without a device — this runs the story screen on the
desktop with canned data:

```sh
./vytoc run apps/hn_android/preview.vt
```

The preview is worth using for more than convenience: it draws through the lean
`SurfacePainter`, which cannot alpha-blend, so it catches a class of bug the
device *hides*. A translucent fill that looks correct on the phone can render
solid here — and that is the honest degradation, not the preview being wrong.

## Things this app exists to demonstrate

**The entry file must be named `vytoapp.vt`.** `VytoActivity.libraryName()` is
hardcoded to `"vytoapp"` and calls `System.loadLibrary` on it, and the compiler
names the `vyto_app_main` export after the module's basename.

**Fetching must not block.** The Vyto thread is also the render thread, so a
`Request.send()` in a tap handler freezes the UI for a whole round trip.
Everything goes through `HttpPool` and a ticker draining `poll(0)` once a
frame — and that ticker exists *only* while requests are outstanding. A
registered ticker keeps `Window.animations` non-empty, which pins the loop to a
16 ms wait and leaves vsync gating on forever, so an idle app would burn a core.
Start it on the first in-flight request; cancel it when the last one lands.

**Navigation belongs on `on_release`, not `on_click`.** `Window` dispatches
`on_click` from the mouse *down* — the desktop convention. A row that navigates
there fires the instant a finger lands, so the list cannot be scrolled by
dragging anywhere over its content: every row becomes a trap. `on_release`
fires only on a release inside the widget with no drag past the slop, which is
exactly a tap. The rows still return the press from `on_click` (returning
`false`), because that capture is what routes the drag to the `ScrollView`.

**Rows have no children.** `Widget.hit` walks to the deepest leaf, and neither
`Label` nor `Text` handles a click — so a row built out of them swallows the
tap silently. A childless widget that draws its own text hit-tests to itself and
needs no `hit()` override. It is also far cheaper: a comment thread is 150 rows,
and as `Text` widgets that would be 150 selection models in the focus ring.

**A bare `Box` does not size its child.** `Widget.arrange` is the
absolute-placement escape hatch and leaves children at whatever bounds they
already hold, so a freshly built subtree lands at 0×0. `Padding(0)` is the
single-child container that hands its rect down.

**Bounce and pull-to-refresh are the same gesture.** Both are a downward drag on
content already at its top. The feed's `ScrollView` sets `bounce = true` and
still pulls to refresh, because `on_drag` offers the drag to every ancestor
before it bounces — `PullToRefresh` claims it there.

**`<queries>` is load-bearing.** On targetSdk ≥ 30 an app cannot see a package
it has not declared interest in, so without the block `mf.uses(ACT_VIEW_URL)`
emits, `Actions.view_url` returns false and says nothing — story links would
simply do nothing on a device. `build-apk.sh` asserts it survived into the
linked manifest.

## Limits worth knowing

- **One page of comments.** The tree is capped at 150 rendered nodes and 8
  levels of depth; past that a row offers to open the thread on the website.
- **No user profiles, no search, no login.** The Algolia API this reads is
  public and read-only.
- **arm64 only.** `build-apk.sh` packages one ABI. Adding `armeabi-v7a` or
  `x86_64` means another `--target` pass and another `lib/<abi>/` entry.
- **Gesture testing is manual.** MIUI refuses `adb shell input tap` and
  `input keyevent` into another app without `INJECT_EVENTS`, so every claim
  about how this feels came from a finger.

Design of record for the port as a whole: `local/docs/ANDROID.md`.
