# Paged KV-Cache Manager with Radix Prefix Sharing

EE538 Final Project, USC Fall 2026, Track B (LLM/MLSys module).

We built a KV-cache manager in C++17. It keeps each request's data in fixed-size blocks
taken from a pool with a hard size limit. Requests with the same prompt prefix share
blocks through a compressed radix tree, a forked request shares everything with its parent until
one of them writes, and when the pool is full we evict cached blocks using LRU or FIFO.
We don't run a real model: the bytes stored for each token are computed from the token
prefix, which is enough to test sharing and isolation.

The scope we agreed on is in the [proposal](EE538_Final_Project_Proposal.pdf).

## Team

| Member | What they did |
|---|---|
| Bohan Cui | Storage: `BlockPool`, `BlockTable`, reference counts, copy-on-write, the `MinHeap` template |
| Bowen Deng | `RadixTree`, LRU/FIFO eviction, radix compression |
| Yingtao Yang | Config and trace parser, the driver, `KVCacheManager` integration, experiments. Integration lead |

Things that are not finished yet are marked **TODO** in this file: `ReferenceCache`,
the O(1) reclaimable count, the report PDF and the AI-use section.

## Build

You need a C++17 compiler and either Make or CMake (3.16+). We used GCC 13.3 on Ubuntu
24.04 (WSL2). It also built with GCC 11.4 and Apple Clang 15. There are no library
dependencies. Python 3 is needed for the trace generator and two of the tests, and
matplotlib only for the plots.

Flags: `-std=c++17 -O2 -Wall -Wextra -Wpedantic`. The code also compiles with `-Werror`.

Make:

```sh
make              # build into build-make/
make test         # build and run the tests
make sanitize     # same tests with ASan + UBSan (build-asan-make/)
make clean
```

CMake:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

CMake with sanitizers (use a separate directory):

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DKV_SANITIZE=ON
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

## Run

```
kvcache --config <cfg> --trace <trace> [--csv <out.csv>] [--no-check]
```

- `--config` and `--trace` are required.
- `--csv` appends one row about the run to a CSV file (config, counters, latency, peak
  memory). The header is added if the file is empty.
- `--no-check` turns off the invariant check that normally runs after every command. We
  use it for experiments. The check still runs once at the end.

The program prints one line per command. The exit code is 0 if everything worked and 1
if a file can't be opened, a trace line is malformed, or an invariant fails. A bad trace
line is reported on stderr and the rest of the trace keeps going.

### Config file

`key=value`, one per line. `#` starts a comment. Keys you leave out keep the default.
Unknown keys, repeated keys and bad values give an error with the line number.

| Key | Default | Meaning |
|---|---|---|
| `capacity` | 1024 | number of blocks in the pool |
| `tokens_per_block` | 16 | tokens per block |
| `bytes_per_token` | 64 | payload bytes per token |
| `policy` | `lru` | `lru` or `fifo` |
| `sharing` | `on` | `off` is the baseline where nothing is shared |
| `seed` | 42 | seed for the fake payload bytes |

### Trace file

One command per line. Blank lines and lines starting with `#` are ignored.

| Command | What it does | Output |
|---|---|---|
| `ADMIT <req> [<tok> ...]` | new request with this prompt | `blocks=[...] cached=<n>` |
| `APPEND <req> <tok>` | add one token | `blocks=[...] cow=<0 or 1>` |
| `FORK <new> <src>` | copy of `<src>` | `blocks=[...]` |
| `RELEASE <req>` | remove the request | `freed=[...]` |
| `STATS` | print counters | `STATS ...` |

`cached` is how many prompt tokens came from the cache. If a command had to evict
blocks, the line also has `evicted=[...]`.

A command can fail in two ways, and in both cases nothing is changed:

- `REJECT`: not enough free or evictable blocks.
- `ERROR`: the request ID doesn't exist, or already exists.

If a request is rejected when it is created, the driver skips the rest of its commands
(and its forks).

### Example

This is the example from the proposal: 2 tokens per block, 4 blocks.

```sh
./build-make/kvcache --config tests/golden/example.cfg --trace tests/golden/example.trace
```

Input (`tests/golden/example.trace`):

```
ADMIT r1 10 11 12
FORK r2 r1
APPEND r2 13
RELEASE r1
```

Output (`tests/golden/example.expected`):

```
ADMIT r1 -> OK blocks=[0,1] cached=0
FORK r2 r1 -> OK blocks=[0,1]
APPEND r2 -> OK blocks=[0,2] cow=1
RELEASE r1 -> OK freed=[1]
```

r2 starts out sharing both blocks with r1. Block 1 is only half full and shared, so when
r2 appends, it gets copied to block 2 first (that's the `cow=1`). When r1 is released,
block 1 is freed. Block 0 stays because r2 still uses it.

There is also an eviction example where LRU and FIFO pick different blocks. We worked
it out by hand in [docs/integration_notes.md](docs/integration_notes.md), section 4.

```sh
./build-make/kvcache --config tests/golden/eviction_lru.cfg  --trace tests/golden/eviction.trace
./build-make/kvcache --config tests/golden/eviction_fifo.cfg --trace tests/golden/eviction.trace
```

## Test

```sh
make test          # unit tests + golden traces
make sanitize      # same, with ASan + UBSan
make gen-test      # trace generator tests (python3)
make csv-test      # CSV output test (cmake)
ctest --test-dir build --output-on-failure     # everything above, 12 tests
python3 tests/demo_errors.py build-asan-make/block_demo    # 12 bad-input cases
```

What each test file covers:

| File | What it checks |
|---|---|
| `tests/storage_tests.cpp` | Pool accounting, copy-on-write, ownership, running out of blocks, moves. 27,000 random steps compared with plain vectors. Heap order with 30,000 random steps compared with a linear scan |
| `tests/radix_tests.cpp` | `match`, `insert`, `erase`, who owns which reference. Compressed edges: a match that ends in the middle of an edge, splits after the first block and after a middle block, split tails keep their children. 30,000 random steps compared with a `std::map` |
| `tests/radix_fault_tests.cpp` | Makes every allocation fail in turn during an edge extension, a new edge and two kinds of split, and checks nothing changed or leaked |
| `tests/eviction_tests.cpp` | LRU vs FIFO order, blocks in use are never evicted, parent becomes evictable after its last child goes, evicting a compressed edge one block at a time. 36,000 random steps compared with a scan |
| `tests/manager_tests.cpp` | The proposal example, fork and copy-on-write, a prefix-reuse trace and an LRU/FIFO eviction trace we calculated by hand, rejects that must not change anything, `sharing=off`, bad request IDs, and 24,000 random operations |
| `tests/golden/example.*` | proposal example through the driver |
| `tests/golden/eviction*` | same eviction trace with LRU, FIFO and `sharing=off` |
| `tests/golden/fork_rejected.*` | commands of a rejected request and its forks are skipped |
| `tests/golden/block_demo.*`, `tests/demo_errors.py` | storage demo and bad input |
| `tests/check_csv.cmake` | `--csv` writes one header and one row per run |
| `tests/gen_trace_test.py` | generated traces are valid, the same for the same seed, and run without errors |

Every class has a `check_invariants()` function. The manager's version rebuilds every
stored payload from the tokens and compares, and checks that each block's reference
count equals the number of requests using it (plus one if it is in the index).

## Experiments

```sh
# generate one trace (modes: unique, system, multiturn, skewed, skewed_short)
python3 scripts/gen_trace.py --mode system --requests 1000 --seed 1 --out traces/system_1k.trace

# run the sweep
scripts/run_experiments.sh --dry-run                             # just show the plan and time estimate
scripts/run_experiments.sh --quick --out results/raw/quick.csv   # 60 runs, a few seconds
scripts/run_experiments.sh --out results/raw/full.csv            # 900 runs, about 35 min

# plots (needs matplotlib), reads results/raw/sweep*.csv
python3 scripts/plot.py             # all workloads  -> results/figures/
python3 scripts/plot.py --report    # report figures -> results/figures/report/
```

The sweep is sharing {off, on} x policy {LRU, FIFO} x capacity {256, 1024, 4096} x 5
workloads x size {1k, 10k, 100k requests}, 5 repetitions each, with 16 tokens per block
and 64 bytes per token. Our results are in `results/raw/sweep.csv` and
`results/raw/sweep_skewed.csv`. The `.meta.txt` files next to them have the machine,
compiler, flags and seeds.

What we found (100k requests):

- With a shared system prompt, 66% of prompt tokens come from the cache. At 256 blocks,
  sharing lets 99.9% of requests in, compared with 58.5% without sharing.
- For multi-turn chats the hit rate depends a lot on capacity: 1.7%, 33.0%, 60.7% at
  256, 1024, 4096 blocks.
- ADMIT is slower with sharing on (1.3x to 6.4x), and it gets worse with bigger
  capacity. FORK is 6x to 14x faster.
- LRU and FIFO evict almost exactly the same blocks in most workloads. They only differ
  when a cached prefix gets reused without anything new being added under it. In that
  workload LRU's hit rate is 2 to 3 points higher.

The full tables, the explanation for the LRU/FIFO result, and the exact commands to
reproduce everything are in [docs/experiment_notes.md](docs/experiment_notes.md). Note
that "uncached tokens" is only a stand-in for recomputation. We are not claiming any
real model speedup.

## Requirements

This follows the Phase II sections of the assignment. Paths are from the repo root.

### Project standard (Track B)

| Requirement | What we did | Where |
|---|---|---|
| Substantive custom work | Blocks are shared by requests, by forks and by the cache at the same time, and the pool has a fixed size. The pool, tables, index, policies and manager all have to agree on the reference counts | `src/`, `docs/integration_notes.md` |
| Operational completeness | Full request lifecycle (ADMIT, APPEND, FORK, RELEASE), admission check, eviction with cascade, rejects that change nothing | `src/kv_cache_manager.cpp`, `src/main.cpp`, `tests/golden/` |
| Inspectable design choice | No sharing vs prefix sharing, and LRU vs FIFO, on the same traces and capacity | `tests/golden/eviction_{lru,fifo,off}.*`, `docs/experiment_notes.md` sec. 4-5 |
| Meaningful evaluation | 5 workloads, 3 capacities, 3 sizes. Includes cases where sharing is slower or useless | `docs/experiment_notes.md`, `results/` |
| Feasible boundaries | Ours: everything in `src/` and `include/`. Reused: STL, Python/matplotlib for scripts. Not done on purpose: GPU, real model, threads, distributed | [Known limitations](#known-limitations), proposal sec. 3 |
| I/O and state contract | Trace commands in, one result line out. Every command either fully happens or changes nothing | [Run](#run), `include/kvcache/kv_cache_manager.h` |
| Driver on the real implementation | `kvcache` replays a trace on the actual manager and buffers | `src/main.cpp` |
| Independent reference | Tests recompute each request's bytes from its tokens only. **TODO:** separate `ReferenceCache` class and a brute-force check of prefix hits | `tests/manager_tests.cpp` |
| Documented harness instead of a model | Fake payload bytes from the token prefix, synthetic traces | `include/kvcache/kv_cache_manager.h`, `scripts/gen_trace.py` |

### KV-cache module contract (from the assignment's example)

| Contract | What we did | Where |
|---|---|---|
| Shared prefix without duplicate payloads | ADMIT reuses the longest cached run of full blocks | `prefix_reuse_tests` in `tests/manager_tests.cpp` |
| Fork and append: private copy on write | A shared partial block is copied before writing | `cow_tests`, `tests/golden/example.*` |
| Release in any order keeps counts correct | Destructors release references; invariants check every count | `release_tests`, `random_tests` |
| Blocks in use are never evicted; defined failure when there's no room | Only unused leaf blocks can be evicted. ADMIT is rejected before anything changes | `eviction_capacity_tests`, `tests/eviction_tests.cpp` |
| Prefix index: splits, inserts, removals | Edges hold runs of whole blocks. Inserting in the middle of an edge splits it at a block boundary, erase removes one block at a time. Lookup results are the same as with one node per block | `src/radix_tree.cpp` (`split_insert`, `extend`, `erase`), `tests/radix_tests.cpp`, `tests/radix_fault_tests.cpp`, `heal_tests` in `tests/manager_tests.cpp` |
| Sharing off/on and LRU/FIFO on the same traces, with a "sharing is rare" workload and one under memory pressure | `unique` workload; capacity 256 | `scripts/run_experiments.sh`, `docs/experiment_notes.md` sec. 5 |
| Report reuse, storage waste, latency, proxy definition | Hit rate, uncached tokens, latency per operation. **TODO:** fragmentation is saved in the CSVs (`fragmentation_slots`) but we haven't analyzed it | `docs/experiment_notes.md`, `results/raw/` |

### C++

| Requirement | What we did | Where |
|---|---|---|
| Core in C++ | Only scripts are Python/shell | `src/`, `scripts/` |
| Classes, encapsulation, const-correctness | `BlockPool`, `BlockTable`, `RadixTree`, `KVCacheManager`, policy classes | `include/kvcache/*.h` |
| Template | `MinHeap<T, Compare>` | `include/kvcache/min_heap.h` |
| STL for supporting work, own implementations identified | We use `vector`, `map`, `unordered_map`, `unique_ptr` for storage. The pool, tables, radix tree, heap (not `std::priority_queue`), policies and manager are ours | `src/`, `include/kvcache/` |
| RAII, copy/move handled | Tables and the index release their references in destructors and are move-only. `BlockPool` can't be copied or moved | `include/kvcache/block_pool.h`, `block_table.h`, `radix_tree.h` |
| Inheritance and virtual functions | `EvictionPolicy` (abstract) -> `RankedPolicy` -> `LRUPolicy`, `FIFOPolicy` | `include/kvcache/eviction_policy.h` |
| File input, invalid input, reproducible output | Config and trace files, errors with line numbers, fixed seeds | `src/config.cpp`, `src/trace.cpp`, `src/main.cpp` |
| Multiple files + build system | 8 `.cpp`, 9 `.h`, Makefile and CMake | `Makefile`, `CMakeLists.txt` |
| C++17, compiler and flags documented, warnings on | See [Build](#build). Builds clean with `-Wall -Wextra -Wpedantic -Werror` | `Makefile` |

### Data structures and algorithms

| | What | Where |
|---|---|---|
| Data structure 1 | Compressed radix tree (prefix index) | `include/kvcache/radix_tree.h`, `src/radix_tree.cpp` |
| Data structure 2 | Indexed binary min-heap | `include/kvcache/min_heap.h` |
| Data structure 3 | Block pool with free list and reference counts | `src/block_pool.cpp` |
| Algorithm 1 | Longest-prefix match with edge splits | `RadixTree::match`, `split_insert` in `src/radix_tree.cpp` |
| Algorithm 2 | Copy-on-write with reference counting | `BlockTable::append` in `src/block_table.cpp` |
| Algorithm 3 | Greedy leaf eviction with cascade, plus counting how many blocks can be reclaimed | `evict_one` in `src/eviction_policy.cpp`; `reclaimable_blocks`, `make_room` in `src/kv_cache_manager.cpp` |
| How they work together | One ADMIT uses all of them: the tree finds the prefix, reference counts tell which cached blocks are unused, the heap orders them, eviction removes leaves from the tree, and the freed blocks are written and indexed | `KVCacheManager::admit` |
| Radix compression (proposal sec. 4) | An edge stores a run of full blocks and their tokens. Edges are extended, split and shortened only at block boundaries | `src/radix_tree.cpp`, `docs/radix_and_eviction.md` |

### Testing

| Requirement | What we did | Where |
|---|---|---|
| Repeatable test command | `make test`, `make sanitize`, `ctest` | [Test](#test) |
| Normal, boundary, invalid input | Empty prompt, exact block boundary, full pool, unknown/duplicate IDs, malformed files | `tests/manager_tests.cpp`, `tests/storage_tests.cpp`, `tests/demo_errors.py` |
| Invariants for each data structure | `check_invariants()` in pool, table, heap, tree, policies, manager | `src/*.cpp`, `include/kvcache/min_heap.h` |
| Hard state transitions | Copy-on-write on a shared block, release with several holders, eviction of a block's ancestor, reject with a partial cache hit | `tests/manager_tests.cpp` |
| Full input with expected output | 3 traces, 5 expected outputs | `tests/golden/` |
| Independent reference | Vectors, linear scans and `std::map` in the unit tests; recomputed bytes in the manager tests. **TODO:** `ReferenceCache` | `tests/` |
| Sanitizer | All tests pass with ASan + UBSan on Linux | `make sanitize` |
| Known limitations | Listed below | [Known limitations](#known-limitations) |

### Complexity and experiments

| Requirement | What we did | Where |
|---|---|---|
| Complexity with variables defined, worst/average/amortized | Table below | [Complexity](#complexity) |
| Two approaches on the same inputs | Sharing off vs on, LRU vs FIFO | `docs/experiment_notes.md` sec. 4-5 |
| At least 3 sizes | 1k, 10k, 100k requests | `results/raw/sweep.csv` |
| Repetitions, central value, variability | 5 runs each, median and interquartile range | `scripts/plot.py`, `docs/experiment_notes.md` sec. 2 |
| Machine, flags, generator, seed | Saved with each sweep | `results/raw/*.meta.txt` |
| Separate preprocessing from operation time | Only the manager call is timed. Parsing and printing are counted separately as driver time | `src/main.cpp` |
| Time and memory | Median, p99, mean per operation. Memory is the peak RSS of the whole process (measured, includes the pool, the index and the driver) | `docs/experiment_notes.md` sec. 5.2 |
| At least 2 factors besides size | Workload (how much prefix sharing) and capacity (memory pressure) | `docs/experiment_notes.md` sec. 2-3 |
| Module boundary, driver overhead, proxy metrics | Operation time vs total driver time. Uncached tokens are labeled as a proxy | `docs/experiment_notes.md` sec. 1, 2, 6 |
| Raw data and reproduce commands | CSV row per run, scripts for traces and plots | `results/raw/`, `scripts/`, `docs/experiment_notes.md` sec. 8 |

### Submission

| Item | Status |
|---|---|
| README with commands and this table | this file |
| Members and contributions | [Team](#team) |
| Sample input/output | `tests/golden/` |
| Report | **TODO:** PDF not added yet |
| Commit or release tag | **TODO** |
| AI use | **TODO:** see [AI use](#ai-use) |

## Design notes

More detail on each part, written by whoever built it:

- [docs/storage_module.md](docs/storage_module.md): pool, tables, copy-on-write, heap (Bohan)
- [docs/radix_and_eviction.md](docs/radix_and_eviction.md): radix index and eviction (Bowen)
- [docs/integration_notes.md](docs/integration_notes.md): how the manager puts them together, with traces worked out by hand (Yingtao)

The short version:

- **Reference counts.** Each request holds one reference per block it uses. The index
  holds one reference per block it caches. So if an indexed block has count 1, it is
  cached and nobody is using it.
- **Only full blocks are cached.** A partial last block belongs to its request (or is
  shared by forks) and is freed when the count reaches 0.
- **Fake payloads.** The bytes for token i depend on all tokens up to i. Same prefix
  gives the same bytes, so sharing is safe. A different prefix gives different bytes,
  so if we ever matched the wrong prefix the tests would see wrong data.
- **ADMIT.** Find the longest cached prefix, hold on to it, check there is room for the
  rest (free blocks + blocks we could evict), evict if needed, write only the tokens
  that weren't cached, then add the newly filled blocks to the index. If there isn't
  room we reject and nothing has changed.
- **Radix tree.** An edge holds a run of full blocks, so a long prompt that nobody
  branches off from is one node. The interface is still per block. If a new request
  branches off in the middle of an edge, the edge is split at that block boundary.
- **Eviction.** We only evict the last block of a leaf edge that no request is using, so we never
  cut off a longer cached prefix. After a leaf goes, its parent may become a leaf and
  can be evicted next. LRU updates a block's position when it is reused, FIFO doesn't.

### Complexity

C = pool capacity (blocks), B = tokens per block, P = bytes per token, L = blocks of one
request, T = its tokens, k = matched prefix blocks, F = max children of a tree node,
E = evictable blocks.

| Operation | Time | Notes |
|---|---|---|
| Create `BlockPool` | O(C B P) | also the space; all payload is allocated up front |
| `allocate`, `clone` | O(B P) | clears or copies one block |
| `retain`, `release` | O(1) | |
| `BlockTable::append` | O(P), or O(B P) with copy-on-write | table growth is amortized O(1) |
| `BlockTable::fork`, `from_prefix`, `clear` | O(L) | |
| `MinHeap` push/pop/update/erase | O(log E) | key lookup is a hash map, expected O(1) |
| `RadixTree::match`, `insert` (k blocks over e edges, e <= k) | O(k B + e B log F) | extending an edge is amortized O(B); a split also copies the s tail blocks and moves its c children: O(s B + c) |
| `RadixTree::erase`, `contains`, `is_leaf`, `parent` | expected O(1) | hash map; erase is amortized |
| Policy `on_insert`/`on_access`/`on_erase`/`set_evictable` | O(log E) | `victim` is O(1) |
| `admit` | O(k B + e B log F) + O((T - kB) P) + O(log E) per eviction | plus O(C) when the free list isn't enough (see below) |
| `append` | O(P) amortized, O(B P) with copy-on-write | plus O(L B log F) when the block fills up and gets indexed |
| `fork` | O(L) with sharing, O(T P) without | the baseline copies everything |
| `release` | O(L log E) | |
| `stats` | O(C) | scans for fragmentation |

Unless marked, these are worst case. "Expected" ones go through a hash map and can be
worse with bad collisions. Space is O(C B P) for payload, O(N B) tokens and at most N
tree nodes for N indexed blocks, and O(T) tokens per live request.

**TODO (O(1) reclaimable count):** when the free list is too short, `admit` scans all
blocks to count what could be evicted. We think this is why ADMIT gets slower with
larger capacity. The plan is to keep that count inside the tree instead; the spec is in
[docs/bowen_reclaimable_count.md](docs/bowen_reclaimable_count.md). We will rerun the
experiments after that.

## Known limitations

- If an APPEND is rejected, the request stays alive and just misses that token. A real
  server would probably cancel it. Details in
  [docs/integration_notes.md](docs/integration_notes.md), section 7.
- The radix tree doesn't merge nodes back after an erase. After eviction a node can be
  left with a single child, so the tree is not always fully compressed. This only
  affects the node count, not results. (Merging would need to allocate inside `erase`,
  and eviction relies on `erase` not throwing.)
- No separate `ReferenceCache` class yet (TODO).
- ADMIT gets slower as capacity grows when sharing is on (TODO above).
- Single-threaded only. No GPU, no real model, no distributed serving.
- Payloads are fake, so the latencies only measure our bookkeeping.
- Experiments ran on one machine with one trace seed. More in
  [docs/experiment_notes.md](docs/experiment_notes.md), section 6.

## AI use

**TODO:** to be written by the team (which tools, what for, how we checked the output).

## Repository layout

```
include/kvcache/     headers
  block_pool.h         pool of blocks, free list, reference counts
  block_table.h        blocks of one request; append, fork, from_prefix
  min_heap.h           MinHeap<T, Compare> template
  radix_tree.h         prefix index (radix tree with compressed edges)
  eviction_policy.h    EvictionPolicy, LRUPolicy, FIFOPolicy, evict_one
  kv_cache_manager.h   KVCacheManager
  config.h, trace.h, types.h
src/                 .cpp files and the driver (main.cpp)
examples/            block_demo.cpp, a small storage demo
tests/               unit tests, test scripts, golden/ traces
scripts/             gen_trace.py, run_experiments.sh, plot.py
results/raw/         sweep CSVs and .meta.txt
results/figures/     plots (report/ has the ones for the report)
docs/
  storage_module.md            storage details (Bohan)
  radix_and_eviction.md        radix and eviction details (Bowen)
  integration_notes.md         integration notes and hand-worked traces
  experiment_notes.md          experiment results and how to reproduce
  bowen_reclaimable_count.md   spec for the O(1) count
Makefile, CMakeLists.txt
EE538_Final_Project_Proposal.pdf
```

`traces/` (generated, about 820 MB for the full sweep) and `build*/` are not in git.
