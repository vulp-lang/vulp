#!/usr/bin/env python3
"""Ablations for Vulp-specific choices (uses the inputs and helpers of run_bench.py).

  * runtime checks:  every Vulp benchmark built normally vs with --no-checks;
  * integer widths:  the LCS table with `int` (64-bit) vs `i32` elements.
Results go to ablation.json.
"""
import json
import argparse
import os
import statistics
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run_bench as rb  # noqa: E402

RUNS = 7
CPU = 7


def measure(cmd, inp, out):
    times, rss = [], []
    for _ in range(RUNS):
        t, m = rb.timed_run(cmd, inp, out, CPU)
        times.append(t)
        rss.append(m)
    return statistics.median(times), statistics.median(rss)


def build(src, exe, extra):
    subprocess.run([VULPC, "-O2", *extra, src, "-o", exe], check=True)


VULPC = os.path.join(rb.ROOT, "build", "vulpc")


def main():
    global RUNS, CPU, VULPC
    ap = argparse.ArgumentParser()
    ap.add_argument("--vulpc", default=VULPC)
    ap.add_argument("--runs", type=int, default=RUNS)
    ap.add_argument("--cpu", type=int, default=CPU)
    ap.add_argument("--output", default=os.path.join(rb.HERE, "ablation.json"))
    args = ap.parse_args()
    RUNS, CPU, VULPC = args.runs, (None if args.cpu < 0 else args.cpu), os.path.abspath(args.vulpc)
    os.makedirs(rb.OUT, exist_ok=True)
    res = {"checks": [], "widths": []}
    for name, impls in rb.PROBLEMS.items():
        inp = os.path.join(rb.INPUTS, name + ".in")
        for label, lang, file in impls:
            if lang != "vulp":
                continue
            src = os.path.join(rb.HERE, name, file)
            base = os.path.join(rb.OUT, "abl_" + os.path.splitext(file)[0])
            build(src, base + "_chk", [])
            build(src, base + "_nochk", ["--no-checks"])
            out = base + ".out"
            # interleave the two variants
            t_chk, t_no = [], []
            for _ in range(RUNS):
                t_chk.append(rb.timed_run([base + "_chk"], inp, out, CPU)[0])
                t_no.append(rb.timed_run([base + "_nochk"], inp, out, CPU)[0])
            row = {"problem": name, "label": label, "checks_s": statistics.median(t_chk),
                   "no_checks_s": statistics.median(t_no)}
            res["checks"].append(row)
            print(f"{name:10} {label:13} checks {row['checks_s']:.3f}s  --no-checks {row['no_checks_s']:.3f}s", flush=True)
    inp = os.path.join(rb.INPUTS, "lcs.in")
    for file, label in (("lcs_table.vp", "int (64-bit) table"), ("lcs_table_i32.vp", "i32 table")):
        src = os.path.join(rb.HERE, "lcs", file)
        exe = os.path.join(rb.OUT, "abl_w_" + os.path.splitext(file)[0])
        for extra, tag in (([], ""), (["--no-checks"], " + --no-checks")):
            build(src, exe, extra)
            t, m = measure([exe], inp, exe + ".out")
            res["widths"].append({"variant": label + tag, "time_s": t, "rss_mib": m})
            print(f"lcs {label + tag:32} {t:.3f}s {m:.1f} MiB", flush=True)
    with open(os.path.abspath(args.output), "w") as f:
        json.dump(res, f, indent=2)


if __name__ == "__main__":
    main()
