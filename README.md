# Paged KV-Cache Manager with Radix Prefix Sharing

EE538 Final Project (USC, Fall 2026), Track B: LLM/MLSys module, C++17.

This project implements a KV-cache manager like the ones used in LLM serving systems.
KV memory comes from a pool of fixed-size blocks, and each sequence maps its tokens to
blocks through a block table. A radix tree over token prefixes lets requests that share
a prompt prefix reuse the same physical blocks. A shared, partially filled block is
copied on write before one sequence appends to it. When the pool hits its capacity
limit, the manager evicts cached prefixes using a configurable policy (LRU or FIFO,
with RLT as a stretch goal).

## Team

| Member       | Responsibilities                                                                                                     |
|--------------|----------------------------------------------------------------------------------------------------------------------|
| Bohan Cui    | Block pool, block tables, reference counting, copy-on-write, `MinHeap<T, Compare>`                                   |
| Bowen Deng   | Radix prefix index, eviction policies                                                                                |
| Yingtao Yang | `KVCacheManager` facade, trace driver/CLI, reference model, workload generator, experiments; integration lead        |

## Status

Proposal stage, see [docs/proposal.pdf](docs/proposal.pdf).

## Build

Normal (Release) build:

```sh
cmake -S . -B build
cmake --build build
./build/kvcache
```

Sanitizer build (AddressSanitizer + UndefinedBehaviorSanitizer):

```sh
cmake -S . -B build-asan -DKV_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan
./build-asan/kvcache
```

## Directory layout

```
include/kvcache/   public headers
src/               implementation sources and main.cpp (CLI)
tests/             unit tests
tests/golden/      golden traces and expected outputs
scripts/           workload generation and plotting scripts
results/raw/       raw experiment output (CSV files are git-ignored)
results/figures/   generated figures
docs/              proposal, design notes, and report
```
