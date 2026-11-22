# Experiment notes: sharing on/off and LRU vs FIFO

Evaluation of proposal section 6. First written 2026-11-21 on branch
`yingtao/experiments`; latency numbers remeasured 2026-11-22 on branch
`yingtao/o1-count`, after radix edge compression and the O(1) reclaimable count.

Where the numbers come from (no ERROR line in any run):

| File | Runs | Used for |
|---|---|---|
| `results/raw/sweep.csv` | 900 | Everything except the two rows below. `reclaimable=count`, the default. |
| `results/raw/scan_sweep.csv` | 900 | The same sweep with `reclaimable=scan`, for section 6. |
| `results/raw/fragmentation.csv` | 180 | One repetition with statistics sampled every 64 operations, for section 7. |

The sweeps from before the O(1) count are kept in `results/raw/before_o1_count/`. Their
counters are identical to the current ones; only latencies changed.

## 1. Summary

- **Sharing pays off in tokens and in admission, and costs little in ADMIT latency.**
  With a shared system prefix, 66% of prompt tokens come from the cache, and at
  capacity 256 the manager admits 99.9% of requests instead of 58.5%. The ADMIT median
  with sharing on is 0.4 to 2.1 times the sharing-off median: equal for the shared
  system prefix, faster when most requests are full cache hits, slower otherwise
  (section 5.2).
- **The O(1) reclaimable count removed most of the cost of sharing.** With the earlier
  O(capacity) scan the ADMIT median was 1.1 to 5.0 times higher and grew with capacity;
  both variants were measured from one build and make identical decisions (section 6).
- **LRU and FIFO make the same decisions on four of the five workloads.** They differ
  only in `skewed_short`, where LRU has a 2-3 percentage point higher hit rate
  (section 4).
- **Unique prompts show the cost of sharing with no benefit:** 0% hits, the same
  admission rate, and ADMIT 1.6-2.1 times slower.
- **Fragmentation is small and comes from partial last blocks:** about 7.5
  empty token slots per running request (3-6% of the storage running requests hold),
  the same with sharing on or off, and no block was stored twice (section 7).
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

The exact settings of each sweep, including the commit, are in the `.meta.txt` file
next to each CSV.

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
- The same holds for the proposal's three workloads: LRU and FIFO have
  the same hits and evictions in every configuration except one (`multiturn` 100k at
  capacity 256: 735,226 against 735,225 evictions).

ADMIT latency is nearly the same under both policies: at 100k the medians are within
4% in every configuration except `skewed_short` at capacity 1,024 (2.2 us for LRU,
2.5 us for FIFO), where FIFO's extra misses mean more ADMITs that write and evict. So
the policy changes what is cached, not what an operation of a given kind costs.

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
| `unique` | 256 | 5.5 / 3.4 | 18.6 / 11.6 | 89 / 90 | 700 / 3824 | 628 / 294 | 1.08 / 0.71 | 6.1 / 5.9 |
| `unique` | 1,024 | 8.5 / 4.9 | 23.7 / 12.8 | 91 / 84 | 668 / 5728 | 628 / 282 | 1.53 / 0.93 | 5.4 / 5.0 |
| `unique` | 4,096 | 10.3 / 4.9 | 27.8 / 12.7 | 91 / 81 | 692 / 5728 | 732 / 282 | 1.79 / 0.93 | 9.4 / 8.2 |
| `system` | 256 | 5.0 / 4.8 | 13.9 / 12.5 | 90 / 100 | 620 / 5344 | 596 / 382 | 1.16 / 0.70 | 4.2 / 7.1 |
| `system` | 1,024 | 5.7 / 6.4 | 15.8 / 13.8 | 91 / 81 | 620 / 7104 | 660 / 308 | 1.30 / 1.08 | 5.4 / 5.0 |
| `system` | 4,096 | 6.6 / 6.4 | 18.6 / 13.9 | 91 / 81 | 644 / 7104 | 732 / 314 | 1.48 / 1.09 | 9.5 / 8.2 |
| `multiturn` | 256 | 4.7 / 2.8 | 17.5 / 10.3 | 90 / 84 | 644 / 3088 | 516 / 251 | 1.08 / 0.70 | 5.1 / 5.0 |
| `multiturn` | 1,024 | 4.2 / 3.6 | 18.8 / 11.6 | 91 / 81 | 644 / 4384 | 588 / 258 | 1.20 / 0.81 | 5.4 / 5.0 |
| `multiturn` | 4,096 | 4.3 / 3.6 | 12.0 / 11.4 | 91 / 81 | 660 / 4384 | 668 / 253 | 1.16 / 0.81 | 9.2 / 8.2 |
| `skewed` | 256 | 6.4 / 4.8 | 19.3 / 12.2 | 91 / 100 | 780 / 5152 | 836 / 382 | 1.09 / 0.70 | 6.6 / 7.0 |
| `skewed` | 1,024 | 8.5 / 6.3 | 20.9 / 13.4 | 91 / 81 | 660 / 6976 | 748 / 306 | 1.55 / 1.07 | 5.4 / 5.0 |
| `skewed` | 4,096 | 8.5 / 6.3 | 25.0 / 13.5 | 91 / 81 | 684 / 7008 | 908 / 306 | 1.68 / 1.07 | 9.4 / 8.2 |
| `skewed_short` | 256 | 6.6 / 4.1 | 15.0 / 8.4 | 90 / 80 | 588 / 4480 | 378 / 199 | 0.68 / 0.52 | 4.1 / 4.1 |
| `skewed_short` | 1,024 | 2.2 / 4.1 | 14.3 / 8.4 | 91 / 81 | 588 / 4512 | 394 / 202 | 0.58 / 0.53 | 5.1 / 4.9 |
| `skewed_short` | 4,096 | 1.8 / 4.1 | 15.3 / 8.4 | 90 / 81 | 588 / 4512 | 410 / 203 | 0.47 / 0.52 | 9.4 / 8.1 |

Findings:

- **ADMIT with sharing on is at most 2.1 times slower than off, and sometimes
  faster.** The on / off ratio of the median is:
  - `system`: 0.9-1.0, the same within noise. The prefix lookup and the eviction
    bookkeeping cost about as much as writing the 128 tokens they save.
  - `multiturn`: 1.7 at capacity 256 (1.7% hits, so sharing only adds work) and 1.2 at
    1,024 and 4,096.
  - `skewed`: 1.3 at every capacity.
  - `unique`: 1.6-2.1, the worst case: no hits, and every ADMIT evicts about 10 blocks.
  - `skewed_short`: 1.6 at 256, but 0.5 at 1,024 and 0.4 at 4,096 (2.2 and 1.8 us
    against 4.1 us). Most requests there are full cache hits that write a few tokens.
- **The ADMIT p99 is higher with sharing on everywhere:** 1.05 to 2.2 times the
  sharing-off p99. The slow ADMITs are the ones that evict many blocks.
- **The ADMIT median no longer grows with capacity** except for `unique` (5.5 to
  10.3 us) and slightly for `system` (5.0 to 6.6 us). In the earlier measurement it
  grew 2.6-3.1 times from 256 to 4,096 blocks because of the reclaimable scan
  (section 6).
- **FORK is 5-11 times faster with sharing** (0.6-0.8 us against 3-7 us): sharing
  retains the source's blocks, the baseline copies every token.
- **RELEASE is 1.6-3.0 times slower with sharing** (0.4-0.9 us against 0.2-0.4 us):
  each released block is reported to the index and the policy. APPEND is the same
  within clock resolution.
- **Total time in manager operations** is 1.1 to 1.9 times higher with sharing on in
  14 of the 15 configurations, and lower in one (`skewed_short` at 4,096: 0.47 s
  against 0.52 s).
- **Peak RSS** is 4-10 MB for the whole driver and grows with capacity (4,096 blocks
  are 4 MiB of payload). Sharing adds at most 1.3 MB for the index and policy.

### 5.3 Trace size

Latency does not depend on trace size: the per-operation medians at 1k, 10k and 100k
requests are within 17% of each other at capacity 1,024
(`latency_vs_size.png`). The spread over the five repetitions is small: for example
the ADMIT median for `unique` at capacity 4,096 with sharing on ranges from 9.8 to
11.6 us over the five runs, and three of the five are 10.3 us.

## 6. Reclaimable count: O(capacity) scan vs incremental count

Before an ADMIT evicts anything it must know how many blocks eviction could free. The
manager can get that number in two ways, chosen by the config key `reclaimable`:

- `scan`: mark every block a request holds and all its ancestors, then count the
  indexed blocks left. O(capacity) per call. This was the only way until Bowen's tree
  learned to count.
- `count` (default): `RadixTree::reclaimable_count()`, which the tree keeps up to date
  as blocks are inserted, erased and pinned. O(1) per call. The manager calls
  `RadixTree::refresh` for each block whose reference state changes.

### 6.1 Validity

- **Same build.** Both sweeps ran one after the other from the same binary (commit
  `b58a5d9`); its checksum was identical before the first, between the two and after
  the second. Only the config key differs.
- **Identical decisions.** All 180 configurations have the same counters (admitted,
  rejected, hits, uncached tokens, copies, evictions, peak blocks) in both sweeps, and
  the same as the earlier sweeps in `before_o1_count/`.
- **Checked in the tests too.** `check_invariants()` compares the count with the scan
  after every operation in both modes, and the golden traces must print the same
  output in both.
- **What "scan" measures.** The `refresh` calls run in both modes, because the
  invariant needs the count to be correct in scan mode as well. So the scan sweep is
  the scan plus that bookkeeping. Its ADMIT medians are within 6% of the earlier,
  scan-only sweep, so the bookkeeping is small next to the scan.

### 6.2 ADMIT latency

100k-request traces, sharing on with LRU, microseconds, median over five repetitions.
"Off" is sharing off from the count sweep (the key has no effect there: the two sweeps
agree within 3% on its median). FIFO is within a few percent of LRU in both modes.

| Mode | Capacity | Median, scan | Median, count | scan / count | Median, off | count / off | p99, scan | p99, count | scan / count | p99, off | count / off |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `unique` | 256 | 9.2 | 5.5 | 1.7 | 3.4 | 1.62 | 28.0 | 18.6 | 1.5 | 11.6 | 1.60 |
| `unique` | 1,024 | 16.5 | 8.5 | 1.9 | 4.9 | 1.74 | 43.8 | 23.7 | 1.8 | 12.8 | 1.85 |
| `unique` | 4,096 | 28.3 | 10.3 | 2.7 | 4.9 | 2.10 | 66.0 | 27.8 | 2.4 | 12.7 | 2.18 |
| `system` | 256 | 7.8 | 5.0 | 1.6 | 4.8 | 1.04 | 20.4 | 13.9 | 1.5 | 12.5 | 1.11 |
| `system` | 1,024 | 11.6 | 5.7 | 2.0 | 6.4 | 0.89 | 29.8 | 15.8 | 1.9 | 13.8 | 1.14 |
| `system` | 4,096 | 22.1 | 6.6 | 3.3 | 6.4 | 1.04 | 58.1 | 18.6 | 3.1 | 13.9 | 1.34 |
| `multiturn` | 256 | 8.3 | 4.7 | 1.8 | 2.8 | 1.69 | 24.7 | 17.5 | 1.4 | 10.3 | 1.70 |
| `multiturn` | 1,024 | 11.2 | 4.2 | 2.7 | 3.6 | 1.15 | 35.1 | 18.8 | 1.9 | 11.6 | 1.62 |
| `multiturn` | 4,096 | 21.4 | 4.3 | 5.0 | 3.6 | 1.17 | 58.6 | 12.0 | 4.9 | 11.4 | 1.05 |
| `skewed` | 256 | 10.2 | 6.4 | 1.6 | 4.8 | 1.33 | 28.8 | 19.3 | 1.5 | 12.2 | 1.58 |
| `skewed` | 1,024 | 17.0 | 8.5 | 2.0 | 6.3 | 1.35 | 43.3 | 20.9 | 2.1 | 13.4 | 1.56 |
| `skewed` | 4,096 | 28.0 | 8.5 | 3.3 | 6.3 | 1.35 | 73.2 | 25.0 | 2.9 | 13.5 | 1.85 |
| `skewed_short` | 256 | 7.5 | 6.6 | 1.1 | 4.1 | 1.62 | 16.3 | 15.0 | 1.1 | 8.4 | 1.79 |
| `skewed_short` | 1,024 | 5.1 | 2.2 | 2.4 | 4.1 | 0.52 | 21.9 | 14.3 | 1.5 | 8.4 | 1.69 |
| `skewed_short` | 4,096 | 1.9 | 1.8 | 1.1 | 4.1 | 0.42 | 40.7 | 15.3 | 2.7 | 8.4 | 1.82 |

Findings:

- **The scan was the cause of the growth with capacity.** From 256 to 4,096 blocks the
  scan median grows 2.6-3.1 times on the first four workloads. With the count it is
  flat for `multiturn` (4.7 to 4.3 us) and grows 1.3 times for `system` and `skewed`.
  The earlier version of these notes only inferred this from the shape of the curve;
  this is the direct comparison.
- **The count is faster in every configuration:** 1.1 to 5.0 times on the median and
  1.1 to 4.9 times on the p99. The gain is largest at capacity 4,096, where the scan
  is longest.
- **`unique` still grows with capacity under the count** (5.5, 8.5, 10.3 us). The scan
  is gone, so this is something else; it has not been measured. One candidate is that
  a larger pool holds a larger index, so the evictions an ADMIT makes touch more
  memory.
- **Is ADMIT with sharing on still slower than off?** For the median: no longer in
  general. It is equal for `system` (0.9-1.0), 1.2-1.3 times slower for `skewed` and
  for `multiturn` above capacity 256, 1.6-2.1 times slower for `unique` and, at
  capacity 256, for `multiturn` and `skewed_short`, and about twice as fast for
  `skewed_short` at 1,024 and 4,096. With the scan it was slower in 14 of 15
  configurations, by up to 5.8 times. For the p99: yes, still slower everywhere, by 1.05
  to 2.2 times.
- **Total time in manager operations** with sharing on fell from 0.75-3.73 s (scan)
  to 0.47-1.79 s (count) per 100k-request trace.

### 6.3 Cost on the other operations

The `refresh` calls are not free. Compared with the sweep made before they existed,
with sharing on:

- **FORK** median went from about 0.50 to about 0.65 us: a fork now reports every
  shared block to the tree.
- **RELEASE** median went from 0.37-0.57 to 0.52-0.91 us on the first four workloads
  (0.28-0.31 to 0.38-0.41 us on `skewed_short`), for the same reason. In the count
  sweep it is up to 14% higher than in the scan sweep although the code path is the
  same; this has not been explained.
- **APPEND** is unchanged (about 90 ns).

A request does one RELEASE and at most one FORK, each about 0.15-0.35 us slower, and
its ADMIT is up to 17 us faster, so the trade is worth it; the totals above include all operations.

## 7. Fragmentation (storage waste)

### 7.1 How it is measured

`fragmentation_slots` counts token slots that are allocated but empty: for every
allocated block, 16 minus the tokens stored in it. The column of that name in the
sweep CSVs is read once at the end of the trace, when every request has been released,
so it is 0 in every row and says nothing. For this section the driver was run with
`--sample-every 64`: after every 64th operation it reads the manager's statistics
(outside the timed region) and reports the mean and the largest sample. That is 9,376
to 59,557 samples per 100k-request trace. The run is separate
(`results/raw/fragmentation.csv`, one repetition, counters identical to the sweep)
because sampling scans the pool between operations and would disturb the latencies.

Two more statistics are sampled: live requests, and **duplicate blocks**, full blocks
that a request holds although the same prefix is already indexed with another block.

### 7.2 Results

100k-request traces. LRU and FIFO give the same values to the digits shown, so one
"on" column is given. "Share of live storage" is the empty slots divided by all slots
of the allocated blocks with sharing off, where every allocated block belongs to a
running request.

| Mode | Capacity | Empty slots, on (mean) | Empty slots, off (mean) | Largest sample, on / off | Live requests, on / off (mean) | Empty slots per live request, on / off | Share of live storage (off) | Share of the pool (on) | Duplicate blocks |
|---|---|---|---|---|---|---|---|---|---|
| `unique` | 256 | 178.5 | 177.5 | 296 / 288 | 24.4 / 24.3 | 7.32 / 7.32 | 4.5% | 4.4% | 0 |
| `unique` | 1,024 | 237.4 | 237.5 | 349 / 349 | 31.6 / 31.6 | 7.50 / 7.50 | 4.3% | 1.4% | 0 |
| `unique` | 4,096 | 237.4 | 237.5 | 349 / 349 | 31.6 / 31.6 | 7.50 / 7.50 | 4.3% | 0.4% | 0 |
| `system` | 256 | 237.0 | 132.2 | 348 / 241 | 31.6 / 18.4 | 7.49 / 7.19 | 3.3% | 5.8% | 0 |
| `system` | 1,024 | 237.1 | 237.2 | 339 / 339 | 31.6 / 31.6 | 7.49 / 7.49 | 3.4% | 1.4% | 0 |
| `system` | 4,096 | 237.1 | 237.2 | 339 / 339 | 31.6 / 31.6 | 7.49 / 7.49 | 3.4% | 0.4% | 0 |
| `multiturn` | 256 | 210.0 | 209.1 | 319 / 326 | 28.4 / 28.3 | 7.40 / 7.40 | 5.5% | 5.1% | 0 |
| `multiturn` | 1,024 | 237.4 | 237.4 | 348 / 348 | 31.7 / 31.7 | 7.50 / 7.50 | 5.2% | 1.4% | 0 |
| `multiturn` | 4,096 | 237.4 | 237.4 | 348 / 348 | 31.7 / 31.7 | 7.50 / 7.50 | 5.2% | 0.4% | 0 |
| `skewed` | 256 | 155.6 | 132.3 | 274 / 220 | 21.5 / 18.4 | 7.25 / 7.19 | 3.3% | 3.8% | 0 |
| `skewed` | 1,024 | 237.4 | 237.4 | 346 / 346 | 31.7 / 31.7 | 7.50 / 7.50 | 3.4% | 1.4% | 0 |
| `skewed` | 4,096 | 237.4 | 237.4 | 346 / 346 | 31.7 / 31.7 | 7.50 / 7.50 | 3.4% | 0.4% | 0 |
| `skewed_short` | 256 | 15.1 | 15.1 | 126 / 126 | 2.1 / 2.1 | 7.28 / 7.30 | 5.0% | 0.4% | 0 |
| `skewed_short` | 1,024 | 15.1 | 15.1 | 126 / 126 | 2.1 / 2.1 | 7.28 / 7.30 | 5.0% | 0.09% | 0 |
| `skewed_short` | 4,096 | 15.1 | 15.1 | 126 / 126 | 2.1 / 2.1 | 7.28 / 7.30 | 5.0% | 0.02% | 0 |

Findings:

- **Fragmentation is about 7.5 empty slots per running request, whatever the workload,
  capacity, sharing mode or policy** (7.2 to 7.5). That is what one partial last block
  per request gives: its fill is anywhere from 1 to 16 tokens, so on average
  (16 - 1) / 2 = 7.5 slots are empty.
- **It follows the number of running requests, not the capacity.** With 31.6 requests
  running on average it is 237 slots, about 15 blocks' worth. It is lower only where
  fewer requests run: at capacity 256, where requests are rejected, and in
  `skewed_short`, whose requests are short-lived (2.1 running on average).
- **Sharing changes it only through admission.** Where both modes admit every request,
  on and off agree within 0.1 slot. At capacity 256 sharing admits more requests
  (`system`: 31.6 running against 18.4), so it also has more partial tails: 237 slots
  against 132. That is more waste in absolute terms but for more work done.
- **As a share of storage it is 3.3-5.5%** of the slots held by running requests.
  With sharing on the pool is kept full of cached blocks, which are all full, so the
  share of the whole pool is 0.4% at capacity 4,096.
- **No duplicate block was sampled in any of the 180 runs.**

`fragmentation_vs_capacity.png` shows the mean empty slots by capacity.

### 7.3 Where the waste comes from

- **Partial tail blocks.** Blocks are allocated whole, and a request's last block is
  usually not full. This is the only source that shows up in the numbers above, and
  it is internal fragmentation in the usual sense. There is no external fragmentation:
  all blocks have the same size and any free block can serve any request.
- **Copy-on-write copies.** A fork shares its source's partial tail, so right after a
  fork the two requests have one partial block between them, not two. The first
  APPEND to either copies the tail, and from then on each has its own. So
  copy-on-write does not add waste beyond "one partial tail per request"; sharing
  only delays it. With sharing off the fork copies everything at once. Forks are 2%
  of requests (923 to 1,866 copies per 100k-request trace), which is why on and off
  differ by at most 0.1 slot.
- **Private duplicates.** Only full blocks are indexed, and only the first block for a
  given prefix. If a second request fills a block with the same prefix, for example
  two forks of one request that generate the same tokens, its block stays private and
  the content is stored twice until that request is released. The unit test
  `waste_tests` in `tests/manager_tests.cpp` builds this case. In the generated
  traces appended tokens are random, so it did not occur once.
- **Not counted here:** with sharing off, every request stores its own copy of a
  prefix it shares with others (the 128-token system prompt is stored once per running
  request). That duplication is what sharing removes; it shows up as uncached tokens
  and peak blocks in section 5.1, not as fragmentation. The manager cannot detect it
  with sharing off, so `duplicate_blocks` is 0 there by definition.
- **The partial tail is also never shared through the index:** two requests with the
  same 20-token prompt share the first 16 tokens and each write the last 4. This costs
  recomputation (uncached tokens), not empty slots.

The block size sets the trade: with B tokens per block the expected waste is
(B - 1) / 2 slots per running request, and sharing works in units of B tokens. Only
B = 16 was measured.

## 8. Limitations

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
- **No profiler was used.** The scan's cost is established by the scan / count
  comparison in section 6, but the remaining growth of the `unique` ADMIT median with
  capacity and the slower RELEASE in the count sweep are unexplained.
- **Fragmentation is sampled** every 64 operations in a separate run (section 7), not
  tracked exactly; the "largest" value is the largest sample.
- **Fake payloads.** Stored bytes are derived from token prefixes; no model runs.
  Latencies are those of the bookkeeping only.

## 9. Figures

`scripts/plot.py` writes two sets from the same data:

- **Report set**, `results/figures/report/` (`plot.py --report`): 7 inches wide for a
  single-column report, with three panels: `system`, `multiturn` and `skewed_short`.
- **Appendix set**, `results/figures/` (`plot.py`): one panel per workload, all five.

Unless noted, a figure has one panel per workload and three series: sharing on with
LRU, sharing on with FIFO, and sharing off. The capacity figures use the 100k traces.

| File | Set | What it shows |
|---|---|---|
| `hit_rate_vs_capacity.png` | both | Share of prompt tokens served from the cache at each capacity; the only panel where the LRU and FIFO lines separate is `skewed_short`. |
| `uncached_tokens_vs_capacity.png` | both | Uncached prompt tokens per admitted request (recomputation proxy) at each capacity, each panel on its own scale; dividing by admitted requests keeps configurations with different admission rates comparable. |
| `uncached_tokens_vs_capacity_total.png` | both | The same as an absolute count over the whole trace; a lower total can mean fewer admitted requests, so read it next to the admitted share. |
| `admitted_vs_capacity.png` | both | Share of ADMITs accepted at each capacity; shows the saturation at 256 blocks and where sharing admits more. |
| `admit_latency_vs_capacity.png` | both | ADMIT median (top) and p99 (bottom) at each capacity, sharing on with LRU against sharing off, with the interquartile range over repetitions. A third series, drawn with `--compare`, is the same sweep with `reclaimable=scan`: it grows with capacity, the count does not (section 6). |
| `lru_minus_fifo.png` | both | For `skewed` and `skewed_short`: LRU minus FIFO hit rate in percentage points (left) and LRU minus FIFO evictions as a percentage of FIFO's (right) at each capacity. Two panels by measure, two series by workload; identical in both sets apart from size. |
| `latency_vs_size.png` | both | Median latency of ADMIT, APPEND, FORK and RELEASE by trace size at capacity 1,024, with the interquartile range over repetitions, on a log scale. |
| `fragmentation_vs_capacity.png` | both | Empty token slots in allocated blocks, mean over the run, at each capacity (section 7). Drawn from `results/raw/fragmentation.csv`, not from the latency sweep. |
| `peak_blocks_vs_capacity.png` | **appendix only** | Most blocks allocated at once; sharing on fills the pool with cached blocks, sharing off shows live demand. Not drawn with `--report`: every sharing-on point simply equals the capacity. |

## 10. Reproducing

From the repository root. Each full sweep takes about 35 minutes on the machine above
and the sampled run about 8; together they write about 820 MB of traces into `traces/`
(ignored by git) once.

```sh
# 1. Build (Release, -O2) and run the tests.
make test
make gen-test        # generator tests; needs python3
make csv-test        # driver CSV test; needs cmake

# 2. The sweeps, as they were run. --overwrite replaces the committed CSVs.
#    Run the first two back to back without rebuilding in between.
bash scripts/run_experiments.sh --reclaimable scan \
    --out results/raw/scan_sweep.csv --overwrite
bash scripts/run_experiments.sh --out results/raw/sweep.csv --overwrite
bash scripts/run_experiments.sh --reps 1 --sample-every 64 \
    --out results/raw/fragmentation.csv --overwrite

# 3. Figures. Needs matplotlib.
python3 -m pip install matplotlib      # or: sudo apt install python3-matplotlib
python3 scripts/plot.py \
    --compare "Sharing on, LRU, O(capacity) scan=results/raw/scan_sweep.csv"   # -> results/figures/
python3 scripts/plot.py --report \
    --compare "Sharing on, LRU, O(capacity) scan=results/raw/scan_sweep.csv"   # -> results/figures/report/
```

`plot.py` pools every `results/raw/sweep*.csv`, so the scan sweep and the sampled run
have names that do not match. `--compare` takes one CSV and draws its sharing-on LRU
rows as a third series in `admit_latency_vs_capacity.png`. The fragmentation figure
reads `results/raw/fragmentation.csv` (`--fragmentation` to change it).

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
