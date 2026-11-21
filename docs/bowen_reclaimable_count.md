# Spec: incremental reclaimable count in RadixTree

For Bowen. Written 2026-11-21 on branch `yingtao/experiments`.

## Why

Before an ADMIT evicts anything, the manager must know how many blocks eviction could
free ("admission checks free plus reclaimable capacity first"). Today
`KVCacheManager::reclaimable_blocks()` computes that with a scan over every block ID.
With sharing on the pool is normally full of cached blocks, so almost every ADMIT takes
this path. In the sweep the ADMIT median grows with capacity, and the scan is the likely
cause:

| Workload (100k requests) | Capacity 256 | Capacity 1,024 | Capacity 4,096 | Sharing off |
|---|---|---|---|---|
| Unique prompts | 9.2 us | 16.3 us | 29.6 us | 3.3-4.7 us |
| Shared system prefix | 8.3 us | 11.7 us | 22.9 us | 4.8-6.1 us |
| Multi-turn | 8.3 us | 11.5 us | 22.4 us | 2.7-3.5 us |

The goal is a count the tree keeps up to date, read in O(1). The manager keeps its scan
until this lands; then we rerun the sweep and compare.

## Definition

For an indexed block `b`:

- **held**: `pool.references(b) > 1`, i.e. at least one request holds it besides the
  index.
- **stuck**: `b` is held, or at least one child of `b` is stuck. A stuck block can never
  become an evictable leaf while the current holders stay.
- **reclaimable**: indexed and not stuck. Equivalently: no block in the subtree rooted
  at `b` (including `b`) is held.

`reclaimable_count()` is the number of reclaimable blocks. It must equal what the
manager's scan returns: `size()` minus the number of blocks that are held or have a held
descendant.

## Interface

Additions to `RadixTree`; nothing existing changes its signature.

```cpp
// Number of indexed blocks that eviction could free right now: blocks that no
// request holds and none of whose descendants a request holds. O(1).
std::size_t reclaimable_count() const noexcept;

// Tells the tree that the reference count of `block` may have changed. Reads
// pool.references(block) and updates the count. No-op if the block is not
// indexed or its held state did not change. Does not change any reference.
void refresh(BlockId block);
```

`refresh` is needed because the tree cannot see reference changes: `BlockTable`
retains and releases blocks directly on the pool. The pool is Bohan's and has no
observer hook, and adding one would put a callback on every retain and release. The
manager already knows every point where an indexed block's count changes (it reports the
same points to the policy), so it will call `refresh` there.

The count is correct only if the caller refreshes after every such change. That is the
caller's contract, the same way `set_evictable` is for the policy.

## Suggested state

Per node:

```cpp
bool held = false;              // pool.references(block) > 1 at the last insert/refresh
std::size_t stuck_children = 0; // children that are stuck
// stuck(node) == node.held || node.stuck_children > 0
```

Per tree: `std::size_t stuck_count_ = 0;` and
`reclaimable_count() == index_.size() - stuck_count_`.

One helper does all the propagation. When a node's stuck state flips:

- false to true: `++stuck_count_`; if it has a parent block, remember whether the parent
  was stuck, `++parent.stuck_children`, and if the parent was not stuck, repeat for the
  parent.
- true to false: the mirror image with `--`.

The walk stops at the first ancestor whose stuck state does not change.

## Which events update it

| Event | Update |
|---|---|
| `insert(prefix, block)` succeeds | New leaf. After the retain, `held = references(block) > 1` (normally true: the inserting request holds it). If held, the node becomes stuck and propagates up. This is also how a reclaimable parent stops being reclaimable when it gains a held child. |
| `insert` returns false or throws | Nothing. |
| `erase(block)` succeeds | Before unlinking: if the node is stuck, flip it to not stuck (propagates up), so the parent's `stuck_children` and `stuck_count_` no longer include it. Then remove it. A reclaimable leaf just leaves `size()` and the count each one smaller. |
| `refresh(block)` | `now = references(block) > 1`. If `now != held`, store it and flip the node's stuck state if it changed (`held` alone flips it only when `stuck_children == 0`). |
| Destructor, `release_all`, move | Reset or move `stuck_count_` with the nodes. |
| `match`, `contains`, `is_leaf`, `is_evictable`, `parent` | Nothing. |
| Pool events (`retain`, `release`, `allocate`, `clone`, `append`) | Nothing directly: the tree is not notified. A retain or release of an indexed block must be followed by `refresh(block)` from the caller. |

Where the manager will call `refresh` (my change, after your API lands):

- ADMIT: for each matched block after `BlockTable::from_prefix`, before the capacity
  check; and again for each of them if the ADMIT is rejected and the pins are dropped.
- RELEASE: for each block the request held, after its table is destroyed.
- Not needed: FORK with sharing on (the source already holds every block, so no count
  crosses between 1 and 2), copy-on-write (partial blocks are never indexed), and
  `evict_one` (it only calls `erase`).

## Cost

- `reclaimable_count()`: O(1).
- `insert`, `erase`, `refresh`: O(1) plus the propagation walk. The walk is O(depth) in
  the worst case (depth = prefix length in blocks), but it only continues while an
  ancestor's stuck state actually changes. A request pins a root-to-node path and later
  releases the same path, so pinning or releasing a path of k blocks costs O(k) in
  total: O(1) amortized per block. Please state both bounds in the README complexity
  section; "O(1)" alone would be wrong for a single deep update.
- Space: one bool and one counter per node.

## Invariants

Add to `RadixTree::check_invariants()`:

1. For every node, `stuck_children` equals the number of its children with
   `held || stuck_children > 0`.
2. `stuck_count_` equals the number of nodes with `held || stuck_children > 0`.
3. For every node, `held == (pool.references(block) > 1)`. This one holds only when the
   caller has kept its side of the `refresh` contract. If you would rather not have
   your own `check_invariants` depend on the caller, expose it as a separate
   `bool check_reclaimable() const` and I will call that from the manager.

Properties that follow and are worth asserting in tests:

4. `is_evictable(b)` implies `b` is reclaimable.
5. `reclaimable_count() > 0` exactly when some block is `is_evictable`.
6. Each successful `evict_one` lowers `reclaimable_count()` by exactly 1, and
   `reclaimable_count()` consecutive calls all succeed (this is the cascade).

## Test idea

1. **Hand cases** on a small tree, checking `reclaimable_count()` after each step:
   - Chain A -> B -> C, all cached: 3. Pin C: 0. Unpin C, pin B only: 1 (C). Pin A
     only: 2 (B and C).
   - Branch: A with children B and C, all cached: 3. Pin B: 1 (C). Evict C: 0. Unpin B:
     2.
   - A cached leaf gains a held child through `insert`: the count drops by one.
   - Erase a held leaf whose parent is cached: the parent becomes reclaimable.
2. **Seeded random test against a brute-force oracle.** Random `insert`, `erase`, pin
   (`BlockTable::from_prefix` on a random indexed path, then `refresh`) and unpin
   (destroy a table, then `refresh`). After every operation compare
   `reclaimable_count()` with a scan that marks every held block and its ancestors, as
   `KVCacheManager::reclaimable_blocks()` does today. Your existing 30,000-step radix
   test is a natural place.
3. **Cascade:** after any state, call `evict_one` until it returns nothing; the number
   of evictions must equal the `reclaimable_count()` read before the loop.

On my side: `KVCacheManager::check_invariants()` will compare `tree.reclaimable_count()`
with the current scan, which stays as a debug-only oracle. The manager's 24,000 seeded
operations then test the count and my `refresh` calls together.

## Before/after comparison

Same traces, same machine, `scripts/run_experiments.sh`; compare the ADMIT median and
p99 per capacity with sharing on. Expected: the ADMIT median stops growing with
capacity. Counters (hits, evictions, admitted) must be identical before and after; any
difference means the count disagrees with the scan.
