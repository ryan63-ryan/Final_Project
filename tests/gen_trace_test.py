#!/usr/bin/env python3
"""Checks scripts/gen_trace.py: every trace is valid, deterministic and bounded,
has the sharing structure its mode promises, and the driver replays it without
a single ERROR line.

Usage: python3 tests/gen_trace_test.py <path to kvcache>
"""

import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GENERATOR = os.path.join(ROOT, "scripts", "gen_trace.py")


def generate(*options):
    result = subprocess.run([sys.executable, GENERATOR] + [str(o) for o in options],
                            check=True, stdout=subprocess.PIPE, universal_newlines=True)
    return result.stdout


def replay(text, options):
    """Independent model of the trace rules. Returns per-request prompts and outputs."""
    live = {}       # id -> [tokens, appends]
    seen = set()
    prompts = []    # (prompt tokens, was it a FORK)
    outputs = []
    most_active = 0
    lines = text.splitlines()
    assert lines[0].startswith("# gen_trace.py "), lines[0]
    assert lines[-1] == "STATS", lines[-1]
    for line in lines[1:-1]:
        words = line.split()
        op, rid = words[0], words[1]
        if op == "ADMIT":
            assert rid not in seen, "duplicate ID " + rid
            tokens = [int(w) for w in words[2:]]
            assert options["prompt_min"] <= len(tokens) <= options["prompt_max"], line[:60]
            live[rid] = [tokens, 0]
            prompts.append((tokens, False))
        elif op == "FORK":
            assert rid not in seen, "duplicate ID " + rid
            assert len(words) == 3 and words[2] in live, "fork of a request not running: " + line
            live[rid] = [list(live[words[2]][0]), 0]
            prompts.append((live[rid][0][:], True))
        elif op == "APPEND":
            assert len(words) == 3 and rid in live, "append to a request not running: " + line
            live[rid][0].append(int(words[2]))
            live[rid][1] += 1
        elif op == "RELEASE":
            assert len(words) == 2 and rid in live, "release of a request not running: " + line
            outputs.append(live.pop(rid)[1])
        else:
            raise AssertionError("unexpected line: " + line)
        seen.add(rid)
        most_active = max(most_active, len(live))
        if op in ("ADMIT", "APPEND"):
            assert all(0 <= t < options["vocab"] for t in live[rid][0][-1:]), line[:60]
    assert not live, "requests never released: %s" % sorted(live)[:5]
    assert most_active <= options["max_active"], most_active
    return prompts, outputs


def shared_prefix(a, b):
    n = 0
    while n < len(a) and n < len(b) and a[n] == b[n]:
        n += 1
    return n


def main():
    kvcache = sys.argv[1]
    options = {"prompt_min": 20, "prompt_max": 120, "output_min": 0, "output_max": 12,
               "max_active": 6, "vocab": 500}
    flags = ["--prompt-min", 20, "--prompt-max", 120, "--output-min", 0, "--output-max", 12,
             "--max-active", 6, "--vocab", 500, "--system-len", 48, "--system-prompts", 3,
             "--turn-min", 20, "--turn-max", 40, "--fork-fraction", 0.1]
    for mode in ("unique", "system", "multiturn"):
        text = generate("--mode", mode, "--requests", 400, "--seed", 7, *flags)
        assert text == generate("--mode", mode, "--requests", 400, "--seed", 7, *flags), \
            "same seed, different trace"
        assert text != generate("--mode", mode, "--requests", 400, "--seed", 8, *flags), \
            "different seed, same trace"
        prompts, outputs = replay(text, options)
        assert len(prompts) == 400 and len(outputs) == 400
        forks = sum(1 for _, forked in prompts if forked)
        assert 10 <= forks <= 90, forks
        assert all(0 <= n <= 12 for n in outputs) and max(outputs) == 12

        admitted = [tokens for tokens, forked in prompts if not forked]
        if mode == "unique":
            # Random prompts over 500 tokens: no two share even one 16-token block.
            assert max(shared_prefix(a, b) for i, a in enumerate(admitted)
                       for b in admitted[:i]) < 16
        if mode == "system":
            starts = {tuple(tokens[:48]) for tokens in admitted}
            assert len(starts) == 3, len(starts)
        if mode == "multiturn":
            # Most turns extend an earlier request's prompt by at least one message.
            extended = sum(1 for i, a in enumerate(admitted)
                           if any(len(b) < len(a) and shared_prefix(a, b) == len(b)
                                  for b in admitted[:i]))
            assert extended > len(admitted) // 2, extended

        # The driver replays it with per-operation invariant checks and no ERROR.
        for sharing, capacity in (("on", 64), ("off", 64), ("on", 16)):
            with tempfile.TemporaryDirectory() as folder:
                config = os.path.join(folder, "run.cfg")
                trace = os.path.join(folder, "run.trace")
                with open(config, "w") as out:
                    out.write("capacity=%d\ntokens_per_block=16\nbytes_per_token=8\n"
                              "policy=lru\nsharing=%s\n" % (capacity, sharing))
                with open(trace, "w") as out:
                    out.write(text)
                run = subprocess.run([kvcache, "--config", config, "--trace", trace],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                     universal_newlines=True)
            assert run.returncode == 0, run.stderr
            assert "ERROR" not in run.stdout, [l for l in run.stdout.splitlines()
                                               if "ERROR" in l][:3]
            assert run.stdout.splitlines()[-1].startswith("STATS admitted=")
        print("PASS %s traces: valid, deterministic, replayed by the driver" % mode)

    # Skewed modes: many system prompts, a few of them very popular.
    for mode, longest in (("skewed", 256), ("skewed_short", 128 + 15)):
        text = generate("--mode", mode, "--requests", 3000, "--seed", 3, "--vocab", 500)
        assert text == generate("--mode", mode, "--requests", 3000, "--seed", 3, "--vocab", 500)
        limits = {"prompt_min": 129, "prompt_max": longest, "max_active": 32, "vocab": 500}
        prompts, outputs = replay(text, limits)
        admitted = [tokens for tokens, forked in prompts if not forked]
        uses = {}
        for tokens in admitted:
            uses[tuple(tokens[:128])] = uses.get(tuple(tokens[:128]), 0) + 1
        ranked = sorted(uses.values(), reverse=True)
        # Zipf with s = 1 over 1024 prompts: the top one gets about 13% of the
        # requests, and several hundred different prompts appear at all.
        assert 300 <= len(ranked) <= 1024, len(ranked)
        assert 0.08 * len(admitted) <= ranked[0] <= 0.20 * len(admitted), ranked[:3]
        assert ranked[0] > 5 * ranked[20], ranked[:21]
        if mode == "skewed_short":
            assert max(outputs) <= 8 and min(outputs) == 0
        else:
            assert max(outputs) == 64 and min(outputs) == 8
        print("PASS %s traces: valid, deterministic, Zipf popularity" % mode)

    # Edge cases: no requests, one running request, and rejected arguments.
    assert generate("--mode", "unique", "--requests", 0).splitlines()[1:] == ["STATS"]
    replay(generate("--mode", "multiturn", "--requests", 50, "--max-active", 1, *flags[:8],
                    "--vocab", 500, "--turn-min", 20, "--turn-max", 40),
           dict(options, max_active=1))
    bad = subprocess.run([sys.executable, GENERATOR, "--mode", "system", "--requests", "5",
                          "--system-len", "300"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert bad.returncode != 0
    print("PASS generator edge cases")


if __name__ == "__main__":
    try:
        main()
    except AssertionError as error:
        print("FAIL", error)
        sys.exit(1)
