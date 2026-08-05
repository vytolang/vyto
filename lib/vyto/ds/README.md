# vyto/ds — trees, graphs and index structures

The structures whose elements are naturally *indices* rather than values: a
vertex is an int, a Trie payload is an int, a Fenwick position is an int.
Everything here is backed by flat `int[]`s with no object per node.

```vyto
import { trie, dsu, fenwick, segtree_min, bloom_for, graph, intervalTree } from "vyto/ds";

let t = trie();          t.put("carbon", 1);  t.longest_prefix("carbonate");  // 6
let d = dsu(n);          d.union(a, b);       d.same(a, b);
let f = fenwick(n);      f.add(i, delta);     f.range(lo, hi);
let s = segtree_min(n);  s.set(i, v);         s.query(lo, hi);
let b = bloom_for(1000000, 0.01);             b.add_str(url);
let g = graph(n);        g.add_edge(u, v, w); g.dijkstra(src);
let iv = intervalTree(); iv.add(10, 20, id);  iv.build();  iv.stab(15);
```

Pure Vyto, no native code, dependencies only on `vyto/hash` and `vyto/coll` —
so the whole package cross-compiles unchanged and builds freestanding.

## Modules

| Module | What it gives you |
|---|---|
| `ds/trie` | `Trie` — prefix tree over byte strings with an `int` payload. `has_prefix`, `keys_with_prefix`, `longest_prefix` |
| `ds/dsu` | `Dsu` — union-find with path halving and union by rank. `union`, `same`, `groups`, `labels` |
| `ds/fenwick` | `Fenwick`, `FenwickF` — prefix sums under mutation, plus `lower_bound` for weighted lookup |
| `ds/segtree` | `SegTree` — any associative range query with updates. Factories for sum/min/max/or/and |
| `ds/bloom` | `Bloom` — probabilistic membership. `bloom_for(n, p)` sizes it from what you know |
| `ds/skiplist` | `SkipList<T>` — an ordered set with O(log n) insert |
| `ds/skipmap` | `SkipMap<K,V>` — an ordered map. `ceiling_key`/`floor_key` answer "what is next at or after k", which no hash table can |
| `ds/graph` | `Graph` — directed weighted graph. `bfs`, `dfs`, `topo_sort`, `dijkstra`, `components` |
| `ds/interval` | `IntervalTree` — which ranges cover this point, or overlap this range |

`import { … } from "vyto/ds"` re-exports all of it.

## How this differs from `vyto/coll`

`vyto/coll` holds **values**: you put a `Task` into a `Deque` and get the `Task`
back. Here the elements are **indices**, and the payload — if there is one — is
an `int` you look up somewhere else.

That is not a stylistic split. It is `CLAUDE.md`'s bulk-data rule: a structure
built one object per element pays a header, a refcount lifetime and a scattered
cache line per element, and these are exactly the structures that get large. It
is also what makes them safe under reference counting — a graph of objects leaks
the moment it has a cycle and nothing detects it, while a graph of integers
cannot form one at all.

To attach real objects, keep them in a `vyto/coll` `Slab` (or any parallel
array) and use the id as the index. That is the pattern all the way down.

## Two conventions the whole directory follows

**Payloads are `int`.** A Trie value, a graph vertex, an interval id. For an
object, store a `Slab` handle — which is also an int, and which cannot dangle.

**Only `SkipList<T>` and `SkipMap<K,V>` are generic**, because they are the only
ones whose elements are not naturally indices. Everything else needs no type
parameter, no `zero`, and no instantiation at all.

## Things worth knowing before you use these

> **`Trie` is not one object per node.** The textbook version allocates a node
> per character with a 256-entry child array, which for any real dictionary is
> both an enormous allocation count and mostly empty space. Here the tree is
> five flat `int[]`s with a first-child / next-sibling chain, so a node costs
> five ints no matter how many children it has and the refcount never sees any
> of it.

> **`Trie.remove` unmarks rather than prunes.** The nodes stay, and the space is
> reclaimed by the next insert down the same path. Pruning would need a refcount
> per node, which is the thing this structure exists to avoid.

> **`Fenwick` is for sums; `SegTree` is for everything else.** A Fenwick answers
> a range by subtracting two prefixes, so it needs an invertible operation. A
> segment tree does not, which is why it can do min, max, gcd and or — at about
> twice the memory. Reach for Fenwick first when it fits.

> **`SegTree.query` folds the left and right halves separately.** `combine` is
> only required to be associative, not commutative, and folding into one
> accumulator would silently reorder the operands. Correct for min; wrong for a
> combine that keeps its left argument.

> **`Bloom` has no `remove`, and that is not an omission.** Clearing a bit would
> clear it for every other item that happened to set it, turning a false
> positive into a false negative and breaking the one guarantee the structure
> makes. `maybe()` false means *definitely not present*; `maybe()` true means
> *probably*. Use it only as a cheap filter in front of a real check, never as
> the answer.

> **Check `Bloom.fill_ratio()`, not `len()`.** Past about 0.5 the filter is
> saturated and its false-positive rate is far above what it was sized for,
> whatever the item count says.

> **`Graph.topo_sort` returns an EMPTY array on a cycle**, not a partial order.
> A caller that ignored the length would otherwise build things in the wrong
> order and never find out.

> **`Graph.dijkstra` panics on a negative weight.** Dijkstra's entire argument is
> that a settled vertex can never improve, which negative weights break. A
> quietly wrong shortest path is worse than a stop. Distances come back as `INF`
> (2^62, not INT64_MAX, so adding a weight to it cannot overflow) where
> unreachable.

> **`IntervalTree` must be `build()`ed after the last `add()`.** Querying a stale
> index panics rather than answering from it — a range structure that silently
> misses a match is worse than one that stops. Ranges are half-open `[lo, hi)`,
> so `stab(20)` does not match `[10, 20)`.

> **`SkipList` and `SkipMap` are for interleaved inserts and queries.** For data
> loaded once and then only read, a sorted array with `binary_search` from
> `vyto/util/sort` is contiguous and strictly better. A skip structure wins when
> an insert would otherwise shift half an array. Positional access is O(i), so
> `for-in` over a `SkipList` is fine but indexing in a loop is not — use
> `toArray()`, or `keys_in_order()` / `values_in_order()` on a `SkipMap`.

> **`SkipMap` is what you reach for when the question is about *adjacency*.**
> `ceiling_key(t)` — "the next scheduled event at or after t" — and
> `floor_key(t)` — "the last checkpoint before t" — have no answer in a hash
> table, which has no notion of neighbours. If you only ever look up exact keys,
> `vyto/coll`'s `HashMap` is faster and simpler.

> **`SkipMap` deliberately has no `at(i)`, so `for-in` does not work on it.** An
> element is a pair, Vyto has no tuples, and "the i'th thing" would have to
> silently pick keys or values. Iterate `keys_in_order()` and look up — the same
> decision `HashMap` and the builtin `Map` make. `key_at`/`value_at` exist for
> genuine positional access and are O(i).
