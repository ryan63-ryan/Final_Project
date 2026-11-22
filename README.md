# Paged KV-Cache Manager with Radix Prefix Sharing

EE538 Final Project, USC Fall 2026. **Track B** (LLM/MLSys module). C++17.

A manager that stores per-request key-value payloads in a bounded pool of fixed-size
blocks, the way an LLM server pages its KV cache. Requests reuse full blocks of a shared
token prefix through a radix index, forks share blocks until a write forces a private
copy, and cached blocks that no request holds are evicted in LRU or FIFO order when the
pool is full. Deterministic bytes derived from each token prefix stand in for KV
tensors, so storage, sharing and isolation can be tested without a model.

See the [proposal](EE538_Final_Project_Proposal.pdf) for the agreed scope.

| Member | Responsibility |
|---|---|
| Bohan Cui | Storage: `BlockPool`, `BlockTable`, reference counts, copy-on-write, template `MinHeap` |
| Bowen Deng | `RadixTree` prefix index, LRU/FIFO eviction policies, radix compression |
| Yingtao Yang | Config and trace parser, CLI driver, `KVCacheManager` integration, experiments; integration lead |

**Open items** (marked TODO below): `ReferenceCache` class, radix edge compression,
O(1) reclaimable count, report PDF, AI-use statement.

## Contents

1. [Build](#build)
2. [Run](#run)
3. [Test](#test)
4. [Experiments](#experiments)
5. [Requirements, implementation and evidence](#requirements-implementation-and-evidence)
6. [Design notes and complexity](#design-notes-and-complexity)
7. [Known limitations](#known-limitations)
8. [AI use](#ai-use)
9. [Repository layout](#repository-layout)

## Build

| | |
|---|---|
| Language | C++17, standard library only |
| Compiler | GCC 13.3 (Ubuntu 24.04, WSL2) is the reference; also built with GCC 11.4 and Apple Clang 15 |
| Flags | `-std=c++17 -O2 -Wall -Wextra -Wpedantic` |
| Build tools | Make, or CMake 3.16 or newer |
| Python | Python 3 (standard library) for the trace generator and two tests; `matplotlib` only for the plots |

With Make:

```sh
make                 # builds everything into build-make/
make test            # builds and runs the tests
make sanitize        # the same tests under AddressSanitizer + UBSan, in build-asan-make/
make clean
```

With CMake:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Sanitizer build with CMake, in its own directory:

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DKV_SANITIZE=ON
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

The sanitizer flags are `-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer`.

## Run

```
kvcache --config <cfg> --trace <trace> [--csv <out.csv>] [--no-check]
```

| Option | Meaning |
|---|---|
| `--config` | Configuration file (below). Required. |
| `--trace` | Trace file (below). Required. |
| `--csv` | Append one row describing the run to this file: configuration, counters, latency per operation type, total times, peak RSS. The header is written if the file is new or empty. |
| `--no-check` | Skip the invariant check after every operation (used for experiments). The invariants are still checked once at the end. |

The driver prints one line per command. It exits with 0 on success and 1 if the files
cannot be read, a trace line cannot be parsed, or an invariant fails. A bad trace line
is reported on standard error and the rest of the trace still runs.

### Configuration file

`key=value` lines; blank lines and `#` comments are skipped; missing keys keep their
defaults. Unknown or repeated keys and invalid values are errors with a line number.

| Key | Default | Meaning |
|---|---|---|
| `capacity` | 1024 | Physical blocks in the pool |
| `tokens_per_block` | 16 | Tokens per block |
| `bytes_per_token` | 64 | Payload bytes stored per token |
| `policy` | `lru` | `lru` or `fifo` |
| `sharing` | `on` | `on`, or `off` for the independent-allocation baseline |
| `seed` | 42 | Seed of the fake payload bytes |

### Trace format

One command per line; blank lines and lines starting with `#` are skipped.

| Command | Meaning | Output on success |
|---|---|---|
| `ADMIT <req> [<tok> ...]` | Create a request with this prompt | `blocks=[...] cached=<tokens from the cache>` |
| `APPEND <req> <tok>` | Add one generated token | `blocks=[...] cow=<0 or 1>` |
| `FORK <new> <src>` | Create `<new>` with the tokens of `<src>` | `blocks=[...]` |
| `RELEASE <req>` | Drop a request | `freed=[...]` |
| `STATS` | Print the counters so far | one `STATS ...` line |

Any operation that evicts cached blocks also prints `evicted=[...]`. Failures print
`REJECT <reason>` (not enough free plus evictable blocks; nothing changed) or
`ERROR <reason>` (unknown or duplicate request ID; nothing changed). After a request is
rejected at creation, its later commands and its forks are skipped.

### Example

`tests/golden/example.cfg` sets two tokens per block and four blocks, the example from
the proposal:

```sh
./build-make/kvcache --config tests/golden/example.cfg --trace tests/golden/example.trace
```

`tests/golden/example.trace`:

```
ADMIT r1 10 11 12
FORK r2 r1
APPEND r2 13
RELEASE r1
```

Expected output (`tests/golden/example.expected`):

```
ADMIT r1 -> OK blocks=[0,1] cached=0
FORK r2 r1 -> OK blocks=[0,1]
APPEND r2 -> OK blocks=[0,2] cow=1
RELEASE r1 -> OK freed=[1]
```

r2 shares both of r1's blocks. Its APPEND finds the partial tail (block 1) shared, so it
copies it into block 2 before writing. Releasing r1 frees block 1; block 0 is still held
by r2 and is also in the prefix index.

A second example shows eviction, and LRU and FIFO making different choices; the
step-by-step calculation is in [docs/integration_notes.md](docs/integration_notes.md),
section 4:

```sh
./build-make/kvcache --config tests/golden/eviction_lru.cfg  --trace tests/golden/eviction.trace
./build-make/kvcache --config tests/golden/eviction_fifo.cfg --trace tests/golden/eviction.trace
```

## Test

```sh
make test            # unit tests and golden traces
make sanitize        # the same under ASan + UBSan
make gen-test        # trace generator tests (needs python3)
make csv-test        # driver CSV test (needs cmake)
ctest --test-dir build --output-on-failure      # all of the above, 12 tests
python3 tests/demo_errors.py build-asan-make/block_demo   # 12 extra input and error cases
```

| Test file | What it checks |
|---|---|
| `tests/storage_tests.cpp` | Pool accounting, payload isolation, ownership, capacity failure, moves, copy-on-write; 27,000 seeded storage steps against independent payload vectors; heap order with 30,000 steps against a scan-based reference and a 4,097-entry heap |
| `tests/radix_tests.cpp` | `match`, `insert`, `erase`, reference ownership and moves of the index; 30,000 seeded steps against a map of indexed prefixes |
| `tests/eviction_tests.cpp` | Policy bookkeeping and errors, LRU against FIFO order, pinned leaves never evicted, parent re-checked after its last child is evicted; 36,000 seeded steps against a scan-based model |
| `tests/manager_tests.cpp` | Prefix-dependent payloads; the proposal example; fork and copy-on-write; hand-calculated prefix-reuse trace; hand-calculated LRU and FIFO eviction trace with cascade; all-or-nothing capacity checks; healing of an evicted ancestor; RELEASE notifications; `sharing=off` baseline; errors and rejects leaving state intact; moves; 24,000 seeded operations with invariants and byte checks after each |
| `tests/golden/example.*` | The proposal example through the driver, exact output |
| `tests/golden/eviction*` | One eviction trace under LRU, FIFO and `sharing=off`, exact output |
| `tests/golden/fork_rejected.*` | Commands of a rejected request and of its forks are skipped |
| `tests/golden/block_demo.*`, `tests/demo_errors.py` | Storage demo output, and 12 malformed-input and error cases |
| `tests/check_csv.cmake` | `--csv` appends one row per run with a single header and a fixed number of columns |
| `tests/gen_trace_test.py` | Generated traces are valid, deterministic and bounded, have the sharing structure of their mode, and replay through the driver without an ERROR line |

Every class has a `check_invariants()` used throughout the tests. The manager's version
recomputes every stored payload from the tokens alone, requires each block's reference
count to equal its request holders plus one if indexed, and cross-checks the policy's
evictable set against the tree (`src/kv_cache_manager.cpp`).

## Experiments

```sh
# One trace by hand (modes: unique, system, multiturn, skewed, skewed_short).
python3 scripts/gen_trace.py --mode system --requests 1000 --seed 1 --out traces/system_1k.trace

# The sweep. It prints the plan and a time estimate first.
scripts/run_experiments.sh --dry-run                               # plan only
scripts/run_experiments.sh --quick --out results/raw/quick.csv     # 60 runs, seconds
scripts/run_experiments.sh --out results/raw/full.csv              # 900 runs, about 35 minutes

# Figures from results/raw/sweep*.csv (needs matplotlib).
python3 scripts/plot.py              # appendix set -> results/figures/
python3 scripts/plot.py --report     # report set   -> results/figures/report/
```

The sweep crosses sharing {off, on} x policy {LRU, FIFO} x capacity {256, 1,024, 4,096
blocks} x five workloads x trace size {1k, 10k, 100k requests}, five repetitions each,
with 16 tokens per block and 64 bytes per token. The committed results are
`results/raw/sweep.csv` and `results/raw/sweep_skewed.csv` (900 runs), with the
hardware, compiler, flags and seeds in the `.meta.txt` files next to them.

Main results at 100k requests:

- With a shared system prefix, 66% of prompt tokens come from the cache, and at 256
  blocks sharing admits 99.9% of requests against 58.5% without.
- Multi-turn hit rate grows with capacity: 1.7%, 33.0% and 60.7% at 256, 1,024 and
  4,096 blocks.
- The ADMIT call is 1.3 to 6.4 times slower with sharing on, and the gap grows with
  capacity; FORK is 6 to 14 times faster.
- LRU and FIFO evict the same blocks unless cached prefixes are reused without being
  extended; in that workload LRU has a 2 to 3 percentage point higher hit rate.

Workloads, full tables, the LRU/FIFO explanation, limitations and exact reproduction
commands are in **[docs/experiment_notes.md](docs/experiment_notes.md)**. Token counts
are a proxy for recomputation; no model-serving speedup is claimed.

## Requirements, implementation and evidence

| Requirement | Implementation | Evidence |
|---|---|---|
| Substantive custom work | Block pool with free list and reference counts, block tables with copy-on-write, radix prefix index, indexed heap, LRU/FIFO policies and the manager are written by the team; STL containers only as supporting storage | `src/`, `include/kvcache/` |
| Operational completeness | ADMIT, APPEND, FORK, RELEASE and STATS through a CLI that reads a config and a trace; every operation is all-or-nothing; defined errors for missing files, malformed lines and invalid IDs | `src/main.cpp`, `src/kv_cache_manager.cpp`, `src/trace.cpp`, `src/config.cpp`, `tests/golden/` |
| Design comparison | Prefix sharing against an independent-allocation baseline (`sharing=off`), and LRU against FIFO, replaying identical traces at the same capacity | `include/kvcache/config.h`, `tests/golden/eviction_{lru,fifo,off}.*`, `docs/experiment_notes.md` sections 4 and 5 |
| Evaluation | Hit rate, uncached tokens, admission, blocks, latency and memory across workloads, capacities and sizes | `docs/experiment_notes.md`, `results/raw/`, `results/figures/` |
| C++: classes | `BlockPool`, `BlockTable`, `RadixTree`, `KVCacheManager`, policies | `include/kvcache/*.h` |
| C++: template | `MinHeap<T, Compare>`, an indexed binary heap | `include/kvcache/min_heap.h` |
| C++: RAII and ownership | `BlockTable` and `RadixTree` release their pool references in their destructors; copying is disabled, moves transfer ownership; `RadixTree` owns nodes with `unique_ptr` | `src/block_table.cpp`, `src/radix_tree.cpp`, `include/kvcache/block_pool.h` |
| C++: inheritance and virtual calls | Abstract `EvictionPolicy`; `RankedPolicy` base; `LRUPolicy` and `FIFOPolicy` override `on_access`; the manager calls them through `unique_ptr<EvictionPolicy>` | `include/kvcache/eviction_policy.h`, `src/eviction_policy.cpp` |
| C++: file input | Config file and trace file parsers with line-numbered errors | `src/config.cpp`, `src/trace.cpp`, `src/main.cpp` |
| C++: multiple files and build | 8 source files, 9 headers, Make and CMake builds | `Makefile`, `CMakeLists.txt` |
| Data structure 1 | Radix tree over full blocks (prefix index) | `include/kvcache/radix_tree.h`, `src/radix_tree.cpp` |
| Data structure 2 | Indexed min-heap with key lookup, `update` and `erase` | `include/kvcache/min_heap.h` |
| Data structure 3 | Block pool with a free list and per-block reference counts | `include/kvcache/block_pool.h`, `src/block_pool.cpp` |
| Algorithm 1 | Longest-prefix match over whole blocks | `RadixTree::match` in `src/radix_tree.cpp`; used by `KVCacheManager::admit` |
| Algorithm 2 | Copy-on-write with reference counting | `BlockTable::append` in `src/block_table.cpp`, `BlockPool::clone` in `src/block_pool.cpp` |
| Algorithm 3 | Greedy leaf eviction with cascade, and the reclaimable-capacity check before it | `evict_one` in `src/eviction_policy.cpp`; `reclaimable_blocks` and `make_room` in `src/kv_cache_manager.cpp` |
| Tests: normal, boundary, error | Empty prompts, exact block boundaries, fork divergence, parent-first release, exhausted capacity, invalid IDs, malformed input | `tests/manager_tests.cpp`, `tests/storage_tests.cpp`, `tests/demo_errors.py` |
| Tests: invariants | `check_invariants()` on every class, called after each step of the seeded tests and after every driver operation unless `--no-check` | `src/kv_cache_manager.cpp`, `src/radix_tree.cpp`, `src/eviction_policy.cpp`, `src/main.cpp` |
| Tests: golden | Three traces with five expected outputs (one runs under LRU, FIFO and `sharing=off`), plus the storage demo | `tests/golden/` |
| Tests: reference | Bytes of every live request recomputed from its tokens alone and compared after each of 24,000 seeded operations; storage, heap, radix and policy tests each compare against a scan-based model. **TODO:** a standalone `ReferenceCache` class and brute-force checks of expected prefix hits are not written yet | `tests/manager_tests.cpp` (`expected_bytes`, `random_tests`), `tests/storage_tests.cpp`, `tests/radix_tests.cpp`, `tests/eviction_tests.cpp` |
| Tests: sanitizers | All tests pass under AddressSanitizer and UndefinedBehaviorSanitizer | `make sanitize`; `KV_SANITIZE` in `CMakeLists.txt` |
| Complexity | Time and space of every main operation | [Design notes and complexity](#design-notes-and-complexity) |
| Experiments: at least 3 sizes | 1k, 10k and 100k requests | `scripts/run_experiments.sh`, `results/raw/sweep.csv` |
| Experiments: 5 repetitions | Five runs per configuration; median and interquartile range reported | `scripts/run_experiments.sh`, `scripts/plot.py` |
| Experiments: at least 2 factors | Sharing, policy, capacity and workload | `docs/experiment_notes.md` section 2 |
| Experiments: time and memory | Per-operation median, p99 and mean latency; total operation and driver time; peak RSS | `src/main.cpp`, `docs/experiment_notes.md` section 5.2 |
| Experiments: raw data | One CSV row per run, settings files, and scripts that regenerate traces and figures | `results/raw/`, `scripts/` |
| Radix edge compression (proposal section 4) | **TODO:** the index on this branch has one node per block. Compressed edges with splits at block boundaries are implemented on branch `bowen/radix-compression` and not merged yet | `include/kvcache/radix_tree.h` |
| Report | **TODO:** report PDF not in the repository yet | |

## Design notes and complexity

**Ownership.** Each `BlockTable` entry owns one pool reference, and the prefix index
owns exactly one reference per indexed block. For an indexed block a count of one
therefore means "cached, no request uses it". Only full blocks are indexed; partial
tails are private or shared by forks and are freed at zero references.

**Payloads.** The bytes stored for token *i* are derived from a hash of the whole
prefix up to *i*. Equal prefixes give equal bytes, so sharing a block is safe;
different prefixes give different bytes, so a wrong prefix match changes what a request
reads and the tests catch it.

**ADMIT.** Match the longest cached run of full blocks, pin it, check that the
remaining blocks are free or reclaimable, evict if needed, write only the uncached
tokens, and index the blocks that were filled. A Reject drops the pins and changes
nothing.

**Eviction.** Only an indexed leaf that no request holds can be evicted, so removing it
orphans no longer prefix. Evicting a leaf can turn its parent into a leaf (cascade).
The victim is the evictable block with the smallest rank; LRU refreshes a block's rank
when a cache hit reuses it, FIFO does not.

Let C be the pool capacity, B tokens per block, P bytes per token, L the blocks of one
request, k the blocks of a matched prefix, F the largest number of children of one
index node, and E the evictable blocks.

| Operation | Time | Notes |
|---|---|---|
| `BlockPool` construction | O(C B P) time and space | Payload is preallocated |
| `allocate`, `clone` | O(B P) | Zeroes or copies one block |
| `retain`, `release` | O(1) | |
| `BlockTable::append` | O(P); O(B P) with copy-on-write | Amortized O(1) table growth |
| `BlockTable::fork`, `from_prefix`, `clear` | O(L) | |
| `MinHeap` push, pop, update, erase | O(log E) | Expected O(1) key lookup |
| `RadixTree::match`, `insert` over k blocks | O(k B log F) token comparisons | |
| `RadixTree::erase`, `contains`, `is_leaf`, `parent` | Expected O(1) | |
| Policy `on_insert`, `on_access`, `on_erase`, `set_evictable` | O(log E) | `victim` is O(1) |
| `KVCacheManager::admit`, T prompt tokens of which k blocks are cached | O(k B log F) match + O((T - kB) P) writes + O(log E) per eviction | Plus O(C) for the reclaimable scan when the free list is too short. Indexing the new blocks copies O(T^2 / B) tokens in total |
| `KVCacheManager::append` | O(P) amortized; O(B P) with copy-on-write | Plus O(L B log F) when the token fills a block and it is indexed |
| `KVCacheManager::fork` | O(L) with sharing; O(T P) without | The baseline copies every token |
| `KVCacheManager::release` | O(L log E) | |
| `KVCacheManager::stats` | O(C) | Fragmentation scan |

Space: O(C B P) payload, O(N B) tokens for N indexed blocks, O(N) policy state, and
O(T) tokens per live request.

**TODO (O(1) reclaimable count):** the O(C) scan in `admit` is the likely reason ADMIT
gets slower as capacity grows with sharing on. The planned replacement, a count kept by
the tree, is specified in
[docs/bowen_reclaimable_count.md](docs/bowen_reclaimable_count.md); the sweep will be
rerun to compare.

## Known limitations

- **A rejected APPEND does not end its request.** The request continues without that
  token. See [docs/integration_notes.md](docs/integration_notes.md), section 7, for the
  consequences and the size of the effect.
- **Radix edges are not compressed** on this branch (TODO above).
- **No standalone `ReferenceCache`** (TODO above); tests use independent recomputation
  instead.
- **ADMIT cost grows with capacity** with sharing on (TODO above).
- **Single-threaded.** GPU kernels, real models, threading and distributed serving are
  outside the scope.
- **Fake payloads.** Latencies are those of the bookkeeping only; token counts do not
  establish a model-serving speedup.
- Experiments were run on one machine with one trace seed; see
  [docs/experiment_notes.md](docs/experiment_notes.md), section 6.

## AI use

**TODO (team):** describe which AI tools were used, for what, and how the output was
reviewed and tested, as the assignment requires.

## Repository layout

```
include/kvcache/     Headers
  block_pool.h         BlockPool: preallocated blocks, free list, reference counts
  block_table.h        BlockTable: a request's blocks; append, fork, from_prefix
  min_heap.h           MinHeap<T, Compare>: indexed binary heap (template)
  radix_tree.h         RadixTree: prefix index over full blocks
  eviction_policy.h    EvictionPolicy, LRUPolicy, FIFOPolicy, evict_one
  kv_cache_manager.h   KVCacheManager: ADMIT, APPEND, FORK, RELEASE, stats
  config.h, trace.h, types.h
src/                 Implementations and the CLI driver (main.cpp)
examples/            block_demo.cpp: small storage demo
tests/               Unit tests, test scripts, and golden/ traces with expected output
scripts/             gen_trace.py, run_experiments.sh, plot.py
results/raw/         Sweep CSVs and their settings (.meta.txt)
results/figures/     Appendix figures; report/ holds the 7-inch report set
docs/
  integration_notes.md         Integration steps, hand-calculated traces, limitations
  experiment_notes.md          Workloads, results, figures, reproduction
  bowen_reclaimable_count.md   Spec for the O(1) reclaimable count
EE538_Final_Project_Proposal.pdf
Makefile, CMakeLists.txt
```

`traces/` (generated traces, about 820 MB for the full sweep) and the `build*/`
directories are not tracked.
