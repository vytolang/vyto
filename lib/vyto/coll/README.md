# vyto/coll — general-purpose containers

The containers a working program reaches for and, until now, had to hand-roll:
a queue that dequeues in O(1), a priority queue, a map keyed on something other
than a string, a cache that evicts, a set of a million flags that costs 125 KB.

```vyto
import { deque_int, heap_int_min, hashmap_str_int, lru_str_str,
         bitset, Slab } from "vyto/coll";

let q = deque_int(64);          q.push(1); q.pop_front();
let h = heap_int_min(64);       h.push(5); h.peek();
let m = hashmap_str_int(64);    m.put("a", 1); m.get_or("b", 0);
let c = lru_str_str(1000);      c.put(url, body);      // evicts when full
let seen = bitset(1000000);     seen.set(id);
let s = new Slab<Node>(1024, null);   let h = s.alloc(n);   // h is an int
```

Pure Vyto, no native code, no dependency outside `vyto/hash` — so the whole
package cross-compiles unchanged and builds freestanding.

## Modules

| Module | What it gives you |
|---|---|
| `coll/deque` | `Deque<T>` — double-ended queue over a ring. O(1) at both ends, one allocation, grows by doubling |
| `coll/ring` | `RingBuffer<T>` — fixed capacity, overwrites the oldest, counts what it dropped, never allocates after `init` |
| `coll/heap` | `Heap<T>` — binary heap / priority queue. `heapify` in O(n), plus `push_pop` and `replace_top` for top-k loops |
| `coll/hashmap` | `HashMap<K,V>` — open-addressed table with keys of any type |
| `coll/hashset` | `HashSet<T>` — the same table without values |
| `coll/bitset` | `BitSet` — a bit per small integer, with `next_set` for iteration proportional to the count |
| `coll/lru` | `LRU<K,V>` — fixed-capacity cache with least-recently-used eviction and hit/miss counters |
| `coll/slab` | `Slab<T>` — index-based arena. Handles are ints, so a graph of them cannot form a reference cycle |
| `coll/util` | `next_pow2`, `popcount64`, `ctz64` |

`import { … } from "vyto/coll"` re-exports all of it; importing the individual
module pulls in less.

## Two conventions the whole directory follows

**Every generic container takes a `zero: T`.** v1 generics have no bounds and no
notion of a default value for `T`, so a container that writes at an index rather
than pushing cannot invent its own empty slots — and it needs real slots.
`zero` is what a vacated slot is set to: `null` for a class or string, `0` for a
number, `false` for a bool. It is not decoration: storing it is what lets `pop()`
clear the slot it left, and clearing that slot is what stops a popped object
living on until the ring wraps around to overwrite it. Every container ships
`*_int` / `*_str` factories so the common cases never spell it.

**Ordering and hashing are closure fields, never inferred.** There is no `Ord`
and no `Hashable`. `Heap` takes a `cmp`, `HashMap` and `HashSet` take a `hash`
and an `eq`. This is the same trade `vyto/util/sort` and `vyto/reactive`'s
`Signal` already make, and it is precisely what lets a container work with your
own types without the language growing a trait system.

## Things worth knowing before you use these

> **A `T[]` used as a queue is O(n) per dequeue.** `remove_at(0)` shifts every
> remaining element down, so draining one is O(n²). That is what `Deque` is for,
> and it is not a hypothetical — `vyto/reactive`'s flush loop and
> `vyto/os/worker`'s backlog both do exactly that today.

> **`Deque` grows, `RingBuffer` does not.** Pick by whether the input is bounded.
> A scrollback buffer, the last N frame times, a rolling sensor window: those
> want `RingBuffer`, which drops the oldest instead of allocating, and tells you
> how many it dropped so the UI can say "1,204 earlier lines".

> **The builtin `Map` is string-keyed and that is a language limit**, not a
> library choice — `Map<string, V>` is enforced in the parser. `HashMap` is how
> you key on an int, a struct id, a composite, or your own class.

> **Deletion is backward-shift, not tombstones.** On removal the following run is
> walked and any element whose ideal slot lies at or before the hole is moved
> back. A tombstoned table is easier to write and degrades silently under churn
> until every lookup scans the whole run; this one does not. The fixture removes
> half of a 200-entry map and then checks all 200 keys, and separately does the
> same on a table where every key deliberately hashes to 0.

> **Hash your keys with `vyto/hash`, and use `siphash24` when they come from the
> network.** An attacker who chooses your keys can, against any unkeyed hash,
> pick thousands that collide and turn every lookup into a linear scan. The
> factories here use `hash_int`/`hash_str`, which are fine for keys you generate
> and wrong for keys a request body supplies.

> **`Slab` is about memory safety, not speed.** Reference counting leaks a strong
> cycle and nothing detects it; `weak` breaks a tree but not a graph. In a Slab,
> nodes refer to each other by `int` handle — and an integer holds nothing alive,
> so a cycle of handles is not a cycle. Handles carry a generation counter, so
> freeing and reallocating a slot invalidates the old handle instead of silently
> aliasing it: a use-after-free panics with a message rather than returning a
> neighbour's object. Handle `0` (`NIL`) is never valid, so a zeroed field reads
> as "no handle" without a separate flag.

> **`LRU`'s recency list has no nodes.** A doubly-linked list of objects under
> reference counting is a leak by construction. Here `prev` and `next` are
> `int[]` over slot indices, so a cache hit moves four integers and touches no
> allocator. `peek()` and `has()` deliberately do *not* count as a use, so
> instrumentation cannot change what gets evicted.

> **`BitSet` is a bit per element; `HashSet<int>` is a slot per element.** For
> ids under a few million — a visited mark, a sieve, per-row selection — BitSet
> wins by two orders of magnitude and its set operations run 64 flags per
> instruction. Use `HashSet` when the elements are sparse, large, or not
> integers.

> **Iterating a `Heap` is not sorted.** `at(0)` is the minimum and the rest is
> only partially ordered — that is what a heap *is*. Use `drain()` when order
> matters; it is heapsort and it empties the heap.

> **A `Slab` has no `at(i)`, so `for-in` does not work on it.** "The i'th live
> slot" would be a scan while reading like an index. Iterate `handles()` or
> `values()`. Everything else here has `len()`/`at(i)` and so iterates with
> `for-in` directly.
