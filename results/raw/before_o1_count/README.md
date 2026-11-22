# Results before the O(1) reclaimable count

`sweep.csv` (unique, system, multiturn) and `sweep_skewed.csv` (skewed, skewed_short)
are the sweeps of 2026-11-21. They were measured before radix edge compression and
before `RadixTree::reclaimable_count()`: the manager counted reclaimable blocks with an
O(capacity) scan, and the index had one node per block. The `.meta.txt` files give the
commits.

They are kept for reference and are what `docs/experiment_notes.md` quoted until the
sweep was rerun. `scripts/plot.py` does not read this directory by default.
