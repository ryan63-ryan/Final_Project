#!/usr/bin/env python3
"""Plots the sweep written by scripts/run_experiments.sh.

  python3 scripts/plot.py                         # results/raw/sweep*.csv -> results/figures/
  python3 scripts/plot.py --csv a.csv b.csv --out results/figures --size 10k --capacity 1024

Rows from all given CSVs are pooled. Figures (one PNG each, small multiples with
one panel per workload found in the data):

  hit_rate_vs_capacity.png         cached / prompt tokens of admitted requests
  uncached_tokens_vs_capacity.png  prompt tokens written fresh (recomputation proxy)
  admitted_vs_capacity.png         share of ADMITs accepted (context for the two above)
  peak_blocks_vs_capacity.png      most blocks allocated at once
  latency_vs_size.png              per-operation latency: median and IQR over the repetitions

Series: sharing on with LRU, sharing on with FIFO, and sharing off. With sharing
off the prefix index is never used, so the policy cannot matter; the LRU and
FIFO runs are pooled into one "sharing off" series, and the script checks that
their counters really are identical.

Counters (hit rate, tokens, blocks) are deterministic, so repetitions agree and
one value is drawn. Latency differs between repetitions: the point is the median
of the per-run medians and the bar spans their interquartile range.

Needs matplotlib. The CSV is read with the standard library.
"""

import argparse
import csv
import glob
import os
import statistics
import sys
from collections import defaultdict

MODES = ["unique", "system", "multiturn", "skewed", "skewed_short"]
MODE_TITLES = {"unique": "Unique prompts", "system": "Shared system prefix",
               "multiturn": "Multi-turn conversations", "skewed": "Zipf prefixes",
               "skewed_short": "Zipf prefixes, short turns"}
SIZES = ["1k", "10k", "100k"]
OPS = ["admit", "append", "fork", "release"]

# Series identity is carried by color, marker and line style together, so the
# figures survive grayscale printing and color-vision deficiency. The three hues
# are the first three slots of a palette validated for all-pairs separation.
SERIES = [
    # key (sharing, policy or None), label, color, marker, line style, hollow marker
    (("on", "lru"), "Sharing on, LRU", "#2a78d6", "o", "-", False),
    (("on", "fifo"), "Sharing on, FIFO", "#eb6834", "s", "--", True),
    (("off", None), "Sharing off", "#1baf7a", "^", ":", False),
]
SURFACE = "#fcfcfb"
TEXT = "#0b0b0b"
TEXT_SECONDARY = "#52514e"
GRID = "#e4e3df"

COUNTERS = ["admitted", "rejected", "errors", "skipped_ops", "prompt_tokens", "cached_tokens",
            "uncached_tokens", "cow_copies", "evictions", "peak_used_blocks"]


def load(paths):
    """Returns {(mode, size, capacity, series key): [row, ...]} with numeric fields."""
    groups = defaultdict(list)
    for path in paths:
        with open(path, newline="") as source:
            rows = list(csv.DictReader(source))
        for row in rows:
            name = row["trace"]
            stem = name[:-len(".trace")] if name.endswith(".trace") else name
            mode, _, size = stem.rpartition("_")
            if mode not in MODES or size not in SIZES:
                sys.exit("error: trace '%s' is not named <mode>_<size>.trace" % name)
            for key, value in row.items():
                if key not in ("policy", "sharing", "trace"):
                    row[key] = float(value)
            sharing = row["sharing"]
            series = (sharing, row["policy"] if sharing == "on" else None)
            groups[(mode, size, int(row["capacity"]), series)].append(row)
    if not groups:
        sys.exit("error: no rows in %s" % ", ".join(paths))
    return groups


def check_deterministic(groups):
    """Counters must not differ between repetitions, nor between policies when sharing is off."""
    for key, rows in groups.items():
        for counter in COUNTERS:
            if len({row[counter] for row in rows}) != 1:
                sys.exit("error: '%s' differs between runs of %s; the counters are expected "
                         "to be deterministic" % (counter, key))


def counter(groups, mode, size, capacity, series, compute):
    rows = groups.get((mode, size, capacity, series))
    return compute(rows[0]) if rows else None


def quartiles(values):
    """Median and the 25th / 75th percentiles (inclusive method) of a small sample."""
    values = sorted(values)
    if len(values) == 1:
        return values[0], values[0], values[0]
    low, middle, high = statistics.quantiles(values, n=4, method="inclusive")
    return middle, low, high


def style(plt):
    plt.rcParams.update({
        "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
        "font.size": 10, "text.color": TEXT, "axes.labelcolor": TEXT_SECONDARY,
        "axes.titlesize": 11, "axes.titleweight": "bold", "axes.titlecolor": TEXT,
        "xtick.color": TEXT_SECONDARY, "ytick.color": TEXT_SECONDARY,
        "axes.edgecolor": GRID, "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.8,
        "axes.spines.top": False, "axes.spines.right": False, "axes.axisbelow": True,
        "lines.linewidth": 2, "lines.markersize": 7, "legend.frameon": False,
    })


def draw(axis, xs, ys, series, lows=None, highs=None):
    _, label, color, marker, line, hollow = series
    points = [(x, y, i) for i, (x, y) in enumerate(zip(xs, ys)) if y is not None]
    if not points:
        return
    px = [p[0] for p in points]
    py = [p[1] for p in points]
    face = SURFACE if hollow else color
    if lows is not None:
        errors = [[py[k] - lows[p[2]] for k, p in enumerate(points)],
                  [highs[p[2]] - py[k] for k, p in enumerate(points)]]
        axis.errorbar(px, py, yerr=errors, color=color, linestyle=line, marker=marker,
                      markerfacecolor=face, markeredgecolor=color, markeredgewidth=1.6,
                      capsize=3, elinewidth=1.2, label=label)
    else:
        axis.plot(px, py, color=color, linestyle=line, marker=marker, markerfacecolor=face,
                  markeredgecolor=color, markeredgewidth=1.6, label=label)


def finish(plt, figure, axes, title, note, path):
    handles, labels = axes[0].get_legend_handles_labels()
    figure.legend(handles, labels, loc="upper center", ncol=len(labels),
                  bbox_to_anchor=(0.5, 0.93), handlelength=3.2)
    figure.suptitle(title, fontsize=13, fontweight="bold", color=TEXT, y=0.99)
    figure.text(0.5, 0.01, note, ha="center", va="bottom", fontsize=8.5, color=TEXT_SECONDARY)
    figure.savefig(path, dpi=160)
    plt.close(figure)
    print("wrote", path)


def capacity_figure(plt, groups, capacities, size, compute, ylabel, title, note, path,
                    percent=False, zero_based=True):
    from matplotlib.ticker import FuncFormatter, PercentFormatter
    modes = [m for m in MODES if any(key[0] == m and key[1] == size for key in groups)]
    figure, axes = plt.subplots(1, len(modes), figsize=(3.85 * len(modes), 4.2), sharey=True,
                                squeeze=False)
    axes = list(axes[0])
    positions = list(range(len(capacities)))  # evenly spaced: the capacities are 4x apart
    for axis, mode in zip(axes, modes):
        for series in SERIES:
            ys = [counter(groups, mode, size, capacity, series[0], compute)
                  for capacity in capacities]
            draw(axis, positions, ys, series)
        axis.set_title(MODE_TITLES[mode])
        axis.set_xticks(positions)
        axis.set_xticklabels(["{:,}".format(c) for c in capacities])
        axis.set_xlabel("Capacity (blocks)")
        axis.set_xlim(-0.3, len(capacities) - 0.7)
        if zero_based:
            axis.set_ylim(bottom=0)
        if percent:
            axis.set_ylim(-0.03, 1.03)
            axis.yaxis.set_major_formatter(PercentFormatter(1.0, decimals=0))
        else:
            axis.yaxis.set_major_formatter(FuncFormatter(lambda v, _: "{:,.0f}".format(v)))
    axes[0].set_ylabel(ylabel)
    figure.subplots_adjust(left=0.92 / (3.85 * len(modes)), right=0.985, top=0.78, bottom=0.2,
                           wspace=0.08)
    finish(plt, figure, axes, title, note, path)


def latency_figure(plt, groups, sizes, capacity, path):
    from matplotlib.ticker import FuncFormatter, LogLocator, NullFormatter
    modes = [m for m in MODES if any(key[0] == m and key[2] == capacity for key in groups)]
    figure, axes = plt.subplots(len(OPS), len(modes), figsize=(3.85 * len(modes), 10.5),
                                sharex=True, sharey="row", squeeze=False)
    positions = list(range(len(sizes)))
    for row, op in enumerate(OPS):
        drawn = []  # every value in this row, to choose a common y range
        for column, mode in enumerate(modes):
            axis = axes[row][column]
            for series in SERIES:
                middles, lows, highs = [], [], []
                for size in sizes:
                    rows = groups.get((mode, size, capacity, series[0]), [])
                    # A run with no operation of this type has no latency to report.
                    samples = [r[op + "_median_ns"] for r in rows if r[op + "_ops"] > 0]
                    middle, low, high = quartiles(samples) if samples else (None, 0, 0)
                    middles.append(middle)
                    lows.append(low)
                    highs.append(high)
                    if middle is not None:
                        drawn += [low, high]
                draw(axis, positions, middles, series, lows, highs)
            axis.set_yscale("log")
            axis.yaxis.set_major_locator(LogLocator(base=10, subs=(1.0, 2.0, 5.0)))
            axis.yaxis.set_major_formatter(FuncFormatter(lambda v, _: "{:,.0f}".format(v)))
            axis.yaxis.set_minor_formatter(NullFormatter())
            axis.set_xticks(positions)
            axis.set_xticklabels(sizes)
            axis.set_xlim(-0.3, len(sizes) - 0.7)
            if row == 0:
                axis.set_title(MODE_TITLES[mode])
            if row == len(OPS) - 1:
                axis.set_xlabel("Trace size (requests)")
            if column == 0:
                axis.set_ylabel("%s median (ns)" % op.upper())
        if drawn and min(drawn) > 0:
            # Show at least a factor of 4, so a difference of a few nanoseconds
            # (the clock's own resolution) is not stretched over the whole panel.
            low, high = min(drawn), max(drawn)
            middle = (low * high) ** 0.5
            span = max((high / low) ** 0.5 * 1.25, 2.0)
            axes[row][0].set_ylim(middle / span, middle * span)
    figure.subplots_adjust(left=1.05 / (3.85 * len(modes)), right=0.985, top=0.9, bottom=0.07,
                           wspace=0.08,
                           hspace=0.14)
    handles, labels = axes[0][0].get_legend_handles_labels()
    figure.legend(handles, labels, loc="upper center", ncol=len(labels),
                  bbox_to_anchor=(0.5, 0.965), handlelength=3.2)
    figure.suptitle("Operation latency by trace size, capacity {:,} blocks".format(capacity),
                    fontsize=13, fontweight="bold", color=TEXT, y=0.99)
    figure.text(0.5, 0.01,
                "Point: median over the repetitions of each run's median latency. Bar: "
                "interquartile range of those medians. Log scale. Manager call only; "
                "parsing, checks and output excluded.",
                ha="center", va="bottom", fontsize=8.5, color=TEXT_SECONDARY)
    figure.savefig(path, dpi=160)
    plt.close(figure)
    print("wrote", path)


def main():
    parser = argparse.ArgumentParser(description="Plot a kvcache sweep.")
    parser.add_argument("--csv", nargs="+", default=None,
                        help="one or more sweep CSVs (default: results/raw/sweep*.csv)")
    parser.add_argument("--out", default="results/figures")
    parser.add_argument("--size", default=None,
                        help="trace size for the capacity figures (default: largest in the CSV)")
    parser.add_argument("--capacity", type=int, default=None,
                        help="capacity for the latency figure (default: middle one in the CSV)")
    args = parser.parse_args()

    try:
        import matplotlib
    except ImportError:
        sys.exit("error: matplotlib is not installed (try: python3 -m pip install matplotlib)")
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    paths = args.csv or sorted(glob.glob("results/raw/sweep*.csv"))
    if not paths:
        sys.exit("error: no results/raw/sweep*.csv; run scripts/run_experiments.sh or pass --csv")
    groups = load(paths)
    check_deterministic(groups)
    sizes = [s for s in SIZES if any(key[1] == s for key in groups)]
    capacities = sorted({key[2] for key in groups})
    size = args.size or sizes[-1]
    capacity = args.capacity or capacities[len(capacities) // 2]
    if size not in sizes:
        sys.exit("error: no rows for size %s" % size)
    if capacity not in capacities:
        sys.exit("error: no rows for capacity %d" % capacity)
    repetitions = max(len(rows) for rows in groups.values())
    os.makedirs(args.out, exist_ok=True)
    style(plt)
    scope = "%s-request traces" % size

    capacity_figure(
        plt, groups, capacities, size,
        lambda r: r["cached_tokens"] / r["prompt_tokens"] if r["prompt_tokens"] else 0.0,
        "Prompt tokens served from cache", "Prefix-cache hit rate by capacity, " + scope,
        "Cached / prompt tokens of admitted requests. Only full 16-token blocks are shared, "
        "and sharing off never caches.",
        os.path.join(args.out, "hit_rate_vs_capacity.png"), percent=True)
    capacity_figure(
        plt, groups, capacities, size, lambda r: r["uncached_tokens"] / 1e6,
        "Uncached prompt tokens (millions)", "Uncached prompt tokens by capacity, " + scope,
        "Prompt tokens written fresh: a proxy for recomputation. Rejected requests write "
        "nothing, so read this next to the admitted share.",
        os.path.join(args.out, "uncached_tokens_vs_capacity.png"))
    capacity_figure(
        plt, groups, capacities, size,
        lambda r: r["admitted"] / r["admit_ops"] if r["admit_ops"] else 0.0,
        "ADMITs accepted", "Admitted share by capacity, " + scope,
        "Accepted ADMITs / attempted ADMITs. A rejected request's later commands are skipped.",
        os.path.join(args.out, "admitted_vs_capacity.png"), percent=True)
    capacity_figure(
        plt, groups, capacities, size, lambda r: r["peak_used_blocks"],
        "Peak blocks in use", "Peak blocks in use by capacity, " + scope,
        "With sharing on, cached blocks stay allocated until evicted, so the pool fills. "
        "Sharing off shows the live demand alone.",
        os.path.join(args.out, "peak_blocks_vs_capacity.png"))
    latency_figure(plt, groups, sizes, capacity, os.path.join(args.out, "latency_vs_size.png"))
    print("%d configurations, up to %d repetitions each" % (len(groups), repetitions))


if __name__ == "__main__":
    main()
