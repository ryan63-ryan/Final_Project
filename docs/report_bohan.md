# Bohan's report sections

Prepared against main `c4cd853`, November 22, 2026. These sections belong in the
team report's design, correctness, evaluation and contribution sections; they are
not a separate full report. Section numbers can follow the team's draft.

## Storage design and ownership

`BlockPool` owns a fixed number of preallocated payload blocks and a free list.
`BlockTable` maps each request's logical block positions to physical block IDs.
Each table entry owns one reference, and the prefix index owns another reference
for each block it caches. A block returns to the free list only when its reference
count reaches zero. Tables release their references on destruction; copying a
table is disabled, while moving transfers ownership and `fork()` explicitly shares
it. The pool must outlive the tables and index.

Append has three cases: write into an exclusive partial tail, allocate after a
full tail, or clone a shared partial tail before writing. For example, a fork of
`[10,11,12]` initially shares both two-token blocks. Appending `13` to the child
copies only the partial block containing `12`; the parent's bytes remain unchanged.
If no block is available for this copy, append fails without changing either
request. Full blocks are immutable, and only full blocks enter the prefix index.

The custom `MinHeap<T, Compare>` stores eviction priorities in a vector, with a
hash map from key to heap position. Sift-up and sift-down implement heap repair;
equal priorities use the smaller key. The policy supplies ranks and eligibility,
so the heap supports both LRU and FIFO without implementing either policy itself.

## Storage complexity

Let C be pool capacity, B tokens per block, P bytes per token, L blocks per request,
and H heap entries.

| Operation | Cost |
|---|---|
| Construct pool | O(CBP) time and payload space |
| Allocate or clone a block | O(BP), including zeroing or copying bytes |
| Retain or release one reference | O(1) |
| Append into an exclusive partial tail | O(P) |
| Append requiring a new block or COW | O(BP), plus amortized O(1) table growth |
| Fork, import a prefix, or clear a table | O(L) |
| Heap push/pop/update/erase | Expected amortized O(log H); top is O(1) |

A table resize can copy O(L) IDs. Heap bounds assume constant-cost priorities and
expected O(1) hash access; collisions and reallocation can make an individual
operation slower. Table and heap metadata use O(L) and O(H) space. These are
storage-layer costs, excluding manager lookup, indexing and eviction work.

## Correctness checks

The storage tests compare 27,000 seeded operations with ordinary payload vectors
and 30,000 heap operations with a linear-scan reference. They check ownership,
copy-on-write, parent-first release, exhausted capacity, moves and heap order.

For proposal Section 5, an independent `ReferenceCache` models the whole manager
using vectors, maps and exhaustive scans, without the production pool, block table,
radix tree or heap. It predicts results first. Logical allocation handles are then
matched to physical IDs, allowing ID reuse while detecting unintended aliasing.
After each operation, the test compares request bytes, prefix hits, status,
reference counts, cache membership, eviction order and the original eleven
statistics, including fragmentation. Hand traces cover pinned ancestors, private
duplicates, cascading eviction and rejection rollback.

There are 192 seeded sequences of 400 operations per reclaimable mode, covering
four seeds, four block widths, three capacities, sharing on/off and LRU/FIFO.
Both `count` and `scan` run the same suite. Including hand traces and final releases,
155,088 comparisons passed: 55,680 expected rejections, 39,096 expected errors,
6,632 hit tokens, 548 COW copies and 3,796 evictions. These totals count both modes;
they do not represent twice as many distinct randomized traces.

On macOS 14.3/arm64 with Apple Clang 15, the full Make test suite passed with
`-std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror` and separately with ASan/UBSan.
The reference tests call the manager directly; CLI skipping of rejected requests
is covered separately by golden traces. These checks cover tested cases, not every
possible input or system allocation failure.

## Storage waste

In the team's separate fragmentation experiment, statistics were sampled every
64 operations with B=16 and P=64, one run per configuration. Empty slots are summed
over allocated physical blocks; free blocks are not internal fragmentation. For
the 100k-request system workload at capacity 1,024 with LRU, sharing on averaged
237.126 empty slots and 31.648 live requests: about 7.49 slots per live request.
Sharing off averaged 237.186 slots, or 3.37% of its mean allocated token capacity.
With sharing on, those empty slots occupy 1.45% of the preallocated pool.

The result is consistent with roughly half an unused tail block per request. If
tail fill levels are uniform, expected waste is (B-1)/2 slots; only B=16 was
benchmarked. At capacity 256, sharing has more empty slots because it admits more
requests, so absolute waste alone is misleading. Equal-size reusable blocks avoid
external fragmentation within the pool. Private duplicate blocks remain possible;
none were observed at the sample points. Sampling may miss brief peaks, and these
synthetic payload experiments do not measure model-serving speedup.

## Contribution and assistance

Bohan Cui implemented the block pool, block tables, reference counting,
copy-on-write and indexed heap, their storage tests, and the independent
ReferenceCache differential tests. The experiment data above come from Yingtao's
committed runs; Bowen implemented the radix index and eviction policies. Codex
assisted Bohan with implementation, tests and writing.

---

Integration evidence (omit this block from the final report):

- Design: `src/block_pool.cpp`, `src/block_table.cpp`, `include/kvcache/min_heap.h`.
- Validation: `tests/storage_tests.cpp`, `tests/reference_cache.*`,
  `tests/reference_tests.cpp`, and [ReferenceCache checks](reference_tests.md).
- Measurements: `results/raw/fragmentation.csv` and its `.meta.txt`;
  [experiment notes, Section 7](experiment_notes.md#7-fragmentation-storage-waste).
  These are the team's Linux/GCC measurements, not a new macOS performance run.
- Figure if space permits: `results/figures/report/fragmentation_vs_capacity.png`.
  The paragraph already gives one concrete result; avoid repeating the full table.
- Keep the team's references and AI-use statement in the final report. Merge this
  contribution text with the other members' accounts; total report target is 4-6 pages.
