#!/usr/bin/env python3
"""Benchmark Vulp against C++ and Python on the same problems.

For every problem it
  * builds the Vulp program(s) with `vulpc -O2 -o` and the C++ one with `g++ -O2`, timing both;
  * runs every implementation RUNS times on the same input, pinned to one CPU, in interleaved
    rounds (A B C A B C ...), recording the wall time and peak memory (max RSS) of each run;
  * checks that all implementations print exactly the same output;
  * also times `vulpc prog.vp < input` (JIT: compile in memory and run);
  * measures source size (non-blank, non-comment lines and tokens).
Results go to results.json (consumed by make_figures.py and the report).

Usage: python3 run_bench.py [--vulpc PATH] [--runs N] [--cpu N] [problem ...]
"""
import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
OUT = os.path.join(HERE, "out")
INPUTS = os.path.join(HERE, "inputs")

# problem -> list of (implementation label, language, source file)
PROBLEMS = {
    "sieve": [("Vulp", "vulp", "sieve.vp"), ("C++", "cpp", "sieve.cpp"), ("Python", "py", "sieve.py")],
    "sort_io": [("Vulp", "vulp", "sort_io.vp"), ("C++", "cpp", "sort_io.cpp"), ("Python", "py", "sort_io.py")],
    "lcs": [("Vulp (dp fn)", "vulp", "lcs_dp.vp"), ("Vulp (table)", "vulp", "lcs_table.vp"),
            ("Vulp (i32 table)", "vulp", "lcs_table_i32.vp"),
            ("C++ (table)", "cpp", "lcs.cpp"), ("C++ (memo)", "cpp", "lcs_memo.cpp"), ("Python", "py", "lcs.py")],
    "dijkstra": [("Vulp", "vulp", "dijkstra.vp"), ("C++", "cpp", "dijkstra.cpp"), ("Python", "py", "dijkstra.py")],
    "testcases": [("Vulp", "vulp", "testcases.vp"), ("C++", "cpp", "testcases.cpp"), ("Python", "py", "testcases.py")],
    "map_count": [("Vulp", "vulp", "map_count.vp"), ("C++", "cpp", "map_count.cpp"), ("Python", "py", "map_count.py")],
}

TOKEN_RE = re.compile(r"[A-Za-z_][A-Za-z_0-9]*|\d[\d_.]*|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'|\S")


def code_size(path, lang):
    """Non-blank, non-comment lines and tokens of a source file."""
    lines = tokens = 0
    for raw in open(path):
        line = raw
        if lang in ("vulp", "py"):
            line = re.sub(r"(^|\s)#.*$", "", line) if not line.lstrip().startswith("#include") else line
        else:
            line = re.sub(r"//.*$", "", line)
        if line.strip():
            lines += 1
            tokens += len(TOKEN_RE.findall(line))
    return lines, tokens


def timed_run(cmd, stdin_path, stdout_path, cpu):
    """Run cmd once; return (seconds, max RSS in MiB).

    The peak memory comes from /usr/bin/time: os.wait4 on a child forked from this script would
    report at least this script's own size, because ru_maxrss survives execve.
    """
    rss_file = stdout_path + ".rss"
    cmd = ["/usr/bin/time", "-f", "%M", "-o", rss_file] + cmd
    if cpu is not None and shutil.which("taskset"):
        cmd = ["taskset", "-c", str(cpu)] + cmd
    with open(stdin_path, "rb") as fin, open(stdout_path, "wb") as fout:
        start = time.perf_counter()
        proc = subprocess.run(cmd, stdin=fin, stdout=fout, stderr=subprocess.PIPE)
        elapsed = time.perf_counter() - start
    if proc.returncode != 0:
        raise RuntimeError(f"{' '.join(cmd)} exited with {proc.returncode}:\n{proc.stderr.decode(errors='replace')}")
    rss_kib = int(open(rss_file).read().split()[-1])
    os.remove(rss_file)
    return elapsed, rss_kib / 1024.0


def build(lang, src, exe, vulpc):
    if lang == "vulp":
        cmd = [vulpc, "-O2", src, "-o", exe]
    elif lang == "cpp":
        cmd = ["g++", "-O2", "-std=c++20", src, "-o", exe]
    else:
        return None
    times = []
    for _ in range(3):
        start = time.perf_counter()
        subprocess.run(cmd, check=True)
        times.append(time.perf_counter() - start)
    return statistics.median(times)


def sha(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def tool_versions(vulpc):
    def first_line(cmd):
        try:
            return subprocess.run(cmd, capture_output=True, text=True).stdout.splitlines()[0]
        except Exception:
            return "unknown"
    cpu = "unknown"
    try:
        for line in open("/proc/cpuinfo"):
            if line.startswith("model name"):
                cpu = line.split(":", 1)[1].strip()
                break
    except OSError:
        pass
    return {
        "cpu": cpu,
        "os": platform.platform(),
        "g++": first_line(["g++", "--version"]),
        "python": platform.python_implementation() + " " + platform.python_version(),
        "llvm": first_line(["llvm-config-18", "--version"]),
        "vulpc": os.path.basename(vulpc),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vulpc", default=os.path.join(ROOT, "build", "vulpc"))
    ap.add_argument("--runs", type=int, default=7)
    ap.add_argument("--cpu", type=int, default=7, help="CPU to pin runs to (-1: no pinning)")
    ap.add_argument("--output", default=os.path.join(HERE, "results.json"),
                    help="write results to this JSON file")
    ap.add_argument("problems", nargs="*")
    args = ap.parse_args()
    cpu = None if args.cpu < 0 else args.cpu
    vulpc = os.path.abspath(args.vulpc)
    os.makedirs(OUT, exist_ok=True)

    names = args.problems or list(PROBLEMS)
    missing = [n for n in names if not os.path.exists(os.path.join(INPUTS, n + ".in"))]
    if missing:
        subprocess.run([sys.executable, os.path.join(HERE, "gen_inputs.py")] + missing, check=True)

    results = {"env": tool_versions(vulpc), "runs": args.runs, "problems": {}}
    for name in names:
        inp = os.path.join(INPUTS, name + ".in")
        prob = {"input_bytes": os.path.getsize(inp), "impls": []}
        impls = []
        # 1. build everything and check that every implementation prints the same output
        reference = None
        for label, lang, file in PROBLEMS[name]:
            src = os.path.join(HERE, name, file)
            exe = os.path.join(OUT, os.path.splitext(file)[0] + ("_vp" if lang == "vulp" else "_cpp"))
            compile_s = build(lang, src, exe, vulpc)
            cmd = [exe] if lang != "py" else [sys.executable, src]
            out_path = os.path.join(OUT, f"{name}.{label.replace(' ', '_').replace('(', '').replace(')', '')}.out")
            timed_run(cmd, inp, out_path, cpu)
            digest = sha(out_path)
            if reference is None:
                reference = digest
            elif digest != reference:
                sys.exit(f"{name}: output of {label} differs from {PROBLEMS[name][0][0]}")
            impl = {"label": label, "lang": lang, "source": f"{name}/{file}", "compile_s": compile_s}
            impl["lines"], impl["tokens"] = code_size(src, lang)
            impls.append((impl, cmd, out_path, src, [], []))
        # 2. timing rounds, interleaved so that machine noise affects every implementation alike
        for _ in range(args.runs):
            for impl, cmd, out_path, src, times, rss in impls:
                t, m = timed_run(cmd, inp, out_path, cpu)
                times.append(t)
                rss.append(m)
        for impl, cmd, out_path, src, times, rss in impls:
            impl.update({"time_s": statistics.median(times), "time_min_s": min(times),
                         "time_max_s": max(times), "rss_mib": statistics.median(rss)})
            if impl["lang"] == "vulp":
                jit = [timed_run([vulpc, "-O2", src], inp, out_path, cpu)[0] for _ in range(3)]
                if sha(out_path) != reference:
                    sys.exit(f"{name}: JIT output of {impl['label']} differs")
                impl["jit_total_s"] = statistics.median(jit)
            prob["impls"].append(impl)
            print(f"{name:10} {impl['label']:13} {impl['time_s']:8.3f}s  [{impl['time_min_s']:.3f}-{impl['time_max_s']:.3f}]"
                  f"  {impl['rss_mib']:8.1f} MiB  {impl['lines']:3} lines {impl['tokens']:4} tokens"
                  + (f"  build {impl['compile_s']:.2f}s" if impl["compile_s"] is not None else "")
                  + (f"  jit {impl['jit_total_s']:.3f}s" if "jit_total_s" in impl else ""), flush=True)
        prob["output_sha256"] = reference
        results["problems"][name] = prob

    path = os.path.abspath(args.output)
    if args.problems and os.path.exists(path):   # partial run: merge into the existing results
        old = json.load(open(path))
        old["problems"].update(results["problems"])
        old["env"], old["runs"] = results["env"], results["runs"]
        results = old
    with open(path, "w") as f:
        json.dump(results, f, indent=2)
    print("wrote", path)


if __name__ == "__main__":
    main()
