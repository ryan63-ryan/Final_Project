#!/usr/bin/env python3
"""Plots the sweeps written by scripts/run_experiments.sh.

  python3 scripts/plot.py              # appendix figures -> results/figures/
  python3 scripts/plot.py --report     # report figures   -> results/figures/report/
  python3 scripts/plot.py --csv a.csv b.csv --out some/dir --size 10k --capacity 1024
  python3 scripts/plot.py --compare "Sharing on, O(1) count=results/raw/o1_sweep.csv"

Rows from all --csv files are pooled (default: results/raw/sweep*.csv).

Two layouts:

  appendix (default)  one panel per workload found in the data, about 3.9 inches each.
  --report            only the system, multiturn and skewed_short panels, 7 inches wide
                      in total with fonts sized to be read at that width, for a
                      single-column report. peak_blocks is not drawn in this layout.

Figures:

  hit_rate_vs_capacity.png           cached / prompt tokens of admitted requests
  uncached_tokens_vs_capacity.png    uncached prompt tokens per admitted request
  uncached_tokens_vs_capacity_total.png   the same as an absolute count
  admitted_vs_capacity.png           share of ADMITs accepted
  admit_latency_vs_capacity.png      ADMIT median and p99, sharing on (LRU) vs off
  lru_minus_fifo.png                 LRU minus FIFO: hit rate and evictions, skewed workloads
  latency_vs_size.png                per-operation latency by trace size
  peak_blocks_vs_capacity.png        most blocks allocated at once (appendix only)

Series in the per-workload figures: sharing on with LRU, sharing on with FIFO, and
sharing off. With sharing off the prefix index is never used, so the policy cannot
matter; the LRU and FIFO runs are pooled into one "sharing off" series, and the
script checks that their counters really are identical.

Counters (hit rate, tokens, blocks) are deterministic, so repetitions agree and
one value is drawn. Latency differs between repetitions: a point is the median
over the repetitions of the per-run value and its bar spans their interquartile
range.

--compare LABEL=FILE adds a later sweep of the same traces, made with a changed
build, to admit_latency_vs_capacity.png as one more series (its sharing-on LRU
rows). Its rows are kept apart from the pooled ones, and the file is left out of
the default results/raw/sweep*.csv pool. Give the option once per build.

Needs matplotlib. The CSVs are read with the standard library.
"""

import argparse
import csv
import glob
import math
import os
import statistics
import sys
import textwrap
from collections import defaultdict

MODES = ["unique", "system", "multiturn", "skewed", "skewed_short"]
REPORT_MODES = ["system", "multiturn", "skewed_short"]
MODE_TITLES = {"unique": "Unique prompts", "system": "Shared system prefix",
               "multiturn": "Multi-turn conversations", "skewed": "Zipf prefixes",
               "skewed_short": "Zipf prefixes, short turns"}
# Narrow panels in the report layout need shorter titles.
SHORT_TITLES = {"unique": "Unique", "system": "System prefix", "multiturn": "Multi-turn",
                "skewed": "Zipf", "skewed_short": "Zipf, short turns"}
SIZES = ["1k", "10k", "100k"]
OPS = ["admit", "append", "fork", "release"]

# A series is (data key, label, color, marker, line style, hollow marker).
# Identity is carried by color, marker and line style together, so the figures
# survive grayscale printing and color-vision deficiency. The hues come from a
# palette whose combinations used here were checked for all-pairs separation;
# a color always means the same thing across figures.
ON_LRU = (("on", "lru"), "Sharing on, LRU", "#2a78d6", "o", "-", False)
ON_FIFO = (("on", "fifo"), "Sharing on, FIFO", "#eb6834", "s", "--", True)
OFF = (("off", None), "Sharing off", "#1baf7a", "^", ":", False)
SERIES = [ON_LRU, ON_FIFO, OFF]
# Later builds overlaid with --compare, in the order given.
COMPARE_STYLES = [("#eda100", "D", "-.", False), ("#e34948", "v", "--", True)]
# lru_minus_fifo.png draws workloads, not configurations: its own two hues.
WORKLOAD_SERIES = [
    ("skewed", "Zipf prefixes", "#008300", "s", "--", True),
    ("skewed_short", "Zipf prefixes, short turns", "#6250d6", "o", "-", False),
]
SURFACE = "#fcfcfb"
TEXT = "#0b0b0b"
TEXT_SECONDARY = "#52514e"
GRID = "#e4e3df"
ZERO_LINE = "#8a8983"

COUNTERS = ["admitted", "rejected", "errors", "skipped_ops", "prompt_tokens", "cached_tokens",
            "uncached_tokens", "cow_copies", "evictions", "peak_used_blocks"]


# --------------------------------------------------------------------- data

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


def spread(groups, key, column, scale=1.0, needs=None):
    """(median, q1, q3) of one column over the repetitions of a configuration, or None."""
    rows = groups.get(key, [])
    # A run with no operation of a type has no latency to report for it.
    samples = [row[column] * scale for row in rows if needs is None or row[needs] > 0]
    return quartiles(samples) if samples else None


# ------------------------------------------------------------------- layout

class Layout:
    """Sizes that differ between the appendix and the report figures."""

    def __init__(self, report):
        self.report = report
        self.titles = SHORT_TITLES if report else MODE_TITLES
        self.font = 9.5 if report else 10.0
        self.note_font = 8.0 if report else 8.5
        self.row_height = 2.15 if report else 2.9

    def modes(self, present):
        wanted = REPORT_MODES if self.report else MODES
        return [mode for mode in wanted if mode in present]

    def width(self, columns):
        return 7.0 if self.report else max(3.85 * columns, 7.0)

    def legend_columns(self, entries):
        return min(entries, 3) if self.report else entries


def style(plt, layout):
    plt.rcParams.update({
        "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
        "font.size": layout.font, "text.color": TEXT, "axes.labelcolor": TEXT_SECONDARY,
        "axes.titlesize": layout.font + 0.5, "axes.titleweight": "bold",
        "axes.titlecolor": TEXT, "axes.labelsize": layout.font,
        "xtick.labelsize": layout.font - 0.5, "ytick.labelsize": layout.font - 0.5,
        "legend.fontsize": layout.font,
        "xtick.color": TEXT_SECONDARY, "ytick.color": TEXT_SECONDARY,
        "axes.edgecolor": GRID, "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.8,
        "axes.spines.top": False, "axes.spines.right": False, "axes.axisbelow": True,
        "lines.linewidth": 2, "lines.markersize": 6.5 if layout.report else 7,
        "legend.frameon": False,
    })


class Figure:
    """A grid of panels with a title, one legend row block above and a note below.

    All margins are set in inches, so the same code lays out a 7-inch report
    figure and a 19-inch appendix figure without text running into the panels.
    """

    def __init__(self, plt, layout, rows, columns, note, legend_entries, sharey,
                 row_height=None, sharex=True):
        self.plt, self.layout = plt, layout
        self.width = layout.width(columns)
        # Characters that fit across the figure: about 122 / font size per inch,
        # which leaves a small margin on both sides.
        self.note = textwrap.wrap(note, int(self.width * 122 / layout.note_font))
        self.legend_columns = layout.legend_columns(legend_entries)
        legend_rows = math.ceil(legend_entries / self.legend_columns) if legend_entries else 0
        self.top = 0.36 + 0.25 * legend_rows + 0.30
        bottom = 0.52 + 0.16 * len(self.note) + 0.10
        height_each = row_height or layout.row_height
        gap = 0.28
        self.height = rows * height_each + (rows - 1) * gap + self.top + bottom
        self.figure, self.axes = plt.subplots(rows, columns, figsize=(self.width, self.height),
                                              sharex=sharex, sharey=sharey, squeeze=False)
        own_ticks = sharey in (False, "none")
        self.figure.subplots_adjust(
            left=0.80 / self.width, right=1 - 0.14 / self.width,
            top=1 - self.top / self.height, bottom=bottom / self.height,
            wspace=(0.36 if layout.report else 0.24) if own_ticks else 0.09,
            hspace=gap / height_each)

    def save(self, title, path, legend_axis=None):
        figure, height = self.figure, self.height
        figure.suptitle(title, fontsize=self.layout.font + 2, fontweight="bold", color=TEXT,
                        y=1 - 0.07 / height, va="top")
        axis = legend_axis if legend_axis is not None else self.axes[0][0]
        handles, labels = axis.get_legend_handles_labels()
        if handles:
            figure.legend(handles, labels, loc="upper center", ncol=self.legend_columns,
                          bbox_to_anchor=(0.5, 1 - 0.33 / height), handlelength=3.0,
                          columnspacing=1.6, borderaxespad=0.0)
        figure.text(0.5, 0.06 / height, "\n".join(self.note), ha="center", va="bottom",
                    fontsize=self.layout.note_font, color=TEXT_SECONDARY, linespacing=1.25)
        figure.savefig(path, dpi=200 if self.layout.report else 160)
        self.plt.close(figure)
        print("wrote", path)


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


def capacity_axis(axis, capacities, label=True):
    """Capacities are 4x apart, so they are drawn evenly spaced."""
    axis.set_xticks(range(len(capacities)))
    axis.set_xticklabels(["{:,}".format(c) for c in capacities])
    axis.set_xlim(-0.3, len(capacities) - 0.7)
    if label:
        axis.set_xlabel("Capacity (blocks)")


def plain(value, _=None):
    return "{:,.0f}".format(value)


# ------------------------------------------------------------------ figures

def capacity_figure(plt, layout, groups, capacities, size, compute, ylabel, title, note, path,
                    percent=False, sharey=True, formatter=plain):
    from matplotlib.ticker import FuncFormatter, PercentFormatter
    modes = layout.modes({key[0] for key in groups if key[1] == size})
    fig = Figure(plt, layout, 1, len(modes), note, len(SERIES), sharey)
    positions = list(range(len(capacities)))
    tops = {}  # largest value drawn in each panel
    for axis, mode in zip(fig.axes[0], modes):
        for series in SERIES:
            ys = [counter(groups, mode, size, capacity, series[0], compute)
                  for capacity in capacities]
            draw(axis, positions, ys, series)
            tops[mode] = max([tops.get(mode, 0.0)] + [y for y in ys if y is not None])
        axis.set_title(layout.titles[mode])
        capacity_axis(axis, capacities)
        if percent:
            axis.set_ylim(-0.03, 1.03)
            axis.yaxis.set_major_formatter(PercentFormatter(1.0, decimals=0))
        else:
            # Start at zero and leave headroom above the largest value (of this
            # panel, or of all panels when the axis is shared), so no marker is
            # cut off at the top.
            top = max(tops.values()) if sharey else tops[mode]
            axis.set_ylim(0, top * 1.12 if top > 0 else 1.0)
            axis.yaxis.set_major_formatter(FuncFormatter(formatter))
    fig.axes[0][0].set_ylabel(ylabel)
    fig.save(title, path)


def admit_latency_figure(plt, layout, groups, compare, capacities, size, path):
    """ADMIT median and p99 by capacity: sharing on (LRU) against off, plus later builds."""
    from matplotlib.ticker import FuncFormatter
    modes = layout.modes({key[0] for key in groups if key[1] == size})
    drawn = [(groups, ON_LRU), (groups, OFF)]
    for (label, other), look in zip(compare, COMPARE_STYLES):
        drawn.append((other, (("on", "lru"), label) + look))
    note = ("Time of the admit call alone, %s-request traces. Point: median over the "
            "repetitions of each run's median (top) or 99th percentile (bottom). Bar: "
            "interquartile range over the repetitions. Sharing on is shown for LRU only." % size)
    fig = Figure(plt, layout, 2, len(modes), note, len(drawn), "row")
    positions = list(range(len(capacities)))
    rows = [("admit_median_ns", "ADMIT median (µs)"), ("admit_p99_ns", "ADMIT p99 (µs)")]
    for row, (column, ylabel) in enumerate(rows):
        for index, mode in enumerate(modes):
            axis = fig.axes[row][index]
            for data, series in drawn:
                stats = [spread(data, (mode, size, capacity, series[0]), column, 1e-3,
                                "admit_ops") for capacity in capacities]
                draw(axis, positions, [s[0] if s else None for s in stats], series,
                     [s[1] if s else 0 for s in stats], [s[2] if s else 0 for s in stats])
            if row == 0:
                axis.set_title(layout.titles[mode])
            capacity_axis(axis, capacities, label=row == len(rows) - 1)
            axis.margins(y=0.12)
            axis.set_ylim(bottom=0)
            axis.yaxis.set_major_formatter(FuncFormatter(plain))
        fig.axes[row][0].set_ylabel(ylabel)
    fig.save("ADMIT latency by capacity", path)


def lru_minus_fifo_figure(plt, layout, groups, capacities, size, path):
    """LRU minus FIFO for the skewed workloads: hit rate and evictions."""
    from matplotlib.ticker import FuncFormatter
    present = [s for s in WORKLOAD_SERIES
               if any(key[0] == s[0] and key[1] == size for key in groups)]
    if not present:
        print("skipped", path, "(no skewed workloads in the data)")
        return
    note = ("Sharing on, %s-request traces. Left: LRU hit rate minus FIFO hit rate, in "
            "percentage points. Right: LRU evictions minus FIFO evictions, as a percentage "
            "of the FIFO evictions. Above zero on the left and below zero on the right both "
            "favor LRU." % size)
    fig = Figure(plt, layout, 1, 2, note, len(present), False,
                 row_height=layout.row_height + 0.35)
    positions = list(range(len(capacities)))

    def hit_rate(row):
        return row["cached_tokens"] / row["prompt_tokens"] if row["prompt_tokens"] else 0.0

    def difference(mode, capacity, panel):
        lru = groups.get((mode, size, capacity, ("on", "lru")))
        fifo = groups.get((mode, size, capacity, ("on", "fifo")))
        if not lru or not fifo:
            return None
        if panel == 0:
            return 100.0 * (hit_rate(lru[0]) - hit_rate(fifo[0]))
        if fifo[0]["evictions"] == 0:
            return 0.0 if lru[0]["evictions"] == 0 else None
        return 100.0 * (lru[0]["evictions"] - fifo[0]["evictions"]) / fifo[0]["evictions"]

    panels = [("Hit rate", "LRU − FIFO (percentage points)", "{:+.1f}"),
              ("Evictions", "LRU − FIFO (% of FIFO evictions)", "{:+.1f}%")]
    for panel, (title, ylabel, pattern) in enumerate(panels):
        axis = fig.axes[0][panel]
        axis.axhline(0, color=ZERO_LINE, linewidth=1.2, zorder=1)
        for mode, label, color, marker, line, hollow in present:
            ys = [difference(mode, capacity, panel) for capacity in capacities]
            draw(axis, positions, ys, (None, label, color, marker, line, hollow))
            for x, y in zip(positions, ys):
                if y is None:
                    continue
                # Every point is labeled: six values per panel, and the two
                # series sit far apart, so the numbers do not collide.
                text = pattern.format(y)
                if text in ("+0.0", "-0.0", "+0.0%", "-0.0%"):
                    text = text[1:]
                below = (panel == 1) == (mode == "skewed_short")
                axis.annotate(text, (x, y), textcoords="offset points",
                              xytext=(0, -13 if below else 8), ha="center",
                              fontsize=layout.font - 1, color=TEXT)
        axis.set_title(title)
        axis.set_ylabel(ylabel)
        capacity_axis(axis, capacities)
        axis.margins(y=0.22)
        axis.yaxis.set_major_formatter(FuncFormatter(lambda v, _: "{:g}".format(v)))
    fig.save("LRU minus FIFO on the skewed workloads", path)


def latency_figure(plt, layout, groups, sizes, capacity, path):
    from matplotlib.ticker import FuncFormatter, LogLocator, NullFormatter
    modes = layout.modes({key[0] for key in groups if key[2] == capacity})
    note = ("Capacity {:,} blocks. Point: median over the repetitions of each run's median "
            "latency. Bar: interquartile range of those medians. Log scale. Manager call "
            "only; parsing, checks and output are excluded.".format(capacity))
    fig = Figure(plt, layout, len(OPS), len(modes), note, len(SERIES), "row",
                 row_height=layout.row_height - 0.6)
    positions = list(range(len(sizes)))
    for row, op in enumerate(OPS):
        drawn = []  # every value in this row, to choose a common y range
        for column, mode in enumerate(modes):
            axis = fig.axes[row][column]
            for series in SERIES:
                stats = [spread(groups, (mode, size, capacity, series[0]), op + "_median_ns",
                                1.0, op + "_ops") for size in sizes]
                drawn += [v for s in stats if s for v in s[1:]]
                draw(axis, positions, [s[0] if s else None for s in stats], series,
                     [s[1] if s else 0 for s in stats], [s[2] if s else 0 for s in stats])
            axis.set_yscale("log")
            axis.yaxis.set_major_locator(LogLocator(base=10, subs=(1.0, 2.0, 5.0)))
            axis.yaxis.set_major_formatter(FuncFormatter(plain))
            axis.yaxis.set_minor_formatter(NullFormatter())
            axis.set_xticks(positions)
            axis.set_xticklabels(sizes)
            axis.set_xlim(-0.3, len(sizes) - 0.7)
            if row == 0:
                axis.set_title(layout.titles[mode])
            if row == len(OPS) - 1:
                axis.set_xlabel("Trace size (requests)")
        fig.axes[row][0].set_ylabel("%s median (ns)" % op.upper())
        if drawn and min(drawn) > 0:
            # Show at least a factor of 4, so a difference of a few nanoseconds
            # (the clock's own resolution) is not stretched over the whole panel.
            low, high = min(drawn), max(drawn)
            middle = (low * high) ** 0.5
            span = max((high / low) ** 0.5 * 1.25, 2.0)
            fig.axes[row][0].set_ylim(middle / span, middle * span)
    fig.save("Operation latency by trace size", path)


# --------------------------------------------------------------------- main

def main():
    parser = argparse.ArgumentParser(description="Plot kvcache sweeps.")
    parser.add_argument("--csv", nargs="+", default=None,
                        help="one or more sweep CSVs (default: results/raw/sweep*.csv)")
    parser.add_argument("--out", default=None,
                        help="output directory (default: results/figures, or "
                             "results/figures/report with --report)")
    parser.add_argument("--report", action="store_true",
                        help="7-inch figures with the system, multiturn and skewed_short "
                             "panels only")
    parser.add_argument("--compare", action="append", default=[], metavar="LABEL=FILE",
                        help="overlay a later sweep in admit_latency_vs_capacity.png, e.g. "
                             "\"Sharing on, O(1) count=results/raw/o1_sweep.csv\"")
    parser.add_argument("--size", default=None,
                        help="trace size for the capacity figures (default: largest in the CSV)")
    parser.add_argument("--capacity", type=int, default=None,
                        help="capacity for latency_vs_size (default: middle one in the CSV)")
    args = parser.parse_args()

    compare_files = []
    for item in args.compare:
        label, separator, path = item.partition("=")
        if not separator or not label.strip() or not path.strip():
            parser.error("--compare needs LABEL=FILE, got '%s'" % item)
        compare_files.append((label.strip(), path.strip()))
    if len(compare_files) > len(COMPARE_STYLES):
        parser.error("at most %d --compare sweeps" % len(COMPARE_STYLES))

    try:
        import matplotlib
    except ImportError:
        sys.exit("error: matplotlib is not installed (try: python3 -m pip install matplotlib)")
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    compared = {os.path.abspath(path) for _, path in compare_files}
    paths = args.csv or sorted(glob.glob("results/raw/sweep*.csv"))
    paths = [path for path in paths if os.path.abspath(path) not in compared]
    if not paths:
        sys.exit("error: no results/raw/sweep*.csv; run scripts/run_experiments.sh or pass --csv")
    groups = load(paths)
    check_deterministic(groups)
    compare = []
    for label, path in compare_files:
        other = load([path])
        check_deterministic(other)
        compare.append((label, other))

    sizes = [s for s in SIZES if any(key[1] == s for key in groups)]
    capacities = sorted({key[2] for key in groups})
    size = args.size or sizes[-1]
    capacity = args.capacity or capacities[len(capacities) // 2]
    if size not in sizes:
        sys.exit("error: no rows for size %s" % size)
    if capacity not in capacities:
        sys.exit("error: no rows for capacity %d" % capacity)
    layout = Layout(args.report)
    out = args.out or ("results/figures/report" if args.report else "results/figures")
    os.makedirs(out, exist_ok=True)
    style(plt, layout)
    scope = "%s-request traces" % size
    target = lambda name: os.path.join(out, name)

    capacity_figure(
        plt, layout, groups, capacities, size,
        lambda r: r["cached_tokens"] / r["prompt_tokens"] if r["prompt_tokens"] else 0.0,
        "Prompt tokens from cache", "Prefix-cache hit rate by capacity",
        "Cached / prompt tokens of admitted requests, %s. Only full 16-token blocks are "
        "shared, and sharing off never caches." % scope,
        target("hit_rate_vs_capacity.png"), percent=True)
    capacity_figure(
        plt, layout, groups, capacities, size,
        lambda r: r["uncached_tokens"] / r["admitted"] if r["admitted"] else None,
        "Uncached tokens per request", "Uncached prompt tokens per admitted request",
        "Prompt tokens written fresh (a proxy for recomputation) divided by the admitted "
        "ADMITs, %s. Each panel has its own scale. Rejected requests are not counted, so "
        "panels with different admitted shares still compare like with like." % scope,
        target("uncached_tokens_vs_capacity.png"), sharey=False)
    capacity_figure(
        plt, layout, groups, capacities, size, lambda r: r["uncached_tokens"] / 1e6,
        "Uncached tokens (millions)", "Uncached prompt tokens, total",
        "Prompt tokens written fresh over the whole trace, %s. Each panel has its own "
        "scale. Rejected requests write nothing, so a lower total can mean fewer admitted "
        "requests: read this next to the admitted share." % scope,
        target("uncached_tokens_vs_capacity_total.png"), sharey=False,
        formatter=lambda v, _=None: "{:g}".format(v))
    capacity_figure(
        plt, layout, groups, capacities, size,
        lambda r: r["admitted"] / r["admit_ops"] if r["admit_ops"] else 0.0,
        "ADMITs accepted", "Admitted share by capacity",
        "Accepted ADMITs / attempted ADMITs, %s. A rejected request's later commands are "
        "skipped." % scope,
        target("admitted_vs_capacity.png"), percent=True)
    admit_latency_figure(plt, layout, groups, compare, capacities, size,
                         target("admit_latency_vs_capacity.png"))
    lru_minus_fifo_figure(plt, layout, groups, capacities, size, target("lru_minus_fifo.png"))
    latency_figure(plt, layout, groups, sizes, capacity, target("latency_vs_size.png"))
    if not args.report:
        capacity_figure(
            plt, layout, groups, capacities, size, lambda r: r["peak_used_blocks"],
            "Peak blocks in use", "Peak blocks in use by capacity",
            "Most blocks allocated at once, %s. With sharing on, cached blocks stay "
            "allocated until evicted, so the pool fills. Sharing off shows the live demand "
            "alone." % scope,
            target("peak_blocks_vs_capacity.png"))
    print("%d configurations, up to %d repetitions each"
          % (len(groups), max(len(rows) for rows in groups.values())))


if __name__ == "__main__":
    main()
