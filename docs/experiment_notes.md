# Experiment notes: sharing on/off and LRU vs FIFO

Branch `yingtao/experiments`, 2026-11-21. Evaluation of proposal section 6.

Every number in this file comes from `results/raw/sweep.csv` and
`results/raw/sweep_skewed.csv` (900 runs in total, no ERROR line in any run).

## 1. Summary

- **Sharing pays off in tokens and in admission, not in ADMIT latency.** With a shared
  system prefix, 66% of prompt tokens come from the cache, and at capacity 256 the
  manager admits 99.9% of requests instead of 58.5%. But the ADMIT call itself is 1.3 to
  6.4 times slower with sharing on, in every configuration except one (section 5).
- **LRU and FIFO make the same decisions on four of the five workloads.** They differ
  only in `skewed_short`, where LRU has a 2-3 percentage point higher hit rate
  (section 4).
- **Unique prompts show the cost of sharing with no benefit:** 0% hits, the same
  admission rate, and ADMIT 3-6 times slower.
- Token counts are a proxy for recomputation. They do not establish a model-serving
  speedup: a real system spends far more time computing a token's KV entries than this
  manager spends storing fake payload bytes.

## 2. Setup

| | |
|---|---|
| Machine | AMD Ryzen 7 9800X3D, 16 GB RAM, Linux 6.6 under WSL2 |
| Compiler | GCC 13.3.0, `-std=c++17 -O2 -Wall -Wextra -Wpedantic` |
| Block | 16 tokens per block, 64 payload bytes per token (1,024 bytes per block) |
| Capacity | 256, 1,024 and 4,096 blocks (0.25, 1 and 4 MiB of payload) |
| Sharing | off, on |
| Policy | LRU, FIFO |
| Trace size | 1k, 10k, 100k requests |
| Repetitions | 5 per configuration, run sequentially |
| Driver | `--no-check`, standard output to `/dev/null` |
| Seeds | trace generator seed 1, payload seed 42 |

The exact settings of each sweep, including the commit, are in
`results/raw/sweep.meta.txt` and `results/raw/sweep_skewed.meta.txt`.

How the numbers are measured:

- **Latency** is the time of the manager call alone (`std::chrono::steady_clock`
  around `admit`, `append`, `fork` or `release`). Parsing, invariant checks and printing
  are outside. Rejected calls are timed too.
- Each run reports the median, 99th percentile and mean per operation type from a
  constant-memory histogram (percentiles within 0.8% of the true sample). The tables
  below give the median over the repetitions of the per-run value.
- **Counters** (hits, evictions, admitted, blocks) are deterministic: all repetitions of
  a configuration agree, and `plot.py` refuses to plot if they do not.
- With sharing off the prefix index is never used, so the policy cannot matter. The LRU
  and FIFO runs with sharing off have identical counters and are pooled (10
  repetitions).
- **Peak RSS** is `getrusage` `ru_maxrss` for the whole driver process.
- APPEND medians are 80-100 ns, which is at the resolution of the clock; differences
  between configurations there are not meaningful.

## 3. Workloads

All traces come from `scripts/gen_trace.py` (standard library only, seeded). A request
is created by ADMIT, or for about 2% of requests by FORK of a running request; it then
receives its output one APPEND at a time and is RELEASEd. Up to 32 requests run at once
and their commands are interleaved at random. The generator never emits an invalid
command.

Parameters common to all modes (generator defaults):

| Parameter | Value |
|---|---|
| `--max-active` | 32 running requests at most |
| `--arrival` | 0.1: chance per step that a new request starts while below the maximum |
| `--fork-fraction` | 0.02 |
| `--vocab` | 32,000 token IDs |
| `--prompt-min`, `--prompt-max` | 32 to 256 tokens |
| `--output-min`, `--output-max` | 8 to 64 APPENDs (0 to 8 in `skewed_short`) |
| `--seed` | 1 |

The five modes:

| Mode | Prompt | Mode parameters |
|---|---|---|
| `unique` | Random tokens, uniform length 32-256 | none |
| `system` | One of 4 fixed system prompts (uniform choice) of 128 tokens, then a random user part of 1-128 tokens | `--system-prompts 4`, `--system-len 128` |
| `multiturn` | The whole conversation so far (previous prompt + previous output) plus a new user message of 16-64 tokens; a conversation ends when it would exceed 256 tokens | `--turn-min 16`, `--turn-max 64`, `--continue-chance 0.8` |
| `skewed` | One of 1,024 system prompts of 128 tokens chosen with Zipf weights 1/k, then a random user part of 1-128 tokens | `--system-prompts 1024`, `--zipf 1.0` |
| `skewed_short` | As `skewed`, but the user part is 1-15 tokens and the output 0-8 tokens | `--short-user-max 15`, output 0-8 |

`unique`, `system` and `multiturn` are the three workloads named in the proposal.
`skewed` and `skewed_short` were added to study the eviction policies. The 1,024 system
prompts take 8 blocks each, 8,192 prefix blocks in total: twice the largest pool, so the
cache can never hold all of them.

What the generated traces contain (measured with sharing off at capacity 4,096, where
nothing is rejected):

| Mode | Size | ADMITs | FORKs | APPENDs | Mean prompt (tokens) | Peak live blocks |
|---|---|---|---|---|---|---|
| `unique` | 1k | 975 | 25 | 36,453 | 148 | 413 |
| `unique` | 10k | 9,810 | 190 | 362,317 | 145 | 421 |
| `unique` | 100k | 98,007 | 1,993 | 3,611,673 | 144 | 440 |
| `system` | 1k | 982 | 18 | 35,877 | 192 | 478 |
| `system` | 10k | 9,785 | 215 | 361,857 | 193 | 494 |
| `system` | 100k | 98,104 | 1,896 | 3,606,713 | 193 | 500 |
| `multiturn` | 1k | 977 | 23 | 36,167 | 111 | 369 |
| `multiturn` | 10k | 9,801 | 199 | 358,921 | 114 | 370 |
| `multiturn` | 100k | 98,057 | 1,943 | 3,602,941 | 114 | 386 |
| `skewed` | 1k | 977 | 23 | 36,784 | 194 | 482 |
| `skewed` | 10k | 9,808 | 192 | 362,137 | 192 | 489 |
| `skewed` | 100k | 98,011 | 1,989 | 3,607,102 | 192 | 497 |
| `skewed_short` | 1k | 986 | 14 | 3,895 | 136 | 93 |
| `skewed_short` | 10k | 9,891 | 109 | 39,215 | 136 | 118 |
| `skewed_short` | 100k | 98,846 | 1,154 | 400,119 | 136 | 165 |

"Peak live blocks" is the demand of the running requests alone. It is 370-500 blocks
for the first four modes, so capacity 256 is saturated (requests are rejected), and
1,024 and 4,096 hold every running request with room left for the cache.
`skewed_short` needs at most 165 blocks and is never rejected.

## 4. LRU vs FIFO

### 4.1 Full table for the skewed workloads (sharing on)

Hit rate = cached prompt tokens / prompt tokens of admitted requests. The difference is
in percentage points. Uncached tokens are the prompt tokens written fresh.

| Mode | Size | Capacity | LRU hit rate | FIFO hit rate | LRU - FIFO | LRU evictions | FIFO evictions | LRU uncached tokens | FIFO uncached tokens |
|---|---|---|---|---|---|---|---|---|---|
| `skewed` | 1k | 256 | 24.79% | 24.79% | +0.00 | 6,669 | 6,669 | 91,995 | 91,995 |
| `skewed` | 1k | 1,024 | 30.92% | 30.85% | +0.07 | 9,005 | 9,013 | 130,961 | 131,089 |
| `skewed` | 1k | 4,096 | 41.77% | 41.77% | +0.00 | 4,648 | 4,648 | 110,401 | 110,401 |
| `skewed` | 10k | 256 | 26.34% | 26.34% | +0.00 | 68,997 | 68,997 | 919,526 | 919,526 |
| `skewed` | 10k | 1,024 | 32.77% | 32.75% | +0.01 | 96,317 | 96,333 | 1,269,256 | 1,269,512 |
| `skewed` | 10k | 4,096 | 45.54% | 45.53% | +0.01 | 78,173 | 78,181 | 1,028,104 | 1,028,232 |
| `skewed` | 100k | 256 | 25.65% | 25.65% | +0.00 | 694,588 | 694,588 | 9,240,082 | 9,240,082 |
| `skewed` | 100k | 1,024 | 32.54% | 32.54% | +0.01 | 973,530 | 973,610 | 12,724,255 | 12,725,535 |
| `skewed` | 100k | 4,096 | 45.76% | 45.75% | +0.01 | 814,657 | 814,721 | 10,231,439 | 10,232,463 |
| `skewed_short` | 1k | 256 | 35.49% | 32.56% | +2.93 | 4,922 | 5,168 | 86,576 | 90,512 |
| `skewed_short` | 1k | 1,024 | 53.65% | 51.49% | +2.16 | 2,631 | 2,812 | 62,208 | 65,104 |
| `skewed_short` | 1k | 4,096 | 62.37% | 62.37% | +0.00 | 0 | 0 | 50,496 | 50,496 |
| `skewed_short` | 10k | 256 | 35.61% | 32.78% | +2.83 | 51,611 | 53,992 | 866,069 | 904,165 |
| `skewed_short` | 10k | 1,024 | 56.62% | 53.73% | +2.89 | 33,186 | 35,614 | 583,557 | 622,405 |
| `skewed_short` | 10k | 4,096 | 77.77% | 76.00% | +1.77 | 12,333 | 13,818 | 299,061 | 322,821 |
| `skewed_short` | 100k | 256 | 35.64% | 32.77% | +2.87 | 517,730 | 541,879 | 8,650,885 | 9,037,269 |
| `skewed_short` | 100k | 1,024 | 57.18% | 54.44% | +2.74 | 336,038 | 359,052 | 5,756,101 | 6,124,325 |
| `skewed_short` | 100k | 4,096 | 79.56% | 77.58% | +1.98 | 144,897 | 161,568 | 2,746,997 | 3,013,733 |

Reading the table:

- In `skewed` the two policies are within 0.07 percentage points everywhere, and
  identical at capacity 256.
- In `skewed_short` LRU is ahead by 2-3 points wherever anything is evicted, makes
  4-10% fewer evictions and writes 4-9% fewer tokens. The 1k trace at capacity 4,096
  evicts nothing (the pool never fills), so there is nothing for the policies to
  decide.
- The same holds for the proposal's three workloads: in `sweep.csv`, LRU and FIFO have
  the same hits and evictions in every configuration except one (`multiturn` 100k at
  capacity 256: 735,226 against 735,225 evictions).

ADMIT latency is the same under both policies (100k: medians within 2.4% in every
configuration), so the policy changes what is cached, not what an operation costs.

### 4.2 Why plain `skewed` does not separate the policies

Two rules of the design combine:

1. **Only leaves are evicted.** A block with a cached child cannot be evicted until the
   child is gone.
2. **Every block a request fills is indexed below the prefix it reused.**

In `skewed` a request reuses the 8 blocks of a system prompt and then stores its own
user part and output below them: 1-128 user tokens plus 8-64 output tokens, almost
always at least one new full block. So every time a prefix is used, a new child is
inserted under it a moment later.

Now follow one system prompt P to its eviction:

- P's last block can only become a leaf when all the user blocks below it have been
  evicted. Those are unique, never reused, so both policies rank them by insertion
  time and evict them in the same order.
- When P's newest child C is finally evicted, P becomes a candidate. Under FIFO its
  rank is its insertion time, which is old. Under LRU its rank is its last use, and
  the last use was by the request that inserted C, just before C's insertion. C was
  the oldest candidate a moment ago, and P's rank is slightly older than C's.
- So under both policies P is now the oldest candidate, and it is evicted next,
  followed by the rest of its chain.

"Least recently used" and "first inserted" therefore give the same eviction order:
the recency of a prefix is tied to the insertion time of its newest descendant, and
the descendant shields it until then. A popular prefix survives under both policies
for the same reason: it keeps getting new children.

The small differences that remain (8 to 80 evictions out of several thousand to
nearly a million)
come from the few requests that reuse a prefix without filling a block below it: for
example a short user part followed by a short output, or a request rejected for lack
of capacity.

The same argument covers `system` (4 prompts with unique user parts) and `multiturn`
(each turn reuses the previous turn's blocks and adds new ones below).

### 4.3 Why `skewed_short` does

`skewed_short` breaks rule 2's side effect: the user part is 1-15 tokens (less than one
block) and the output 0-8 tokens, so a request adds at most 23 tokens after the system
prompt and often fills no block at all. Most cache hits then leave no child behind.

The last block of a system prompt is now usually a leaf itself, and it is reused while
it is an eviction candidate:

- Under LRU, each reuse gives it a new rank. A popular prompt is reused before it
  reaches the front of the queue and stays cached.
- Under FIFO, its rank stays its insertion time. However often it is reused, it
  reaches the front, is evicted, and its 8 blocks are written again at the next
  request.

That is the 2-3 point gap in the table. It is the same effect as in the
hand-calculated unit trace (`docs/integration_notes.md` section 4, step 8), at scale.

The gap is modest because Zipf with exponent 1 over 1,024 prompts is not very
concentrated (the most popular prompt gets about 13% of requests), and because the
most popular prompts are also the ones most likely to be held by a running request,
which protects them under either policy.

### 4.4 What this means for the report

The answer to "how do LRU and FIFO handle pressure" is workload-dependent in a specific
way: with leaf-only eviction, the policies differ only when cached blocks are reused
*without being extended*. Workloads where every request extends what it reuses
(system prompt plus a long unique turn, multi-turn chat) make the policy choice
irrelevant. Workloads with many requests that reuse a prefix and add little
(classification or short-answer queries over a shared prompt) favor LRU.

## 5. Sharing on vs off

100k-request traces. "On" is sharing on with LRU; "off" is the pooled baseline.

### 5.1 Hits, admission and blocks

| Mode | Capacity | Hit rate (on) | ADMITs accepted, on | ADMITs accepted, off | Uncached tokens (M), on | Uncached tokens (M), off | Peak blocks, on | Peak blocks, off | Evictions (on) |
|---|---|---|---|---|---|---|---|---|---|
| `unique` | 256 | 0.0% | 76.7% | 76.7% | 10.07 | 10.05 | 256 | 256 | 763,999 |
| `unique` | 1,024 | 0.0% | 100.0% | 100.0% | 14.14 | 14.14 | 1,024 | 440 | 1,062,329 |
| `unique` | 4,096 | 0.0% | 100.0% | 100.0% | 14.14 | 14.14 | 4,096 | 440 | 1,059,257 |
| `system` | 256 | 66.4% | 99.9% | 58.5% | 6.34 | 10.78 | 256 | 256 | 575,438 |
| `system` | 1,024 | 66.4% | 100.0% | 100.0% | 6.35 | 18.91 | 1,024 | 500 | 575,281 |
| `system` | 4,096 | 66.4% | 100.0% | 100.0% | 6.35 | 18.91 | 4,096 | 500 | 572,209 |
| `multiturn` | 256 | 1.7% | 89.3% | 89.2% | 9.24 | 9.37 | 256 | 256 | 735,226 |
| `multiturn` | 1,024 | 33.0% | 100.0% | 100.0% | 7.50 | 11.19 | 1,024 | 386 | 646,627 |
| `multiturn` | 4,096 | 60.7% | 100.0% | 100.0% | 4.40 | 11.19 | 4,096 | 386 | 450,219 |
| `skewed` | 256 | 25.6% | 67.3% | 58.6% | 9.24 | 10.77 | 256 | 256 | 694,588 |
| `skewed` | 1,024 | 32.5% | 100.0% | 100.0% | 12.72 | 18.86 | 1,024 | 497 | 973,530 |
| `skewed` | 4,096 | 45.8% | 100.0% | 100.0% | 10.23 | 18.86 | 4,096 | 497 | 814,657 |
| `skewed_short` | 256 | 35.6% | 100.0% | 100.0% | 8.65 | 13.44 | 256 | 165 | 517,730 |
| `skewed_short` | 1,024 | 57.2% | 100.0% | 100.0% | 5.76 | 13.44 | 1,024 | 165 | 336,038 |
| `skewed_short` | 4,096 | 79.6% | 100.0% | 100.0% | 2.75 | 13.44 | 4,096 | 165 | 144,897 |

Findings:

- **Shared system prefix:** 66.4% of prompt tokens are cached at every capacity. The 4
  prompts need only 32 blocks and are always in use, so capacity does not matter. At
  capacity 256, sharing admits 99.9% of ADMITs against 58.5%: requests that share 8
  blocks need fewer blocks of their own.
- **Multi-turn:** the hit rate depends strongly on capacity: 1.7% at 256, 33.0% at
  1,024, 60.7% at 4,096. A conversation's blocks must survive in the cache until its
  next turn; at 256 blocks the running requests alone fill the pool and nearly
  everything is evicted first.
- **Unique prompts:** no hits and no admission benefit. Sharing only adds work: about
  10 evictions per ADMIT, because every block a request fills is cached and later
  evicted without ever being reused.
- **Skewed:** between the two: 26-46% hits, and 67.3% against 58.6% admission at 256.
- **Peak blocks:** with sharing on the pool always fills to capacity, because cached
  blocks stay allocated until evicted. Sharing off shows the live demand (165-500
  blocks).
- At capacity 256 the uncached-token columns are not comparable between on and off
  where the admission rates differ: a rejected request writes nothing. Compare them at
  1,024 and 4,096, or read them next to the admission columns.
- Copy-on-write happens only for forks: about 1,800 copies per 100k requests (about
  900 in `skewed_short`).

### 5.2 Latency and memory

Median over repetitions, written as on / off. ADMIT in microseconds, the others in
nanoseconds.

| Mode | Capacity | ADMIT median (us) | ADMIT p99 (us) | APPEND median (ns) | FORK median (ns) | RELEASE median (ns) | All operations (s) | Peak RSS (MB) |
|---|---|---|---|---|---|---|---|---|
| `unique` | 256 | 9.2 / 3.3 | 27.3 / 8.9 | 90 / 80 | 524 / 3664 | 422 / 270 | 1.71 / 0.71 | 6.1 / 5.9 |
| `unique` | 1,024 | 16.3 / 4.6 | 39.2 / 10.2 | 81 / 80 | 490 / 5504 | 450 / 262 | 2.34 / 0.86 | 5.5 / 5.0 |
| `unique` | 4,096 | 29.6 / 4.6 | 61.7 / 9.8 | 90 / 80 | 502 / 5472 | 502 / 262 | 3.77 / 0.85 | 9.6 / 8.2 |
| `system` | 256 | 8.3 / 4.7 | 21.1 / 10.2 | 81 / 100 | 510 / 5184 | 442 / 350 | 1.47 / 0.74 | 4.4 / 7.1 |
| `system` | 1,024 | 11.7 / 6.1 | 28.0 / 12.0 | 90 / 80 | 502 / 6880 | 470 / 290 | 1.86 / 1.02 | 5.4 / 5.0 |
| `system` | 4,096 | 22.9 / 6.1 | 49.9 / 12.1 | 90 / 80 | 510 / 6880 | 510 / 290 | 3.08 / 1.02 | 9.6 / 8.2 |
| `multiturn` | 256 | 8.3 / 2.7 | 24.4 / 8.6 | 90 / 80 | 502 / 2992 | 370 / 241 | 1.55 / 0.69 | 5.0 / 5.1 |
| `multiturn` | 1,024 | 11.5 / 3.5 | 32.1 / 9.1 | 90 / 80 | 482 / 4192 | 422 / 241 | 1.91 / 0.76 | 5.4 / 5.0 |
| `multiturn` | 4,096 | 22.4 / 3.5 | 49.9 / 9.2 | 90 / 80 | 490 / 4224 | 462 / 241 | 3.04 / 0.75 | 9.6 / 8.2 |
| `skewed` | 256 | 10.2 / 4.7 | 26.8 / 9.5 | 90 / 92 | 572 / 5024 | 502 / 342 | 1.76 / 0.73 | 6.8 / 7.0 |
| `skewed` | 1,024 | 17.0 / 6.0 | 35.1 / 10.6 | 90 / 80 | 510 / 6752 | 498 / 290 | 2.39 / 1.00 | 5.4 / 5.0 |
| `skewed` | 4,096 | 28.3 / 6.1 | 57.1 / 11.5 | 90 / 80 | 510 / 6752 | 572 / 290 | 3.58 / 1.00 | 9.6 / 8.2 |
| `skewed_short` | 256 | 7.5 / 4.1 | 14.3 / 7.6 | 81 / 80 | 478 / 4448 | 282 / 181 | 0.72 / 0.50 | 4.1 / 4.1 |
| `skewed_short` | 1,024 | 5.4 / 4.1 | 16.8 / 7.6 | 81 / 81 | 482 / 4448 | 294 / 183 | 0.75 / 0.50 | 5.2 / 4.9 |
| `skewed_short` | 4,096 | 1.9 / 4.1 | 30.6 / 7.8 | 81 / 80 | 482 / 4448 | 310 / 187 | 0.95 / 0.50 | 9.5 / 8.1 |

Findings:

- **ADMIT is slower with sharing on, and the gap grows with capacity:** 9-10 us at 256
  and 22-30 us at 4,096 for the first four workloads, against 3-6 us with sharing off
  at any capacity. This holds even for `system`, where 66% of the tokens are not
  written at all.
- **Likely cause (not profiled):** the pool is always full of cached blocks, so nearly
  every ADMIT finds the free list short and runs the O(capacity) reclaimable scan
  before evicting. Growth with capacity is what that scan would produce. The spec in
  `docs/bowen_reclaimable_count.md` replaces it with a count kept by the tree; the
  sweep will be rerun to compare.
- **The one exception is `skewed_short` at capacity 4,096:** ADMIT median 1.9 us with
  sharing against 4.1 us without. Most requests there are full cache hits that need
  no new block, so they skip the eviction path. Its p99 (30.6 us) is still the slow
  path.
- **FORK is 6-14 times faster with sharing** (about 0.5 us against 3-7 us): sharing
  retains the source's blocks, the baseline copies every token.
- **RELEASE is 1.3-2 times slower with sharing** (index and policy bookkeeping), and
  APPEND is the same within clock resolution.
- **Total time in manager operations** is 1.4 to 4.4 times higher with sharing on,
  driven by ADMIT.
- **Peak RSS** is 4-10 MB for the whole driver and grows with capacity (4,096 blocks
  are 4 MiB of payload). Sharing adds at most 1.4 MB for the index and policy.

### 5.3 Trace size

Latency does not depend on trace size: the per-operation medians at 1k, 10k and 100k
requests are within 16% of each other at capacity 1,024
(`latency_vs_size.png`). The interquartile range over the five repetitions is small:
for example the ADMIT median for `unique` at capacity 4,096 with sharing on ranges from
28.8 to 29.8 us over the five runs.

## 6. Limitations

- **Rejected APPENDs do not end their request.** At capacity 256, 1-4% of executed
  APPENDs are rejected and the request continues without that token. See
  `docs/integration_notes.md` section 7.1. Capacities 1,024 and 4,096 are not
  affected.
- **One machine, one trace seed.** Each configuration was repeated five times on the
  same trace; the repetitions measure timing noise, not variation between traces.
- **Timing under WSL2** with traces read from a Windows drive. Only the manager calls
  are timed, so file access does not enter the operation latencies, but it dominates
  the total driver time.
- **The clock costs about as much as an APPEND.** Two `steady_clock` readings bracket
  every operation; for operations under 100 ns the reported time is mostly that.
- **The reclaimable scan explanation is an inference** from how ADMIT latency grows
  with capacity. It has not been confirmed with a profiler.
- **Fake payloads.** Stored bytes are derived from token prefixes; no model runs.
  Latencies are those of the bookkeeping only.

## 7. Figures

All in `results/figures/`, one panel per workload, with three series: sharing on with
LRU, sharing on with FIFO, and sharing off. The capacity figures use the 100k traces.

| File | What it shows |
|---|---|
| `hit_rate_vs_capacity.png` | Share of prompt tokens served from the cache at each capacity; the only panel where the LRU and FIFO lines separate is `skewed_short`. |
| `uncached_tokens_vs_capacity.png` | Prompt tokens written fresh (recomputation proxy) at each capacity; read it next to the admitted share, since rejected requests write nothing. |
| `admitted_vs_capacity.png` | Share of ADMITs accepted at each capacity; shows the saturation at 256 blocks and where sharing admits more. |
| `peak_blocks_vs_capacity.png` | Most blocks allocated at once; sharing on fills the pool with cached blocks, sharing off shows live demand. |
| `latency_vs_size.png` | Median latency of ADMIT, APPEND, FORK and RELEASE by trace size at capacity 1,024, with the interquartile range over repetitions, on a log scale. |

## 8. Reproducing

From the repository root. The sweeps take about 22 and 11 minutes on the machine above
and write about 820 MB of traces into `traces/` (ignored by git).

```sh
# 1. Build (Release, -O2) and run the tests.
make test
make gen-test        # generator tests; needs python3
make csv-test        # driver CSV test; needs cmake

# 2. The two sweeps, as they were run. --overwrite replaces the committed CSVs.
scripts/run_experiments.sh --modes "unique system multiturn" \
    --out results/raw/sweep.csv --overwrite
scripts/run_experiments.sh --modes "skewed skewed_short" \
    --out results/raw/sweep_skewed.csv --overwrite

# 3. Figures from every results/raw/sweep*.csv. Needs matplotlib.
python3 -m pip install matplotlib      # or: sudo apt install python3-matplotlib
python3 scripts/plot.py
```

Useful variants:

```sh
# Print the plan and the time estimate without running anything.
scripts/run_experiments.sh --dry-run

# Small subset: 1k traces, one repetition (60 runs, a few seconds).
scripts/run_experiments.sh --quick --out results/raw/quick.csv

# One trace and one run by hand.
python3 scripts/gen_trace.py --mode skewed_short --requests 10000 --seed 1 \
    --out traces/skewed_short_10k.trace
printf 'capacity=1024\ntokens_per_block=16\nbytes_per_token=64\npolicy=lru\nsharing=on\nseed=42\n' > /tmp/run.cfg
./build-make/kvcache --config /tmp/run.cfg --trace traces/skewed_short_10k.trace \
    --csv /tmp/run.csv --no-check > /dev/null

# Other figure selections.
python3 scripts/plot.py --size 10k --capacity 4096 --out results/figures_10k
```

Traces are deterministic for a given generator version, mode, size and seed: the
counters in the CSVs (hits, evictions, admitted, blocks) should reproduce exactly on
any machine. Latencies and peak RSS will differ.

Running both sweeps as one (`scripts/run_experiments.sh` with no options, 900 runs,
about 35 minutes) gives the same rows in a single `sweep.csv`; remove
`sweep_skewed.csv` first so `plot.py` does not pool the skewed rows twice.
