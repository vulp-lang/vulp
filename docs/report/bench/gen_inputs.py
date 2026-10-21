#!/usr/bin/env python3
"""Generate the benchmark inputs (deterministic: fixed seeds) into bench/inputs/."""
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "inputs")


def write(name, text):
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, name), "w") as f:
        f.write(text)


def sieve():
    write("sieve.in", "20000000\n")


def sort_io():
    r = random.Random(1)
    n = 1_000_000
    a = [r.randint(-10**9, 10**9) for _ in range(n)]
    write("sort_io.in", f"{n}\n" + " ".join(map(str, a)) + "\n")


def lcs():
    r = random.Random(2)
    n = 3000
    a = "".join(r.choice("acgt") for _ in range(n))
    b = "".join(r.choice("acgt") for _ in range(n))
    write("lcs.in", f"{a}\n{b}\n")


def dijkstra():
    r = random.Random(3)
    n, m = 100_000, 500_000
    lines = [f"{n} {m}"]
    # a random path through all vertices keeps the graph connected, the rest is random
    order = list(range(2, n + 1))
    r.shuffle(order)
    prev = 1
    for v in order:
        lines.append(f"{prev} {v} {r.randint(1, 1_000_000)}")
        prev = v
    for _ in range(m - (n - 1)):
        lines.append(f"{r.randint(1, n)} {r.randint(1, n)} {r.randint(1, 1_000_000)}")
    write("dijkstra.in", "\n".join(lines) + "\n")


def testcases():
    r = random.Random(4)
    t = 200_000
    parts = [str(t)]
    for _ in range(t):
        n = r.randint(1, 9)
        parts.append(str(n))
        parts.append(" ".join(str(r.randint(-10**9, 10**9)) for _ in range(n)))
    write("testcases.in", "\n".join(parts) + "\n")


def map_count():
    r = random.Random(5)
    n = 1_000_000
    pool = [r.randint(1, 10**18) for _ in range(200_000)]
    a = [r.choice(pool) for _ in range(n)]
    write("map_count.in", f"{n}\n" + " ".join(map(str, a)) + "\n")


GENERATORS = {f.__name__: f for f in (sieve, sort_io, lcs, dijkstra, testcases, map_count)}

if __name__ == "__main__":
    for name in sys.argv[1:] or GENERATORS:
        GENERATORS[name]()
        print("generated", name)
