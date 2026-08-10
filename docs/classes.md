# Classes in Vyto

> **Status: experimental.** Names and edge-case behaviour can still change.

`class` is Vyto's one reference type with identity, inheritance, and virtual
dispatch. If you want a copied value instead, see `struct` in
[`docs/types.md`](types.md) §7. Everything below is grounded in
`src/check.c` (`layout_class`, `check_call`, the `EX_NEW` case) and
`src/emit.c` (constructor/deinit codegen) — file:line pointers are given
where the behaviour isn't obvious from reading `.vt` source alone.

## 1. Declaring one

```vyto
class Shape {
    name: string;
    fn init(name: string) { this.name = name; }
    virtual fn area(): float { return 0.0; }
    fn describe() { print(this.name + " area=" + this.area()); }
    deinit { print("~" + this.name); }
}
```

A class body is fields (`name: Type;`, no inline default value) followed by
methods and at most one `init` and one `deinit`. Declaring a second `init` or
`deinit` is a parse error (`src/parse.c:830`, `:844`).

Instances are heap-allocated and reference-counted — `TY_CLASS` is one of
the reference kinds (`src/ast.h:25`), alongside `string`/array/`Map`/`fn`.
`==`/`!=` on two classes compare **identity** (the underlying pointer), not
field values (`src/emit.c:1132`).

## 2. Construction

```vyto
let s = new Shape("circle");
```

`new` allocates and zeroes the instance, then runs `init` if the class (or
the nearest ancestor that has one) declares it — `init` is optional and,
unlike fields, is inherited by lookup, not copied
(`src/check.c:2372-2378`):

```vyto
for (ClassDecl *k = cd; k && !ctor; k = k->parent) ctor = k->ctor;
```

So a subclass with no `init` of its own uses its parent's, argument list and
all. If no class in the chain has an `init`, `new Foo()` must be called with
no arguments and every field starts at its type's zero value (`0`, `false`,
`""`-equivalent `null` for strings, `null` for other references) — nothing
enforces that you overwrite them, so a class meant to always be constructed
through `init` should declare one.

There is no constructor overloading — one `init` per class. Use default
parameter values on it for optional construction arguments, the same as any
other function.

## 3. Methods: plain, `virtual`, `override`

A method is plain by default: called directly, no indirection, and the C
compiler is free to inline it. Mark it `virtual` to get dynamic dispatch —
one indirect call through the class's vtable — and `override` in a subclass
to replace it:

```vyto
class Circle extends Shape {
    r: float;
    fn init(r: float) { super.init("circle"); this.r = r; }
    override fn area(): float { return 3.14159 * this.r * this.r; }
}
```

(`examples/03_classes.vt`) Rules the checker enforces at class-layout time
(`src/check.c:350-390`, `layout_class`):

- **`override` requires something virtual to override**, with an *identical*
  signature (same param types, same return type) — no covariant returns, no
  overloading by arity.
- **A plain method cannot reuse an inherited method's name at all**, virtual
  or not — that's "hiding", and it's a compile error steering you to
  `override` instead. There's no way to accidentally shadow a base method
  the way you can in some C-family languages.
- **A field cannot reuse an inherited field's name** either — same
  no-shadowing rule, checked at the same pass.
- **`virtual`/`override` methods cannot have default-valued parameters** —
  the vtable slot has one fixed signature.
- Vtable slot numbers are inherited: an `override` reuses its parent's slot
  (`src/check.c:371`); a fresh `virtual` method claims the next free one
  (`src/check.c:374`). Slots accumulate down the inheritance chain, so a
  three-level hierarchy's leaf class carries every ancestor's virtual slots.

Inheritance is **single** — one `extends`, no interfaces or traits to
declare. See §7 for how Vyto gets interface-like polymorphism (`for-in`)
without either.

## 4. `super`

Two distinct forms, both resolved statically (not through the vtable) so
neither one can recurse back into an override:

**`super.init(...)`** — only inside your own `init`, calls the parent's
constructor first. Required before touching inherited fields:

```vyto
fn init(r: float) { super.init("circle"); this.r = r; }
```

**`super.<method>(...)`** — inside *any* method, calls the base class's
implementation of that name directly, letting an `override` extend the base
behavior instead of copying its body (`src/check.c:1298-1321`):

```vyto
override fn area(): float {
    return super.area() + extra;   // base implementation, then extend it
}
```

Both require a base class that actually defines the target (`"class has no
base class"` / `"base class has no method '%s'"`), and neither is currently
usable inside a closure — call the base method outside it and pass the
result in instead (`src/check.c:1304-1306`).

## 5. `builder` methods

A method declared `builder` always returns `this`, typed as the **receiver's
concrete type at the call site** — not the declaring class — so a chain
survives through a subclass:

```vyto
class Node {
    tag: string;
    kids: Node[];
    fn init(tag: string) { this.tag = tag; this.kids = []; }
    builder fn add(k: Node) { this.kids.push(k); return this; }
}

class Panel extends Node {
    gap: int;
    builder fn spacing(g: int) { this.gap = g; return this; }
}

let p = new Panel("root").add(new Node("a")).spacing(8);   // .add is Node's,
                                                             // but the chain
                                                             // keeps Panel's type
```

(`examples/19_builder.vt`) A builder body may only `return this;` — no other
return statement is allowed (`src/check.c:2723-2725`) — and it can't declare
a return type or be `virtual`/`override` (`src/parse.c:826-862`).

## 6. `deinit` and teardown order

`deinit` runs deterministically, the moment the last strong reference drops
— there's no GC pause and no finalizer queue. The generated destructor runs,
in order (`src/emit.c:1951-1992`):

1. your `deinit` body,
2. `vt_release` on every reference-typed field (`vt_weak_drop` instead, for
   `weak` fields — no double-free, since a weak slot was never counted),
3. the parent class's generated destructor, recursively up the chain.

So a field is still valid for reading throughout your own `deinit` body, and
you never write field cleanup by hand — only whatever *other* side effect
(closing a file handle, logging) belongs in the body.

## 7. `weak` — breaking reference cycles

Vyto has **no cycle collector**: a strong reference cycle (parent ↔ child,
observer stored back onto its subject) leaks silently, with nothing to catch
it. `weak` is a real keyword that applies **only to class types**
(`src/check.c:240` — `'weak' applies only to class types`):

```vyto
class Widget {
    parent: weak Widget;   // back-reference: does not keep the parent alive
}
```

A `weak` field reads as `null` once its target is gone rather than dangling.
Any back-reference — child → parent, part → owner — should be `weak`; owning
references (a parent's list of children) stay strong. `lib/vyto/ui/core.vt`
follows this consistently — `parent: weak Widget`, `win: weak Window`,
`last_click_w: weak Widget` — and is worth reading as the reference example.
See [`docs/memory.md`](memory.md) §3 for the closure/bound-method variant of
this same cycle and the full refcounting model.

## 8. Checked downcasts

```vyto
let shapes: Shape[] = [new Circle(1.0)];
let c = shapes[0] as Circle;
```

`as` between related classes compiles to `vt_checked_cast`
(`runtime/vyto_rt.c:541`), which **panics** with a file:line message if the
instance isn't actually a `Circle` or subclass — it's a real runtime check,
not a reinterpret cast. Upcasting (subclass → ancestor) is free — a plain C
pointer cast, no check needed, since it can't fail.

## 9. Duck-typed iteration: `len()` / `at()`

There's no iterator interface to implement. A class with `fn len(): int` and
`fn at(i: int): T` can be used directly in `for (let x in c)` — the compiler
builds the equivalent index loop for you, calling `at` virtually if you
declared it that way:

```vyto
class Steps {
    n: int;
    fn init(n: int) { this.n = n; }
    fn len(): int { return this.n; }
    virtual fn at(i: int): int { return i; }
}

class Evens extends Steps {
    override fn at(i: int): int { return i * 2; }
}

fn describe(s: Steps): string {
    let out = "";
    for (let v in s) { out = out + str(v) + " "; }
    return out;
}
```

(`examples/93_iterable.vt`) `len()` must take no arguments and return `int`;
`at()` must take one `int` and return a non-`void` value; neither may be a
`builder` (`src/check.c:2518-2537`). The container is evaluated once and
held for the whole loop, but `len()` is **re-read on every iteration**, so a
loop body that grows the container keeps going — that's the hand-written
index loop's actual behaviour, not a special case.

This is deliberately name-based, not a declared trait, so a class written
before this feature existed becomes iterable retroactively just by having
the right two method names.

## 10. Generic classes

```vyto
class Box<T> {
    val: T;
    fn get(): T { return this.val; }
}
```

Monomorphized like generic structs and functions (see
[`docs/types.md`](types.md) §10) — one restriction specific to classes: a
generic class's base, if it has one, **must be non-generic**
(`src/check.c:1027-1028`, `"generic base classes are not supported yet"`).
`class Derived<T> extends Base<T>` doesn't work yet; `class Derived<T>
extends Base` (a concrete, non-generic `Base`) does.

## 11. Arena-allocated instances

`new@name ClassName(...)` (or bare `new` inside an `arena { }` block)
bump-allocates the instance in that lexical region instead of the normal
refcounted heap; the whole region is freed in one shot when its block ends,
with no per-object destructor teardown:

```vyto
class Node { value: int; next: Node; }

arena {
    let n = new Node();      // bump-allocated in this arena
    n.value = 42;
}                             // freed here, in one shot
```

(`examples/70_arena.vt`) This is an MVP safety story, not a general escape
hatch: an arena instance's `init` must be the default (no custom `init` yet,
`src/check.c:2395-2396`), and every field down the inheritance chain must be
a scalar or another class reference — no `string`, array, `Map`, or `fn`
field, since those are independently heap-managed and an arena's bulk free
has no way to release them (`src/check.c:2397-2405`). Reach for this only for
bulk allocation — large counts of short-lived, uniformly-scoped objects,
where per-object refcount teardown is the actual cost you're trying to
avoid — everyday classes should just use `new`. See
[`docs/memory.md`](memory.md) §4 for named/nested arenas, the exact
compile-time escape checks, and §5 for the bulk-allocation trap arenas are
meant to solve.

## 12. What classes are *not* for

FFI structs — a Vyto type whose layout matches a C struct exactly — are
`struct`, always, even if you'd reach for a class in ordinary code. See
[`docs/native-bindings.md`](native-bindings.md): `vytobind` only ever
generates `struct`s from C headers, never classes, because classes carry a
refcount header and vtable pointer that a C struct doesn't have room for.
