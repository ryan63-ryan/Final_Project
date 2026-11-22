# ReferenceCache checks

This test supports proposal section 5. It runs the real `KVCacheManager` against a
small reference cache using ordinary vectors, maps and linear scans. The reference
does not use the production block pool, block table, radix tree or eviction heap.
It predicts outcomes before comparing them with the manager.

Logical block handles identify shared storage in the reference. The test maps these
handles to returned physical block IDs, so it can check sharing and ID reuse without
depending on a particular free-list order. Equal token prefixes can still occupy
separate private blocks; the reference keeps that distinction.

The checks cover request bytes, full-block prefix hits, capacity decisions, COW,
release, the original eleven statistics (including fragmentation), reference
counts and LRU/FIFO eviction. The later `live_requests` and `duplicate_blocks`
statistics have separate manager tests; they are not compared by this oracle.
Hand traces cover
empty prompts, block boundaries, private duplicate prefixes, pinned ancestors,
healing, cascading eviction and rejection rollback. Sharing on and off each have
their own expected state, since their admission decisions may differ. Parser
checks also exercise malformed commands and token boundaries.

There are 192 seeded sequences of 400 operations per reclaimable mode: seeds
17/538/2026/991, block
widths 1/2/4/16, capacities 1/3/8, both sharing settings and both policies. Payload
sizes are 1/8/64 bytes for capacities 1/3/8 respectively. Both `count` and `scan`
run the same suite: 384 sequence executions, or 153,600 randomized operations,
plus hand traces and final releases. Every step checks all live requests and all pool slots;
each sequence ends by releasing all requests. Failures print the configuration,
seed and operation history. Re-running the test reproduces the same sequence.

Run from the repository root:

```sh
make reference-test
make test
make sanitize
```

The first command runs just the differential test. The other two include it with
the existing tests; `make sanitize` enables ASan and UBSan.

CMake also registers `reference_tests` with CTest:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build -R '^reference_tests$' --output-on-failure
```

These are correctness checks on small caches, not performance measurements. The
reference is intentionally slow and is compiled only into the test executable.
Operations call the manager API directly; CLI rules for skipping later commands
after a rejected request are outside this model. System allocation failures are
distinct from the pool-capacity rejections tested here.

Revalidated on November 22, 2026 against main `c4cd853` on macOS 14.3/arm64,
Apple Clang 15.0.0. Both the full Make test suite with `-Werror` and a separate
ASan/UBSan build passed. This run did not repeat CMake/CTest or Ubuntu/GCC testing.

```text
PASS malformed commands and valid trace boundaries
PASS hand-calculated ReferenceCache traces (reclaimable=count and scan)
PASS 155088 differential operations; 55680 rejects, 39096 errors, 6632 hit tokens, 548 COW copies, 3796 evictions
```

Rejects and errors here are expected outcomes of capacity and invalid-request
tests, not failed comparisons. Totals include both modes; the same suite is
executed twice, rather than adding new random seeds.

Exact commands for this revalidation (separate build directories avoid stale binaries):

```sh
make -j2 BUILD=build-bohan-report CXXFLAGS='-std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror' test
make -j2 BUILD=build-bohan-report-asan CXXFLAGS='-std=c++17 -O1 -g -Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer' test
```
