# Storage module

**Author: Bohan Cui.** Block pool, block tables, reference counts, copy-on-write and the
indexed template heap.

The two sections below are Bohan's text, verbatim, as he wrote it in the project README
(commit `8b0f553`, "Implement storage module, copy-on-write and indexed heap with
tests"). They were moved here when the README was rewritten for submission. They
describe the storage module at the time of that commit: statements such as "Ubuntu/GCC
and the CMake path have not been run here" were true then; later checks on Ubuntu are
recorded in [radix_and_eviction.md](radix_and_eviction.md) and in the README.

## Storage API

| Component | Interface and behavior |
|---|---|
| `BlockPool` | Owns preallocated bytes; `allocate`, `clone`, `retain`, `release`, `read_token`. Exhaustion returns `nullopt`. |
| `BlockTable` | Maps a request to blocks; `append`, `fork`, `from_prefix`, `read_token`, `clear`. Capacity failure on one append returns false without changing stored state. |
| `MinHeap<T, Compare>` | Custom indexed heap; `push`, `top`, `pop`, `update`, `erase`. Lower priority wins, then lower key. |

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
larger transaction. The heap comparator must not throw and priorities must support
non-throwing moves. LRU/FIFO policies decide heap eligibility and update its ranks.

## Validation and complexity

Tests check pool accounting, payload isolation, ownership, capacity failure, moves,
cached prefixes and heap invariants. They include 27,000 seeded storage steps against
independent payload vectors, 30,000 heap steps against a scan-based reference,
16-token/64-byte storage, and a 4,097-entry heap with 64-bit priorities. Seeds are
538, 2026 and 17; random steps include no-op choices. Golden demo output and 12 extra
input/error cases are included. The demo reads a prompt length (0–7), its byte values,
then one append byte; it uses four blocks of two tokens and prints both request states.

Validated on macOS 14.3/arm64 with Apple Clang 15.0.0 and Make 3.81. Release uses
`-std=c++17 -O2 -Wall -Wextra -Wpedantic`; a fresh build also passed `-Werror`.
ASan/UBSan builds use `-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer`
and passed without reported errors. Ubuntu/GCC and the CMake path have not been run
here. These checks cover storage, not the full manager or a process-wide leak audit.

Let C be pool capacity, B tokens/block, P bytes/token, L blocks/request and H heap
entries. Pool construction costs O(CBP) time/space. Allocate zeroes a block and clone
copies it: O(BP). Retain/release are O(1); an exclusive append is O(P), COW O(BP).
Table growth can copy O(L) IDs, amortized O(1) per added ID. Fork/import/clear are O(L).
The heap uses O(H) space and O(log H) repair comparisons, with expected O(1) hash
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
