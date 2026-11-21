# Integration notes: prefix index and eviction in KVCacheManager

Branch `integration`, 2026-11-21. Not pushed. Bowen's files (`radix_tree.*`,
`eviction_policy.*`, `radix_tests.cpp`, `eviction_tests.cpp`) are unchanged since the merge.

Every commit below passed `make test`, `make sanitize` (ASan + UBSan) and `ctest`
(9 of 9 tests at the end) on WSL2.

## 1. Commits

| Commit | Step | What it does |
|---|---|---|
| `b0dea97` | merge | Bowen's radix prefix index and LRU/FIFO eviction merged into `integration` |
| `4ef83a6` | 1 | Manager owns a `RadixTree` and an `EvictionPolicy`; move and destruction order; index-aware invariants |
| `324062c` | 2 | Full blocks are indexed on ADMIT and APPEND; missing ancestors are healed; policy notifications |
| `8fe0152` | 3 | ADMIT reuses the longest cached prefix (`match` + `from_prefix`); prefix-reuse trace test |
| `8ef9a05` | 4 | Tests for RELEASE notifications (tests only, see 2.4) |
| `e4e776f` | 5 | Eviction on ADMIT and APPEND with the reclaimable scan; eviction, heal, capacity and seeded tests |
| `8a2b11b` | 6 | `sharing=off` bypass tests; golden eviction traces for LRU, FIFO and the baseline |

## 2. What each step changed

### 2.1 Step 1: wiring and invariants

- Members are declared in the order `pool_`, `tree_`, `policy_`, `requests_`, so tables
  and the index release their references before the pool is destroyed.
- Move assignment clears `requests_`, then move-assigns `tree_` (which releases the old
  index's references into the old pool), then replaces `pool_`.
- The policy is created by a small factory inside `kv_cache_manager.cpp`
  (`PolicyKind::LRU` or `FIFO`).
- `check_invariants()` now requires, for every block,
  `references == request holders + (1 if indexed)`. It also calls the tree's and the
  policy's own checks, and compares the policy's evictable set with the tree: equal
  count, and `victim()` must be `tree.is_evictable`. With `sharing=off` the tree must be
  empty.
- New read-only accessors for tests and the demo: `indexed_blocks()`,
  `evictable_blocks()`, `is_indexed(block)`, `block_references(block)`.

### 2.2 Step 2: indexing on fill, healing, notifications

`index_full_blocks(state)` runs after ADMIT writes its blocks and after an APPEND that
fills the tail:

1. `covered = tree.match(tokens).size()`: levels already indexed, by this request's
   blocks or by an equal block of another request (then ours stays private).
2. For each level from `covered` to the last full block: `tree.insert(prefix, block)`,
   `policy.on_insert(block)`, and `policy.set_evictable(parent, false)`.

Starting at the first uncovered level, instead of only the newest block, is the heal:
`RadixTree::insert` throws if the parent prefix is not indexed, and the parent can have
been evicted while this request holds a private copy of it (section 5).

The `set_evictable(parent, false)` call is required: a cached leaf that gains a child is
no longer a leaf, and a stale "evictable" mark makes `evict_one` throw `logic_error`.

RELEASE: after the table is destroyed, every block the request held that is still
indexed gets `policy.set_evictable(block, tree.is_evictable(block))`. `freed` lists only
blocks whose last reference was this request, so indexed blocks stay cached.

### 2.3 Step 3: ADMIT prefix match

Order of operations in `admit`:

1. `hit = tree.match(prompt)`; `BlockTable::from_prefix(pool, hit)` pins the blocks.
2. Capacity check for the remaining `blocks_for(prompt) - hit.size()` blocks. On Reject
   the table is destroyed, which drops the pins; nothing else was touched.
3. `policy.on_access` for every matched block, `set_evictable(hit.back(), false)`.
4. Evict if needed (step 5), then write only the uncached tokens. The running prefix
   hash is advanced over the cached tokens without writing them.
5. Index the newly filled blocks.

`cached_tokens = hit.size() * tokens_per_block`; `uncached_tokens` is the rest.

### 2.4 Step 4: RELEASE notifications

The code landed in step 2, because the invariants fail as soon as one block is indexed
without it. The step 4 commit adds `release_tests`: two holders of the same blocks, a
holder of only the parent, a holder of the leaf, and a shared partial tail (never
indexed, freed by its last holder).

### 2.5 Step 5: eviction

- `reclaimable_blocks()`: indexed blocks that no request holds and none of whose
  descendants a request holds. It marks every held indexed block and its ancestors as
  stuck and returns `tree.size() - stuck`. O(capacity), and it includes parents that only
  become leaves by cascade. This matches the proposal: "admission checks free plus
  reclaimable capacity first".
- `has_room(needed)` runs the scan only when the free list alone is too short.
- `make_room(needed, result)` calls `evict_one(tree, policy)` until enough blocks are
  free, and records the victims in `result.evicted` and `stats.evictions`.
- ADMIT checks `has_room` after pinning its match, so a matched block and its ancestors
  are never evicted for the request that matched them.
- APPEND needs at most one block, so `policy.evictable_count() > 0` is enough: no scan.
- FORK with `sharing=on` needs no blocks. FORK with `sharing=off` cannot evict because
  the index is empty.
- The Reject message is now `needs 3 blocks, 1 free, 0 evictable`.

### 2.6 Step 6: sharing=off and golden traces

- With `sharing=off` the manager never calls `match` or `insert`: no cache hits, RELEASE
  frees full blocks immediately, nothing is evicted. `baseline_bypass_tests` checks this
  under both policy settings.
- `tests/golden/eviction.trace` runs through the driver with `eviction_lru.cfg`,
  `eviction_fifo.cfg` and `eviction_off.cfg`; the three `.expected` files are compared in
  `make test` and `ctest`. The proposal example (`example.expected`) did not change.

### 2.7 Pre-existing expectations that changed

Both in `tests/manager_tests.cpp`, both in step 3:

| Test | Old | New | Reason |
|---|---|---|---|
| `cow_tests`, after `ADMIT direct 10 11 12 13` | `used_blocks == 4`, `peak_used_blocks == 4` | `used_blocks == 2`, `peak_used_blocks == 3` | `direct` now reuses r2's two full blocks instead of allocating two more |
| `failure_tests`, prompt of `big` | `{1,2,3,4,5}` | `{9,2,3,4,5}` | The old prompt shares block `[1,2]` with r1, needs only 2 blocks and is correctly admitted; the test wants a 3-block request with 2 free |

## 3. Tests added

| Test in `manager_tests.cpp` | Covers |
|---|---|
| `wiring_tests` | Both policies and both sharing modes construct with a consistent empty cache |
| `indexing_tests` | Index on ADMIT and APPEND, duplicate fill stays private, parent stops being evictable when it gains a child, copy-on-write block is indexed |
| `prefix_reuse_tests` | Hand-calculated prefix-reuse trace (partial hit, full hit, no hit on a different first token, Reject with a partial hit, cache outliving its requests) |
| `release_tests` | RELEASE notifications |
| `eviction_trace_tests` | Hand-calculated LRU and FIFO trace of section 4 |
| `eviction_capacity_tests` | All-or-nothing Reject, matched prefix not evicted for its own request, eviction from APPEND |
| `heal_tests` | Trace of section 5 |
| `baseline_bypass_tests` | `sharing=off` |
| `random_tests` | 24,000 seeded operations (2 policies x 2 sharing modes x 20 seeds x 300 steps): invariants after every operation, every live request's bytes recomputed from its tokens, Reject changes nothing |

## 4. Hand-calculated LRU vs FIFO eviction trace

Config: `tokens_per_block=2`, `capacity=4`, `sharing=on`. Trace:
`tests/golden/eviction.trace`. Test: `eviction_trace_tests`.

Rules used in the calculation:

- References = requests holding the block + 1 if the block is indexed.
- Evictable = indexed leaf with references == 1.
- Rank = the policy clock value given at insert. LRU gives a block a new rank when a
  cache hit reuses it; FIFO does not (and does not advance the clock).
- Victim = the evictable block with the smallest rank.
- The free list is a stack: the block freed last is allocated first.

Notation: `[1,2] (2)` is a block holding tokens 1 2 with 2 references; `-` is a free
block. Ranks are internal to the policy: they are derived by hand from the rules above
and are not observable through the manager, so the tests check their consequences
(victims and block IDs), not the rank values themselves.

### 4.1 Steps 1-6: both policies store the same blocks

| # | Operation | Block 0 | Block 1 | Block 2 | Block 3 | Evictable | Ranks (LRU) | Ranks (FIFO) |
|---|---|---|---|---|---|---|---|---|
| 1 | ADMIT a 1 2 | [1,2] (2) | - | - | - | {} | 0:1 | 0:1 |
| 2 | ADMIT b 3 4 | [1,2] (2) | [3,4] (2) | - | - | {} | 0:1 1:2 | 0:1 1:2 |
| 3 | ADMIT c 5 6 | [1,2] (2) | [3,4] (2) | [5,6] (2) | - | {} | 0:1 1:2 2:3 | 0:1 1:2 2:3 |
| 4 | RELEASE a, b, c | [1,2] (1) | [3,4] (1) | [5,6] (1) | - | {0,1,2} | 0:1 1:2 2:3 | 0:1 1:2 2:3 |
| 5 | ADMIT d 1 2 (hit [0], cached=2) | [1,2] (2) | [3,4] (1) | [5,6] (1) | - | {1,2} | **0:4** 1:2 2:3 | 0:1 1:2 2:3 |
| 6 | RELEASE d | [1,2] (1) | [3,4] (1) | [5,6] (1) | - | {0,1,2} | 0:4 1:2 2:3 | 0:1 1:2 2:3 |

After step 6 the next victim is block 1 under LRU (rank 2) and block 0 under FIFO
(rank 1).

### 4.2 Steps 7-10 under LRU

| # | Operation | Victim(s) | Block 0 | Block 1 | Block 2 | Block 3 | Evictable after | Ranks after |
|---|---|---|---|---|---|---|---|---|
| 7 | ADMIT e 7 8 9 10 (needs 2, 1 free) | 1 | [1,2] (1) | [7,8] (2) | [5,6] (1) | [9,10] (2) | {0,2} | 0:4 1:5 2:3 3:6 |
| 8 | ADMIT f 1 2 (hit [0], cached=2) | none | [1,2] (2) | [7,8] (2) | [5,6] (1) | [9,10] (2) | {2} | 0:7 1:5 2:3 3:6 |
| 9 | RELEASE e | none | [1,2] (2) | [7,8] (1) | [5,6] (1) | [9,10] (1) | {2,3} | unchanged |
| 10 | ADMIT g 20 21 22 23 24 (needs 3, 0 free) | 2, 3, 1 | [1,2] (2) | [20,21] (2) | [24] (1) | [22,23] (2) | {} | 0:7 1:8 3:9 |

Driver output: `e blocks=[1,3] evicted=[1]`, `f blocks=[0] cached=2`,
`g blocks=[1,3,2] evicted=[2,3,1]`.

### 4.3 Steps 7-10 under FIFO

| # | Operation | Victim(s) | Block 0 | Block 1 | Block 2 | Block 3 | Evictable after | Ranks after |
|---|---|---|---|---|---|---|---|---|
| 7 | ADMIT e 7 8 9 10 (needs 2, 1 free) | 0 | [7,8] (2) | [3,4] (1) | [5,6] (1) | [9,10] (2) | {1,2} | 0:4 1:2 2:3 3:5 |
| 8 | ADMIT f 1 2 (miss, needs 1, 0 free) | 1 | [7,8] (2) | [1,2] (2) | [5,6] (1) | [9,10] (2) | {2} | 0:4 1:6 2:3 3:5 |
| 9 | RELEASE e | none | [7,8] (1) | [1,2] (2) | [5,6] (1) | [9,10] (1) | {2,3} | unchanged |
| 10 | ADMIT g 20 21 22 23 24 (needs 3, 0 free) | 2, 3, 0 | [20,21] (2) | [1,2] (2) | [24] (1) | [22,23] (2) | {} | 0:7 1:6 3:8 |

Driver output: `e blocks=[0,3] evicted=[0]`, `f blocks=[1] cached=0 evicted=[1]`,
`g blocks=[0,3,2] evicted=[2,3,0]`.

### 4.4 Step 10 in detail (the cascade)

Before step 10 only two blocks are evictable, but three are reclaimable. The block
holding `[7,8]` has references == 1 but is not a leaf: its child `[9,10]` is cached
under it.

| Round | LRU: evictable (rank) | LRU victim | FIFO: evictable (rank) | FIFO victim |
|---|---|---|---|---|
| 1 | 2 (3), 3 (6) | 2 | 2 (3), 3 (5) | 2 |
| 2 | 3 (6) | 3 | 3 (5) | 3 |
| 3 | 1 (5), now a leaf | 1 | 0 (4), now a leaf | 0 |

A capacity check against `evictable_count()` alone (2) would have rejected g.

### 4.5 Totals

| | LRU | FIFO | sharing=off |
|---|---|---|---|
| Prompt tokens | 19 | 19 | 19 |
| Cached tokens | 4 | 2 | 0 |
| Uncached tokens | 15 | 17 | 19 |
| Evictions | 4 | 5 | 0 |
| Rejected | 0 | 0 | 0 |

Step 8 is the policy difference: LRU kept `[1,2]` because d had just reused it, FIFO
evicted it as the oldest insert and had to write it again.

Reproduce:

```sh
./build-make/kvcache --config tests/golden/eviction_lru.cfg  --trace tests/golden/eviction.trace
./build-make/kvcache --config tests/golden/eviction_fifo.cfg --trace tests/golden/eviction.trace
./build-make/kvcache --config tests/golden/eviction_off.cfg  --trace tests/golden/eviction.trace
```

## 5. Heal-scenario trace

Config: `tokens_per_block=4`, `capacity=3`, `sharing=on`, LRU. Test: `heal_tests`.

Two requests fill the same prefix independently, so one of them holds a private,
unindexed copy. The indexed copy is then evicted, and the request with the private copy
fills its next block, whose parent prefix is no longer in the index.

The number in parentheses is the reference count, as in section 4.

| # | Operation | Block 0 | Block 1 | Block 2 | Indexed | Evictable | Result |
|---|---|---|---|---|---|---|---|
| 1 | ADMIT r1 1 2 3 | [1,2,3] (1) | - | - | {} | {} | blocks=[0] |
| 2 | ADMIT r2 1 2 3 | [1,2,3] (1) | [1,2,3] (1) | - | {} | {} | blocks=[1]; partial blocks are not shared |
| 3 | APPEND r1 4 | [1,2,3,4] (2) | [1,2,3] (1) | - | {0} | {} | block 0 is full and indexed |
| 4 | APPEND r2 4 | [1,2,3,4] (2) | [1,2,3,4] (1) | - | {0} | {} | prefix already indexed by block 0: block 1 stays private |
| 5 | RELEASE r1 | [1,2,3,4] (1) | [1,2,3,4] (1) | - | {0} | {0} | freed=[]; block 0 stays cached |
| 6 | ADMIT x 9 | [1,2,3,4] (1) | [1,2,3,4] (1) | [9] (1) | {0} | {0} | blocks=[2]; pool is full |
| 7 | APPEND r2 5 | [5] (1) | [1,2,3,4] (1) | [9] (1) | {} | {} | evicted=[0]; r2 blocks=[1,0]; the index is empty |
| 8 | APPEND r2 6, APPEND r2 7 | [5,6,7] (1) | [1,2,3,4] (1) | [9] (1) | {} | {} | tail not full yet |
| 9 | APPEND r2 8 | [5,6,7,8] (2) | [1,2,3,4] (2) | [9] (1) | {1,0} | {} | heal: block 1 inserted for `[1,2,3,4]`, then block 0 under it |
| 10 | ADMIT y 1 2 3 4 5 6 7 8 | [5,6,7,8] (3) | [1,2,3,4] (3) | [9] (1) | {1,0} | {} | blocks=[1,0] cached=8 |

Step 9 is the heal. `tree.match([1..8])` covers 0 levels, so the manager inserts r2's
blocks from level 0 down. Inserting only the newly filled block 0 would throw
`std::invalid_argument("parent prefix is not indexed")` from `RadixTree::insert`.
Block 0 is the same physical block that was evicted in step 7; it now holds different
tokens.

## 6. Remaining work compared with the proposal

Today is Nov 21; the proposal's "integrate prefix sharing, fork, copy-on-write and
eviction; complete core tests" milestone ends Nov 22.

### 6.1 Deviations from the proposal text

| Proposal | Current state | Suggested action |
|---|---|---|
| Section 3 and 4: "radix edge splits", "compressed radix edges hold whole token chunks" | `RadixTree` has one node per block; edges are not compressed and never split | Decide with Bowen: implement compression, or state the simplification and its cost in the report. Section 4 lists edge splits as a course-mapping algorithm, so this needs an explicit decision |
| Section 5: "an independent vector model checks accepted requests' bytes" | Tests recompute bytes from tokens with `expected_bytes` and keep a `std::map` of tokens in `random_tests`; there is no separate `ReferenceCache` class. `kv_cache_manager.h` still mentions `ReferenceCache` in a comment | Either add the class or fix the comment and describe the test model as the reference |
| Section 5: "brute-force scans of small expected cache histories check whole-block prefix hits" | Not done. `random_tests` does not check `cached_tokens` against an independent model | Add a brute-force cache model to `random_tests` (needs the eviction order, so it must model LRU and FIFO) |

### 6.2 Done for the manager (proposal sections 3 and 5)

Whole-block prefix matching; fork and copy-on-write; LRU and FIFO eviction of unpinned
leaves; cascading eviction; free-plus-reclaimable admission with state preserved on
rejection; indexed full blocks stay cached; partial blocks freed at zero references;
empty prompts; exact block boundaries; fork divergence; parent-first release; exhausted
capacity; golden trace with expected output; hand-calculated capacity, rejection and
eviction traces; no-sharing baseline; seeded traces under ASan and UBSan.

### 6.3 Not started (proposal section 6, evaluation)

- Trace generators for unique prompts, shared system prefixes and multi-turn prefixes
  at 1,000, 10,000 and 100,000 requests. `scripts/` is empty.
- Experiment runs: sharing off/on and LRU/FIFO at capacities 256, 1,024 and 4,096 with
  16 tokens x 64 bytes per block, five runs per setting, median and interquartile range.
- Latency measurement separated from parsing, checks and driver overhead. The driver
  has no timing; `--csv` writes one row of final statistics only.
- Memory: peak RSS and explicit storage estimates.
- Plots and raw CSVs. `results/raw/` and `results/figures/` are empty.
- Time and space analysis for the manager operations.

### 6.4 Open items in the code

- `README.md` still says the `kvcache` entry point is a placeholder and does not
  describe the manager, the driver or the new golden traces.
- Two O(capacity) paths may show up in the latency results at capacity 4,096 under
  saturation: `reclaimable_blocks()` on every ADMIT that finds the free list short, and
  `stats()` (fragmentation scan).
- `index_full_blocks` copies the whole prefix for each block it inserts: O(L^2 / B)
  tokens for an L-token prompt.
- CLI error handling (missing files, malformed lines, invalid IDs) exists in the driver
  but was not reviewed or extended in this integration.
- `integration` is not merged into `main` and not pushed.

### 6.5 Optional requests for Bowen

None blocks the integration; the manager works around each one.

1. `RadixTree::reclaimable_count()`: indexed blocks with references == 1 and no
   descendant held by a request. Replaces the manager's O(capacity) scan.
2. A per-block query on the policy (`is_evictable(block)` or `tracked_count()`), so the
   manager's invariant check can compare sets instead of only the count and the victim.
3. Move `using Token` into `types.h`, as the comment in `radix_tree.h` suggests.
4. An `insert` that takes the parent block and one block of tokens, to avoid copying the
   whole prefix per indexed block.
