"""Benchmark recursive Fibonacci with and without Vulp's dp keyword.

Run from the repository root:
python3 docs/report/bench/dp_fib/benchmark.py --vulpc /path/to/vulpc
"""

import argparse
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile
import time


HERE = Path(__file__).resolve().parent
EXPECTED = {34: 5702887, 37: 24157817, 40: 102334155}


def timed(command, n, expected):
    start = time.perf_counter()
    result = subprocess.run(command, input=f"{n}\n", capture_output=True, text=True, check=True)
    elapsed = time.perf_counter() - start
    if result.stdout.strip() != str(expected):
        raise RuntimeError(f"incorrect result from {command}: {result.stdout!r}")
    return elapsed


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--vulpc", default="build/vulpc")
    parser.add_argument("--rounds", type=int, default=3)
    args = parser.parse_args()

    vulpc = Path(args.vulpc).resolve()
    with tempfile.TemporaryDirectory(prefix="vulp-fib-") as tmp:
        tmp = Path(tmp)
        for stem in ("fib_input_plain", "fib_input_dp"):
            subprocess.run(
                [str(vulpc), "-O2", str(HERE / f"{stem}.vp"), "-o", str(tmp / stem)],
                check=True,
            )
        subprocess.run(
            ["g++", "-O2", "-std=c++20", str(HERE / "fib_plain.cpp"), "-o", str(tmp / "fib_cpp")],
            check=True,
        )

        results = {}
        for n, expected in EXPECTED.items():
            commands = {
                "Vulp dp fn": [str(tmp / "fib_input_dp")],
                "Vulp plain": [str(tmp / "fib_input_plain")],
                "C++ plain": [str(tmp / "fib_cpp")],
                "Python plain": [sys.executable, str(HERE / "fib_plain.py")],
            }
            samples = {name: [] for name in commands}
            for round_no in range(args.rounds):
                names = list(commands)
                names = names[round_no % len(names):] + names[:round_no % len(names)]
                for name in names:
                    elapsed = timed(commands[name], n, expected)
                    samples[name].append(elapsed)
                    print(f"n={n} round={round_no + 1} {name}: {elapsed:.6f} s", flush=True)
            results[str(n)] = {
                name: {"median_s": statistics.median(values), "runs_s": values}
                for name, values in samples.items()
            }

    output = {
        "method": f"Wall time for a fresh process; all programs read n from standard input; interpreter startup included where applicable; {args.rounds} interleaved rounds per n; output equality checked; g++ -O2, vulpc -O2.",
        "host": os.uname().nodename,
        "python": sys.version.split()[0],
        "rounds": args.rounds,
        "results": results,
    }
    (HERE / "results_cross_language.json").write_text(json.dumps(output, indent=2) + "\n")


if __name__ == "__main__":
    main()
