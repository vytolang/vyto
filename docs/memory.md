# Memory in Vyto

> **Status: experimental.** Names and edge-case behaviour can still change.

How Vyto manages memory, and the gotchas that come with the trade-offs it
makes. The short version: reference counting, no GC, deterministic frees —
which buys low latency and low memory at the cost of one sharp edge (cycles
leak) and one discipline you opt into for scale (arenas). Every claim below
is grounded in `runtime/vyto_rt.c` and `src/check.c`/`src/emit.c`, with
file:line pointers where the behaviour isn't visible from `.vt` source.

## 1. The model in one paragraph

Every reference-typed value — `string`, arrays, `Map`, `class` instances,
`fn` closures — carries a refcount. Assigning, passing, or storing one
retains it; a local going out of scope, a slot being overwritten, or a
`class` deinit running releases what it held. When the count hits zero the
object's memory (and every strong reference *it* holds) is freed
immediately, synchronously, at the point of the last release. There is no
garbage collector, no tracing pass, and no pause to wait for — frees happen
exactly where you'd predict from reading the code top to bottom.

`int`, `float`, `bool`, `byte`, and `struct` are **not** reference-counted at
all — they're copied. A `struct` can't even hold a reference-typed field
(`src/check.c:298-302`: `"struct fields must be value types in v0.1"`), so
copying one is a flat `memcpy`, zero refcount traffic, full stop. See
[`docs/types.md`](types.md) for the complete value-vs-reference type list.

## 2. What refcounting actually costs

An allocation is one bump or freelist pop from a per-size-class thread-local
pool (`runtime/vyto_rt.c:21-53`) — 10 classes from 16 to 512 bytes, carved
out of 64 KiB chunks, so a single small object is cheap: no `malloc` call,
no lock. What is **not** cheap is *count*. A million-object structure built
one object per element pays a million pool headers, a million independent
retain/release lifetimes, and scattered cache lines on every traversal —
that's real cost the profiler will show you, even though no single
allocation is slow. §5 below is what to do instead.

## 3. Reference cycles leak — nothing detects it

The one real gotcha in the model: a strong cycle (A holds B, B holds A —
directly or through a chain) never reaches refcount zero, so it never frees.
There is no cycle collector and no leak detector built in; it fails silently
by simply not showing up as a bug until you're watching RSS climb.

Two shapes cause this in practice:

**Parent/child back-references.** A widget tree, a DOM-like structure, any
"child points back to its owner" relationship:

```js
class Widget {
    children: Widget[];   // owns them — strong, correct
    parent: weak Widget;  // points back — must be weak
}
```

**A bound method stored back onto its own receiver.** `obj.method` captures
`obj` as its receiver; assigning it onto a field `obj` itself owns —
`this.btn.onPress = this.handle` — creates the same cycle through a closure
instead of a class field.

### `weak` breaks it

`weak` is a real keyword, and it applies **only to class types**
(`src/check.c:240`: `"'weak' applies only to class types"` — you can't weak
a `string` or an array, only a `class` reference). A `weak` field is not
counted in the target's refcount, and reads as `null` once the target is
actually freed rather than dangling:

```js
sb_printf(dst, "    vt_weak_drop((void**)&self->f_%s);\n", ...)   // deinit
sb_printf(em->out, "vt_weak_set((void**)&%s, %s);\n", ...)        // assignment
```

(`src/emit.c:1985`, `:1442`) — a `weak` slot is a plain pointer that gets
nulled out as a side effect of the target's own teardown, not a second
refcounted handle to the same object.

**Any back-reference — child → parent, part → owner, observer → subject —
should be `weak`.** Owning references (a parent's list of children) stay
strong. `lib/vyto/ui/core.vt` is the toolkit-wide example to copy from:
`parent: weak Widget`, `win: weak Window`, `last_click_w: weak Widget`. See
[`docs/classes.md`](classes.md) §7 for the full construction/`deinit`
interaction.

### Reading a `weak` field gives you an optional

Because a `weak` slot reads as `null` once its target is gone, **loading one
yields a value the checker refuses to let you dereference until you have tested
it**:

```js
fn render(s: Painter, th: Theme) {
    this.win.skin.button.paint(...);     // error: 'this.win' is a weak
                                         // reference and may be null here
}
```

That is not a style rule. Before it existed, the toolkit carried 52 unchecked
weak dereferences, and one of them — `Button.render` reading `this.win.skin` —
was a real segfault the moment a widget was painted before the window had
adopted it. A `weak` field that is *designed* to become null should not fault
when it does.

Every ordinary way of checking narrows the path for the rest of that branch:

```js
if (this.win != null) { this.win.redraw(); }        // inside the branch
if (this.win == null) { return; }                    // …and after an exiting guard
this.win != null && this.win.mods != 0               // right of a short-circuit
while (this.win != null) { ... }                     // in the loop body
let w = this.win; if (w == null) { return; }         // bound, then checked
```

Writing to the path drops what was known about it, so this is refused:

```js
if (this.win == null) { return; }
this.win = null;
return this.win.n;                       // error — the check no longer holds
```

**One deliberate hole:** a narrowing on `this.win` survives a call, and a call
could in principle null that field. Invalidating on every call would reject
`if (this.win != null) { this.win.layout(); this.win.redraw(); }`, which is the
shape the rule exists to make safe. TypeScript makes the same trade. Bind a
local when a callee might really clear the field — a local cannot be reached
that way.

The check covers *dereferences*: reading a member, calling a method. Passing a
possibly-null reference along is still allowed, because it is still a valid
reference — it is only unsafe to follow.

**Closures capture by value**, which mitigates but doesn't eliminate this:
a click handler can call methods on a captured class instance (instances
are references, so the capture is a retained pointer) but can't mutate a
captured primitive and have it persist. That capture is itself a strong
reference, so storing the closure back onto the object it captured
reproduces the same cycle as the bound-method case above. Clear the field
during teardown, or keep the back-reference `weak`, if you go this route.

## 4. Arenas — opt-in bulk teardown, not a GC alternative

For a batch of objects that all die together — build a tree, walk it, throw
it all away — refcounting every node individually is wasted work. `arena { }`
bump-allocates instead: every `new` inside the block lands in one region,
and the whole region is freed in a single pointer bump at the block's end,
with **no** per-object destructor call:

```js
class Node { value: int; next: Node; }

fn main() {
    let sum = 0;
    arena {
        let head: Node = null;
        let i = 0;
        while (i < 1000) {
            let n = new Node();      // bump-allocated in this arena
            n.value = i;
            n.next = head;           // interior ref to a sibling: fine
            head = n;
            i += 1;
        }
        let cur = head;
        while (cur != null) { sum += cur.value; cur = cur.next; }
    }                                // arena freed here, in one shot
    print("sum = " + sum);
}
```

Named, nested arenas exist too — `new@outer` inside an inner block targets
an enclosing region instead of the innermost one (`examples/70_arena.vt`).

**The compiler enforces the one invariant that makes this safe: nothing
arena-allocated can outlive its arena.** Both directions are checked
statically, not at runtime:

- **Storing an arena value somewhere longer-lived is a compile error** —
  `region_check_store` (`src/check.c:454-460`) rejects any `let`,
  assignment, field store, or array/map store where the arena value's
  region doesn't outlive the target's:
  > `"arena value would outlive its arena via %s (it is freed when its arena block ends)"`
- **Returning one from a function is a compile error** (`src/check.c:2733-2735`):
  > `"cannot return an arena value (it is freed when its arena block ends); copy it out instead"`

Two restrictions keep the model MVP-safe, both checked at the `new` site
(`src/check.c:2390-2405`):

- **An arena class can't have a custom `init`** — `"arena class '%s' cannot
  have a custom init yet"`.
- **Every field down its inheritance chain must be a scalar or another
  class reference** — no `string`, array, `Map`, or `fn` field, since those
  are independently heap-managed and the arena's bulk free has no per-object
  hook to release them: `"arena class '%s' has a heap-managed field '%s';
  not arena-safe yet"`.

Reach for `arena { }` only for genuine bulk allocation — large counts of
short-lived, uniformly-scoped objects where refcount overhead is a measured
cost. Everyday classes should just use `new`; arenas trade the deinit
guarantee (and heap-managed fields) away, so don't reach for one out of
habit.

## 5. Bulk data: one allocation per element is the real trap

This is the gotcha that doesn't announce itself: nothing crashes, nothing
leaks, it's just slow and memory-hungry in a way that's easy to miss until
the input is large. The fix isn't `arena { }` (structured data usually needs
mutation and lookup, not just bulk teardown) — it's **not allocating one
object per element in the first place**.

**For lists, tables, and text at scale, allocate one contiguous backing
buffer and hand out offsets/indices into it.** The reference implementation
is `lib/vyto/data/native/src/coltable.c` — read it before writing anything
similar. Its shape:

- struct-of-arrays: each column is one typed buffer, so a cell is one
  indexed load with zero per-cell boxing;
- strings are `(offset, len)` into a **shared string arena**, not a
  `string` object per cell;
- sort and filter never move column data — they permute an `idx` array that
  names the visible rows, so sorting a million rows is a permutation of one
  `i64` array, not a million refcount-touching swaps;
- growth doubles, and a `reserve`-style call pre-sizes so a bulk load is
  realloc-free.

`StringBuilder` (`lib/vyto/util/text.vt`) is the same idea in the small: one
growing buffer instead of a fresh string per concatenation. `.clear()`
deliberately keeps the allocated capacity (`sb_clear`, `text.vt:65`) so the
buffer is reused across loop iterations instead of reallocated from zero
every time.

**Concrete trap, verified against the runtime: `readlines()` allocates a
fresh `string` per line** — `vt_file_lines` (`runtime/vyto_rt.c:839-856`)
walks the whole file but calls `vt_str_new` (a new refcounted allocation)
for every line it splits out. On a large file, call `readfile()` once and
work on byte offsets into the single returned buffer instead — `.slice()`
still allocates per call, so even then prefer `StringBuilder.appendSlice()`
(§ above) over repeated `.slice()` + `.append()` when building output from
many small ranges of a big buffer.

## 6. Checklist

- **Every back-reference is `weak`.** Child → parent, part → owner,
  observer → subject. If you're not sure which direction "owns," the one
  that outlives the other on teardown is the strong one.
- **A closure or bound method stored back onto the object it closed over is
  a cycle.** Treat it exactly like a class field back-reference.
- **Reach for `arena { }` only when you measured a refcounting bottleneck
  on bulk, uniformly-scoped allocation** — not as a default for "a lot of
  objects." It costs you custom `init` and heap-managed fields.
- **A million small objects is the actual scaling trap, arena or not.**
  Struct-of-arrays with an index array, per `coltable.c`, is the fix — not
  "wrap it in an arena and move on."
- **`readfile()` once, not `readlines()` on a large file** — the latter is
  one allocation per line by construction.
- **A `struct` never carries refcount cost** — it can't hold a
  reference-typed field at all, so passing/copying one is just bytes moving,
  never a retain.
