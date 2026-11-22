#!/usr/bin/env bash
# Runs the evaluation sweep of proposal section 6 and writes one CSV row per run.
#
#   scripts/run_experiments.sh                 full sweep (900 runs)
#   scripts/run_experiments.sh --quick         1k traces, 1 repetition (60 runs)
#   scripts/run_experiments.sh --dry-run       print the plan and the time estimate only
#   scripts/run_experiments.sh --sizes "1k 10k" --modes system --reps 3 --out results/raw/x.csv
#   scripts/run_experiments.sh --reclaimable scan --out results/raw/scan_sweep.csv
#
# --reclaimable scan|count (default count) sets the config key of the same name:
# how the manager counts the blocks eviction could free. Give a scan sweep a
# file name that does not match results/raw/sweep*.csv, which plot.py pools.
#
# Sweep: sharing {off,on} x policy {lru,fifo} x capacity {256,1024,4096} x
# mode {unique,system,multiturn,skewed,skewed_short} x size {1k,10k,100k}, 5
# repetitions (the first three modes are the proposal's; see gen_trace.py), with
# tokens_per_block=16 and bytes_per_token=64. Runs are sequential (they are
# timed) and use the Release build (-O2) with --no-check.
#
# Traces are generated once into traces/ (ignored by git) and reused. Next to
# the CSV, <name>.meta.txt records hardware, compiler, flags, commit and seeds.
set -euo pipefail

cd "$(dirname "$0")/.."

SHARING="off on"
POLICIES="lru fifo"
CAPACITIES="256 1024 4096"
MODES="unique system multiturn skewed skewed_short"
SIZES="1k 10k 100k"
REPS=5
RECLAIMABLE=count           # count (tree's O(1) count) or scan (O(capacity) scan)
SEED=1                      # trace generator seed; the payload seed is fixed at 42
OUT=results/raw/sweep.csv
TRACE_DIR=traces
BUILD=build-make
DRY_RUN=0
OVERWRITE=0

usage() { sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
  case "$1" in
    --sharing)    SHARING=$2; shift 2 ;;
    --policies)   POLICIES=$2; shift 2 ;;
    --capacities) CAPACITIES=$2; shift 2 ;;
    --modes)      MODES=$2; shift 2 ;;
    --sizes)      SIZES=$2; shift 2 ;;
    --reps)       REPS=$2; shift 2 ;;
    --seed)       SEED=$2; shift 2 ;;
    --reclaimable) RECLAIMABLE=$2; shift 2 ;;
    --out)        OUT=$2; shift 2 ;;
    --trace-dir)  TRACE_DIR=$2; shift 2 ;;
    --quick)      SIZES="1k"; REPS=1; shift ;;
    --dry-run)    DRY_RUN=1; shift ;;
    --overwrite)  OVERWRITE=1; shift ;;
    -h|--help)    usage; exit 0 ;;
    *) echo "unknown option '$1'" >&2; usage >&2; exit 2 ;;
  esac
done

requests_of() {
  case "$1" in
    1k) echo 1000 ;; 10k) echo 10000 ;; 100k) echo 100000 ;;
    *) echo "unknown size '$1' (use 1k, 10k or 100k)" >&2; exit 2 ;;
  esac
}
# Seconds for one run, from the full sweep on the development machine (WSL2,
# -O2, traces read from a Windows drive): 540 runs took 22 minutes.
seconds_of() { case "$1" in 1k) echo 0.06 ;; 10k) echo 0.7 ;; 100k) echo 6.6 ;; esac; }

for value in $SHARING;    do case $value in on|off) ;; *) echo "bad sharing '$value'" >&2; exit 2 ;; esac; done
for value in $POLICIES;   do case $value in lru|fifo) ;; *) echo "bad policy '$value'" >&2; exit 2 ;; esac; done
for value in $MODES;      do case $value in unique|system|multiturn|skewed|skewed_short) ;; *) echo "bad mode '$value'" >&2; exit 2 ;; esac; done
for value in $CAPACITIES; do case $value in ''|*[!0-9]*) echo "bad capacity '$value'" >&2; exit 2 ;; esac; done
case $RECLAIMABLE in scan|count) ;; *) echo "bad --reclaimable '$RECLAIMABLE' (use scan or count)" >&2; exit 2 ;; esac
case $REPS in ''|*[!0-9]*|0) echo "bad --reps '$REPS'" >&2; exit 2 ;; esac
for size in $SIZES; do requests_of "$size" > /dev/null; done

count() { echo $#; }
per_size=$(( $(count $SHARING) * $(count $POLICIES) * $(count $CAPACITIES) * $(count $MODES) * REPS ))
total_runs=$(( per_size * $(count $SIZES) ))
estimate=$(for size in $SIZES; do echo "$per_size $(seconds_of "$size")"; done |
           awk '{ s += $1 * $2 } END { printf "%.0f", s }')

echo "Sweep: sharing {$SHARING} x policy {$POLICIES} x capacity {$CAPACITIES}"
echo "       x mode {$MODES} x size {$SIZES} x $REPS repetitions = $total_runs runs"
echo "Estimated time: about $(( estimate / 60 )) min $(( estimate % 60 )) s, plus trace generation"
echo "                (roughly 5 s and 150 MB per 100k trace)."
echo "Reclaimable blocks counted by: $RECLAIMABLE"
echo "Output: $OUT"
if [ "$DRY_RUN" = 1 ]; then exit 0; fi

if [ -e "$OUT" ] && [ "$OVERWRITE" != 1 ]; then
  echo "error: $OUT exists; pass --overwrite or choose another --out" >&2
  exit 1
fi

make "$BUILD/kvcache" > /dev/null
mkdir -p "$TRACE_DIR" "$(dirname "$OUT")"
rm -f "$OUT"

for size in $SIZES; do
  for mode in $MODES; do
    trace="$TRACE_DIR/${mode}_${size}.trace"
    # The first line of a trace records its generator settings, including the seed.
    if [ ! -s "$trace" ] || ! head -n 1 "$trace" | grep -q " seed=$SEED "; then
      echo "generating $trace"
      python3 scripts/gen_trace.py --mode "$mode" --requests "$(requests_of "$size")" \
        --seed "$SEED" --out "$trace.tmp"
      mv "$trace.tmp" "$trace"
    fi
  done
done

meta="${OUT%.csv}.meta.txt"
{
  echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "commit: $(git rev-parse HEAD 2>/dev/null || echo unknown)$(git diff --quiet 2>/dev/null || echo ' (uncommitted changes)')"
  echo "system: $(uname -srm)"
  echo "cpu: $( (grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ //') || true)$(sysctl -n machdep.cpu.brand_string 2>/dev/null || true)"
  echo "memory: $( (grep -m1 MemTotal /proc/meminfo 2>/dev/null | awk '{print $2, $3}') || true)"
  echo "compiler: $(${CXX:-c++} --version | head -n 1)"
  # grep -m1 closes the pipe early, so the pipeline's status is not meaningful.
  flags=$(make -pn "$BUILD/kvcache" 2>/dev/null | grep -m1 '^CXXFLAGS' || true)
  echo "flags: ${flags:-unknown}"
  echo "driver flags: --no-check, standard output to /dev/null"
  echo "tokens_per_block: 16, bytes_per_token: 64, payload seed: 42"
  echo "reclaimable: $RECLAIMABLE"
  echo "trace seed: $SEED, repetitions: $REPS"
  echo "sharing: $SHARING | policies: $POLICIES | capacities: $CAPACITIES"
  echo "modes: $MODES | sizes: $SIZES"
  for size in $SIZES; do for mode in $MODES; do
    echo "trace ${mode}_${size}: $(head -n 1 "$TRACE_DIR/${mode}_${size}.trace")"
  done; done
} > "$meta"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
done_runs=0
started=$(date +%s)
# Repetitions are the outer loop, so slow drift of the machine is spread over
# all settings instead of hitting one of them five times in a row.
for rep in $(seq 1 "$REPS"); do
  for size in $SIZES; do
    for mode in $MODES; do
      for capacity in $CAPACITIES; do
        for sharing in $SHARING; do
          for policy in $POLICIES; do
            config="$work/run.cfg"
            printf 'capacity=%s\ntokens_per_block=16\nbytes_per_token=64\npolicy=%s\nsharing=%s\nreclaimable=%s\nseed=42\n' \
              "$capacity" "$policy" "$sharing" "$RECLAIMABLE" > "$config"
            "./$BUILD/kvcache" --config "$config" --trace "$TRACE_DIR/${mode}_${size}.trace" \
              --csv "$OUT" --no-check > /dev/null
            done_runs=$(( done_runs + 1 ))
          done
        done
      done
    done
    echo "repetition $rep/$REPS, size $size done: $done_runs/$total_runs runs, $(( $(date +%s) - started )) s elapsed"
  done
done
echo "wrote $(( $(wc -l < "$OUT") - 1 )) rows to $OUT and settings to $meta"
