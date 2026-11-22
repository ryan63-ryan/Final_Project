# Paged KV-Cache Manager with Radix Prefix Sharing

EE538 Final Project, USC Fall 2026. C++17, Track B.

The project studies fixed-size KV storage, prefix sharing and capacity-limited
eviction using deterministic byte payloads rather than a running language model.
See the [proposal](EE538_Final_Project_Proposal.pdf) for the agreed scope.

| Member | Responsibility |
|---|---|
| Bohan Cui | Block pool/tables, reference counts, copy-on-write, indexed template heap |
| Bowen Deng | Radix prefix index, LRU/FIFO eviction |
| Yingtao Yang | Manager, CLI, reference model, workloads and experiment integration |

The storage library, radix prefix index, LRU/FIFO eviction policies and `block_demo`
run independently. The `kvcache` entry point is still a placeholder; the complete
manager and experiments require team integration.

## Build and test

Requires a C++17 compiler and Make; Python 3 is used only for extra input tests.

```sh
make test
./build-make/block_demo tests/golden/block_demo.in
make sanitize
python3 tests/demo_errors.py build-asan-make/block_demo
```

With CMake installed, the equivalent build is:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
(cd build && ctest --output-on-failure)
./build/block_demo tests/golden/block_demo.in
```

Use `-DKV_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug` in a separate CMake build directory
to enable AddressSanitizer and UndefinedBehaviorSanitizer.

## Storage API

| Component | Interface and behavior |
|---|---|
| `BlockPool` | Owns preallocated bytes; `allocate`, `clone`, `retain`, `release`, `read_token`. Exhaustion returns `nullopt`. |
| `BlockTable` | Maps a request to blocks; `append`, `fork`, `from_prefix`, `read_token`, `clear`. Capacity failure on one append returns false without changing stored state. |
| `MinHeap<T, Compare>` | Custom indexed heap; `push`, `top`, `pop`, `update`, `erase`. Lower priority wins, then lower key. |
| `RadixTree` | Prefix index over full blocks with compressed edges (runs of whole blocks, split at block boundaries); per-block `match`, `insert`, `erase`, `contains`, `is_leaf`, `is_evictable`, `parent`, plus `node_count`. Holds one pool reference per indexed block. |
| `LRUPolicy`, `FIFOPolicy` | `EvictionPolicy` implementations on `MinHeap`; `on_insert`, `on_access`, `on_erase`, `set_evictable`, `victim`. `evict_one(tree, policy)` performs one eviction. |

Construct the pool before tables/indexes and destroy it last. Each table entry
owns one reference; `clear` and destruction release it. IDs returned by `blocks()`
are borrowed, and freed IDs may be recycled. Pool copying/moving and table copying
are disabled; use `fork` to share or move a table to transfer its ownership.

Each payload contains exactly the configured bytes per token. An exclusive partial
tail is appended in place; a shared partial tail is copied first; a full tail needs
a new block. Full blocks are immutable. Invalid payloads/geometry throw
`invalid_argument`, invalid reads/IDs throw `out_of_range`, and prohibited writes
throw `logic_error`. Operations are single-threaded.

Only full blocks may enter the prefix index. The index must retain/release its own
references. With exactly one cache-owned reference per block, a count of one means
cache-only ownership; eviction must also respect leaf/ancestor constraints. Radix
edge splits must preserve ownership. The manager handles multi-token admission
prechecks and eviction rollback; single-append atomicity does not provide that
larger transaction.

`RadixTree::match` returns the longest run of cached full blocks at the start of a
token sequence and changes no counts; pin the result with `BlockTable::from_prefix`.
`insert` requires a prefix whose length is a positive multiple of the block size, a
full unindexed block and an indexed parent prefix (otherwise it throws without
changes); it retains the block, or returns false if the prefix is already indexed,
in which case the caller's block stays private. `erase` releases only leaves, so
evicting a leaf can turn its parent into a leaf. Destroying the tree releases all of
its references. The heap comparator must not throw and priorities must support
non-throwing moves.

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

## Validation and complexity

Tests check pool accounting, payload isolation, ownership, capacity failure, moves,
cached prefixes, heap invariants, the radix index and eviction. They include 27,000
seeded storage steps against independent payload vectors, 30,000 heap steps against a
scan-based reference, 30,000 radix steps against a map of indexed prefixes (prefixes
up to 6 blocks, so edges are extended, split and shortened; invariants after every
step), hand-built compressed-edge cases (matches ending inside an edge, splits after
the first and a middle block, split tails keeping children and pinned blocks), 36,000
LRU/FIFO steps (with request pins) against a scan of eligible blocks and their ranks,
compressed-edge eviction one block at a time under both policies, and
`radix_fault_tests`, which fails every allocation of an edge extension, a new edge and
two kinds of split in turn and checks that nothing changed or leaked,
16-token/64-byte storage, and a 4,097-entry heap with 64-bit priorities. Seeds are
538, 2026 and 17; random steps include no-op choices. Golden demo output and 12 extra
input/error cases are included. The demo reads a prompt length (0–7), its byte values,
then one append byte; it uses four blocks of two tokens and prints both request states.

Validated on macOS 14.3/arm64 with Apple Clang 15.0.0 and Make 3.81. Release uses
`-std=c++17 -O2 -Wall -Wextra -Wpedantic`; a fresh build also passed `-Werror`.
ASan/UBSan builds use `-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer`
and passed without reported errors. The radix index, eviction policies and compressed
edges were added and checked on Ubuntu 22.04 (WSL2) with GCC 11.4.0 and CMake 3.22.1:
`make test` with `-Werror`, `make sanitize`, the 12 demo cases, and CMake Release and
sanitizer `ctest` all passed. Compressing edges left the driver's output on every
golden trace byte-identical. These checks cover storage, the index and the policies, not the full
manager or a process-wide leak audit.

Let C be pool capacity, B tokens/block, P bytes/token, L blocks/request and H heap
entries. Pool construction costs O(CBP) time/space. Allocate zeroes a block and clone
copies it: O(BP). Retain/release are O(1); an exclusive append is O(P), COW O(BP).
Table growth can copy O(L) IDs, amortized O(1) per added ID. Fork/import/clear are O(L).
A radix match or insert walking k blocks over e edges (e <= k) costs O(kB + eB log F)
token comparisons, where F is the largest number of children of one node. Extending an
edge is amortized O(B); a new edge adds O(B log F); a split also copies the s tail
blocks and their tokens and re-points the tail's c children: O(sB + c).
contains/is_leaf/parent/erase use an expected O(1) block lookup (erase is amortized
O(1), plus a map erase when an edge empties). The index uses O(NB) token space and at
most N nodes for N indexed blocks. With T tracked and E evictable blocks, a policy uses
O(T) space; insert/access/erase/set_evictable cost O(log E) heap work plus expected O(1)
hash lookups, and victim is O(1). `evict_one` adds a radix erase. The heap uses O(H) space and O(log H) repair comparisons, with expected O(1) hash
lookups; growth/rehashing is amortized and adversarial collisions worsen the bounds.
Pool payload capacity excludes metadata and allocator overhead. No model-serving
speedup or full-project performance result is claimed.

## References and assistance

- Kwon et al., *Efficient Memory Management for Large Language Model Serving with
  PagedAttention*, SOSP 2023, doi:10.1145/3600006.3613165.
- Zheng et al., *SGLang: Efficient Execution of Structured Language Model Programs*,
  NeurIPS 2024.
- EE538 Fall 2026 Final Project assignment.

Codex assisted with the storage implementation, tests and documentation. The custom
pool, COW and heap code use STL containers for supporting storage; reference scans
and sorting are used only in tests. The team is responsible for reviewing the work
and explaining the implementation and results.
