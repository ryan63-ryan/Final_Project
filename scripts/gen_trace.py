#!/usr/bin/env python3
"""Seeded trace generator for the kvcache driver (standard library only).

Three workloads, matching proposal section 6:

  unique     every prompt is random tokens: nothing to share except forks.
  system     every prompt starts with one of a few fixed system prompts,
             followed by a unique user part: a shared prefix across requests.
  multiturn  conversations: each turn is one request whose prompt is the whole
             conversation so far (previous prompt + previous output + a new
             user message), so each turn extends the previous turn's tokens.

A request is created by ADMIT (or, for a small fraction, by FORK of a request
that is still running), receives its output one APPEND at a time and is then
RELEASEd. Up to --max-active requests run at once and their commands are
interleaved at random, so the trace looks like a server handling concurrent
requests. --requests counts created requests (ADMIT + FORK).

The trace never contains an invalid command: request IDs are unique, APPEND and
RELEASE name a running request, and FORK names a running source. (The driver
may still reject a request for lack of capacity; it then skips that request's
later commands itself.)

Example:
  python3 scripts/gen_trace.py --mode system --requests 1000 --seed 1 --out traces/system_1k.trace
"""

import argparse
import random
import sys

MODES = ("unique", "system", "multiturn")


def parse_args(argv):
    p = argparse.ArgumentParser(description="Generate a kvcache trace.")
    p.add_argument("--mode", choices=MODES, required=True)
    p.add_argument("--requests", type=int, required=True,
                   help="requests to create (ADMIT + FORK), e.g. 1000, 10000, 100000")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--out", default="-", help="output file, '-' for standard output")
    p.add_argument("--max-active", type=int, default=32,
                   help="most requests running at the same time")
    p.add_argument("--arrival", type=float, default=0.1,
                   help="chance per step that a new request starts while below --max-active")
    p.add_argument("--prompt-min", type=int, default=32, help="shortest prompt in tokens")
    p.add_argument("--prompt-max", type=int, default=256, help="longest prompt in tokens")
    p.add_argument("--output-min", type=int, default=8, help="fewest APPENDs per request")
    p.add_argument("--output-max", type=int, default=64, help="most APPENDs per request")
    p.add_argument("--vocab", type=int, default=32000, help="tokens are 0 .. vocab-1")
    p.add_argument("--fork-fraction", type=float, default=0.02,
                   help="fraction of requests created by FORK instead of ADMIT")
    p.add_argument("--system-prompts", type=int, default=4,
                   help="system mode: number of distinct system prompts")
    p.add_argument("--system-len", type=int, default=128,
                   help="system mode: tokens in each system prompt")
    p.add_argument("--turn-min", type=int, default=16,
                   help="multiturn mode: fewest new user tokens per turn")
    p.add_argument("--turn-max", type=int, default=64,
                   help="multiturn mode: most new user tokens per turn")
    p.add_argument("--continue-chance", type=float, default=0.8,
                   help="multiturn mode: chance that a new request continues a waiting "
                        "conversation instead of starting one")
    args = p.parse_args(argv)

    def need(ok, message):
        if not ok:
            p.error(message)

    need(args.requests >= 0, "--requests must not be negative")
    need(args.max_active >= 1, "--max-active must be at least 1")
    need(0.0 < args.arrival <= 1.0, "--arrival must be in (0, 1]")
    need(1 <= args.prompt_min <= args.prompt_max, "need 1 <= --prompt-min <= --prompt-max")
    need(0 <= args.output_min <= args.output_max, "need 0 <= --output-min <= --output-max")
    need(1 <= args.vocab <= 2 ** 31, "--vocab must be in 1 .. 2^31")
    need(0.0 <= args.fork_fraction < 1.0, "--fork-fraction must be in [0, 1)")
    need(0.0 <= args.continue_chance <= 1.0, "--continue-chance must be in [0, 1]")
    if args.mode == "system":
        need(args.system_prompts >= 1, "--system-prompts must be at least 1")
        need(1 <= args.system_len < args.prompt_max,
             "need 1 <= --system-len < --prompt-max (room for a user part)")
    if args.mode == "multiturn":
        need(1 <= args.turn_min <= args.turn_max, "need 1 <= --turn-min <= --turn-max")
        need(args.turn_min <= args.prompt_max, "--turn-min must fit in --prompt-max")
    return args


class Generator:
    def __init__(self, args, write):
        self.args = args
        self.write = write
        self.rng = random.Random(args.seed)
        self.created = 0
        # Running requests: id -> [tokens so far, APPENDs still to emit, conversation flag].
        self.active = {}
        self.active_ids = []  # same keys, for O(1) random choice
        self.waiting = []     # multiturn: token histories of conversations between turns
        self.system = []
        if args.mode == "system":
            self.system = [self.tokens(args.system_len) for _ in range(args.system_prompts)]

    def tokens(self, count):
        vocab = self.args.vocab
        return [self.rng.randrange(vocab) for _ in range(count)]

    def new_prompt(self):
        """Returns (prompt tokens, True if the request is a conversation turn)."""
        a, rng = self.args, self.rng
        if a.mode == "unique":
            return self.tokens(rng.randint(a.prompt_min, a.prompt_max)), False
        if a.mode == "system":
            user_min = max(1, a.prompt_min - a.system_len)
            user = self.tokens(rng.randint(user_min, a.prompt_max - a.system_len))
            return rng.choice(self.system) + user, False
        # multiturn: continue a waiting conversation if it still fits, else start one.
        message = self.tokens(rng.randint(a.turn_min, min(a.turn_max, a.prompt_max)))
        if self.waiting and rng.random() < a.continue_chance:
            index = rng.randrange(len(self.waiting))
            self.waiting[index], self.waiting[-1] = self.waiting[-1], self.waiting[index]
            history = self.waiting.pop()
            if len(history) + len(message) <= a.prompt_max:
                return history + message, True
            # Too long to continue: that conversation ends, a new one starts.
        return message, True

    def start_request(self):
        a, rng = self.args, self.rng
        self.created += 1
        rid = "r%d" % self.created
        output = rng.randint(a.output_min, a.output_max)
        if self.active_ids and rng.random() < a.fork_fraction:
            source = rng.choice(self.active_ids)
            # A fork is a branch of its source; it does not continue a conversation.
            self.active[rid] = [list(self.active[source][0]), output, False]
            self.write("FORK %s %s\n" % (rid, source))
        else:
            prompt, conversation = self.new_prompt()
            self.active[rid] = [prompt, output, conversation]
            self.write("ADMIT %s %s\n" % (rid, " ".join(map(str, prompt))))
        self.active_ids.append(rid)

    def advance(self):
        """One APPEND for a random running request, or its RELEASE when it is done."""
        a, rng = self.args, self.rng
        index = rng.randrange(len(self.active_ids))
        rid = self.active_ids[index]
        state = self.active[rid]
        if state[1] > 0:
            token = rng.randrange(a.vocab)
            state[0].append(token)
            state[1] -= 1
            self.write("APPEND %s %d\n" % (rid, token))
            return
        self.write("RELEASE %s\n" % rid)
        self.active_ids[index] = self.active_ids[-1]
        self.active_ids.pop()
        del self.active[rid]
        if state[2]:
            # The conversation waits for its next turn. Keep a bounded number.
            self.waiting.append(state[0])
            if len(self.waiting) > 4 * a.max_active:
                self.waiting.pop(rng.randrange(len(self.waiting)))

    def run(self):
        a, rng = self.args, self.rng
        while self.created < a.requests or self.active_ids:
            can_start = self.created < a.requests and len(self.active_ids) < a.max_active
            if can_start and (not self.active_ids or rng.random() < a.arrival):
                self.start_request()
            else:
                self.advance()


def main(argv=None):
    args = parse_args(sys.argv[1:] if argv is None else argv)
    out = sys.stdout if args.out == "-" else open(args.out, "w", newline="\n")
    try:
        settings = " ".join("%s=%s" % (key, value) for key, value in sorted(vars(args).items())
                            if key != "out")
        out.write("# gen_trace.py %s\n" % settings)
        Generator(args, out.write).run()
        out.write("STATS\n")
    finally:
        if out is not sys.stdout:
            out.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
