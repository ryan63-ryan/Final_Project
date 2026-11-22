# Radix prefix index and eviction policies

**Author: Bowen Deng.** `RadixTree` prefix index and the LRU/FIFO eviction policies.

Everything below the headings is Bowen's text, verbatim, as he added it to the project
README (commits `f646b7c`, "Add radix prefix index (RadixTree) with tests", and
`b452f87`, "Implement LRU and FIFO eviction policies"). It was moved here when the
README was rewritten for submission. Where he extended one of Bohan's paragraphs, only
his sentences are quoted, under the heading of the paragraph they came from; the
surrounding text is in [storage_module.md](storage_module.md). The headings are new.
The last sentence of "Radix index" (about the heap comparator) is Bohan's; it is kept
where Bowen placed it in the README.

The first sections describe the index with one node per block. The last section,
"Compressed edges", is the text he added with commit `ff7aa67` ("Compress radix edges
and split at block boundaries"), also verbatim; where it gives a newer version of an
earlier sentence, the newer one is current.

## API (rows added to the "Storage API" table)

| Component | Interface and behavior |
|---|---|
| `RadixTree` | Prefix index over full blocks, one node per block; `match`, `insert`, `erase`, `contains`, `is_leaf`, `is_evictable`, `parent`. Holds one pool reference per indexed block. |
| `LRUPolicy`, `FIFOPolicy` | `EvictionPolicy` implementations on `MinHeap`; `on_insert`, `on_access`, `on_erase`, `set_evictable`, `victim`. `evict_one(tree, policy)` performs one eviction. |

## Radix index

`RadixTree::match` returns the longest run of cached full blocks at the start of a
token sequence and changes no counts; pin the result with `BlockTable::from_prefix`.
`insert` requires a prefix whose length is a positive multiple of the block size, a
full unindexed block and an indexed parent prefix (otherwise it throws without
changes); it retains the block, or returns false if the prefix is already indexed,
in which case the caller's block stays private. `erase` releases only leaves, so
evicting a leaf can turn its parent into a leaf. Destroying the tree releases all of
its references. The heap comparator must not throw and priorities must support
non-throwing moves.

## Eviction policies

Each policy gives every tracked block a unique rank from a monotonic clock and keeps
only evictable blocks in its heap; the smallest rank is the victim. Both rank a block
by insertion; only LRU re-ranks it on `on_access`. Tracked blocks start not evictable.
The caller reports eligibility with `set_evictable(id, tree.is_evictable(id))`, where
`is_evictable` means an indexed leaf whose only reference is the index's own, i.e. no
request holds it. `evict_one` erases the victim from the tree and the policy, then
re-checks the parent, which may have just become a leaf. It throws `logic_error`
without changes if the victim is not `is_evictable`, so a stale mark cannot evict a
held block or an inner node. `make_policy`/`PolicyKind` selection is left to the
manager integration.

## Validation (added to "Validation and complexity")

Added to the list of seeded tests:

> 30,000 radix steps against a map of indexed prefixes, 36,000
> LRU/FIFO steps (with request pins) against a scan of eligible blocks and their ranks

Added to the platform paragraph:

> The radix index and eviction policies were added
> and checked on Ubuntu 22.04 (WSL2) with GCC 11.4.0 and CMake 3.22.1: `make test` with
> `-Werror`, `make sanitize`, the 12 demo cases, and CMake Release and sanitizer `ctest`
> all passed. These checks cover storage, the index and the policies, not the full
> manager or a process-wide leak audit.

## Complexity (added to "Validation and complexity")

Symbols as in [storage_module.md](storage_module.md): C pool capacity, B tokens/block,
P bytes/token, L blocks/request, H heap entries.

A radix match or insert over k blocks costs O(kB log F) token comparisons, where F is
the largest number of children of one node; contains/is_leaf/parent/erase use an
expected O(1) block lookup (plus O(1) amortized map erase). The index uses O(NB)
token space for N indexed blocks. With T tracked and E evictable blocks, a policy uses
O(T) space; insert/access/erase/set_evictable cost O(log E) heap work plus expected O(1)
hash lookups, and victim is O(1). `evict_one` adds a radix erase.

## Compressed edges (added in `ff7aa67`)

Updated row of the "Storage API" table:

| Component | Interface and behavior |
|---|---|
| `RadixTree` | Prefix index over full blocks with compressed edges (runs of whole blocks, split at block boundaries); per-block `match`, `insert`, `erase`, `contains`, `is_leaf`, `is_evictable`, `parent`, plus `node_count`. Holds one pool reference per indexed block. |

Inside the tree, an edge holds a run of k >= 1 full blocks and their k*B tokens,
and siblings are keyed by their first block's tokens. The interface stays per block,
so callers see the same results as with one node per block. `match` compares edges
block by block and also returns the blocks matched before a mismatch inside an edge.
`insert` below the last block of a childless edge extends that edge; below a block in
the middle of an edge it splits the edge after that block: the tail keeps its block
IDs, references and children, and the new block becomes the tail's sibling. A split
allocates everything first and then commits without allocating, so an allocation
failure leaves the tree, its index and all reference counts unchanged. `erase` removes
one block, the last of a childless edge, and the node once its edge is empty, so
eviction still frees one physical block per `evict_one`, and `parent` of the next
block on the edge becomes the next leaf.

**Limitation:** nodes are not merged after an erase. When eviction leaves a node with
a single child, the two edges stay separate, so compression is not maximal after
eviction. Results and complexity bounds are unaffected; only `node_count` is higher
than a fully compressed tree's. Merging would allocate inside `erase`, which the
eviction path relies on not to throw.

### Validation

Added to the list of seeded tests:

> 30,000 radix steps against a map of indexed prefixes (prefixes
> up to 6 blocks, so edges are extended, split and shortened; invariants after every
> step), hand-built compressed-edge cases (matches ending inside an edge, splits after
> the first and a middle block, split tails keeping children and pinned blocks), 36,000
> LRU/FIFO steps (with request pins) against a scan of eligible blocks and their ranks,
> compressed-edge eviction one block at a time under both policies, and
> `radix_fault_tests`, which fails every allocation of an edge extension, a new edge and
> two kinds of split in turn and checks that nothing changed or leaked,

Updated platform sentences:

> The radix index, eviction policies and compressed
> edges were added and checked on Ubuntu 22.04 (WSL2) with GCC 11.4.0 and CMake 3.22.1:
> `make test` with `-Werror`, `make sanitize`, the 12 demo cases, and CMake Release and
> sanitizer `ctest` all passed. Compressing edges left the driver's output on every
> golden trace byte-identical.

### Complexity

A radix match or insert walking k blocks over e edges (e <= k) costs O(kB + eB log F)
token comparisons, where F is the largest number of children of one node. Extending an
edge is amortized O(B); a new edge adds O(B log F); a split also copies the s tail
blocks and their tokens and re-points the tail's c children: O(sB + c).
contains/is_leaf/parent/erase use an expected O(1) block lookup (erase is amortized
O(1), plus a map erase when an edge empties). The index uses O(NB) token space and at
most N nodes for N indexed blocks.
