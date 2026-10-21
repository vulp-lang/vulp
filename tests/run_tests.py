#!/usr/bin/env python3
"""End-to-end test runner for the Vulp compiler.

Modes
  jit       run every tests/e2e/*.vp through `vulpc file.vp` (LLVM JIT)
  aot       compile every tests/e2e/*.vp to a native executable and run it
  errors    every tests/errors/*.vp must be rejected with exit code 65
  examples  compile and run every examples/*.vp (both JIT and AOT)

Test files
  NAME.vp   the program
  NAME.in   optional stdin
  NAME.out  expected stdout (exact match)
Directives inside NAME.vp (comment lines):
  # EXPECT-EXIT: <n>          expected exit code of the program (default 0)
  # EXPECT-STDERR: <text>     text that must appear on stderr (repeatable)
  # EXPECT-ERROR: <text>      (errors mode) text that must appear in the compiler output
  # ARGS: <flags>             extra vulpc flags, e.g. --no-checks
  # TIME-LIMIT: <seconds>     (aot mode) the executable must finish within this wall time
  # MAX-RSS-MB: <megabytes>   (aot mode) peak resident memory of the executable
Set VULP_DEBUG_FREE=1 to make the runtime poison freed memory (catches use-after-free).
"""

import argparse
import difflib
import os
import pathlib
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
TIMEOUT = 120


def directives(source: str, name: str):
    prefix = f"# {name}:"
    return [line.strip()[len(prefix):].strip() for line in source.splitlines() if line.strip().startswith(prefix)]


def show_diff(expected: str, actual: str) -> str:
    diff = difflib.unified_diff(expected.splitlines(True), actual.splitlines(True), "expected", "actual")
    text = "".join(diff)
    return text if len(text) < 4000 else text[:4000] + "\n... (diff truncated)\n"


def run(cmd, stdin=b"", stats=None):
    """Runs cmd; returns (exit code, stdout, stderr). Records the wall time in stats["seconds"]."""
    start = time.monotonic()
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        out, err = proc.communicate(stdin, timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.communicate()
        return None, "", f"timed out after {TIMEOUT}s"
    if stats is not None:
        stats["seconds"] = time.monotonic() - start
    return proc.returncode, out.decode("utf-8", "replace"), err.decode("utf-8", "replace")


def check_program(vulpc, vp: pathlib.Path, mode: str, workdir: pathlib.Path):
    """Returns None on success, otherwise a failure description."""
    source = vp.read_text()
    flags = " ".join(directives(source, "ARGS")).split()
    stdin = vp.with_suffix(".in").read_bytes() if vp.with_suffix(".in").exists() else b""
    expected_out = vp.with_suffix(".out").read_text() if vp.with_suffix(".out").exists() else None
    expected_exit = int((directives(source, "EXPECT-EXIT") or ["0"])[0])
    expected_err = directives(source, "EXPECT-STDERR")
    time_limit = float((directives(source, "TIME-LIMIT") or ["0"])[0])
    max_rss = float((directives(source, "MAX-RSS-MB") or ["0"])[0])
    if os.environ.get("VULP_DEBUG_FREE"):
        max_rss = 0  # freed blocks are only poisoned in this mode, never returned
    stats = {}

    if mode == "jit":
        code, out, err = run([vulpc, *flags, str(vp)], stdin)
    else:
        exe = workdir / vp.stem
        code, out, err = run([vulpc, *flags, str(vp), "-o", str(exe)])
        if code != 0:
            return f"compilation failed (exit {code}):\n{err}"
        code, out, err = run([str(exe)], stdin, stats)
        stats["rss_mb"] = peak_rss_mb(exe, stdin) if max_rss else 0.0

    problems = []
    if mode == "aot" and time_limit and stats.get("seconds", 0) > time_limit:
        problems.append(f"took {stats['seconds']:.2f}s, limit {time_limit}s")
    if mode == "aot" and max_rss and stats["rss_mb"] > max_rss:
        problems.append(f"peak memory {stats['rss_mb']:.1f} MB, limit {max_rss} MB")
    if code != expected_exit:
        problems.append(f"exit code {code}, expected {expected_exit}\nstderr:\n{err}")
    for text in expected_err:
        if text not in err:
            problems.append(f"stderr does not contain {text!r}\nstderr:\n{err}")
    if expected_out is not None and out != expected_out:
        problems.append("stdout differs:\n" + show_diff(expected_out, out))
    if expected_out is None and not expected_err and code == 0 and out == "":
        problems.append("test has no .out file and no expectations")
    return "\n".join(problems) or None


def peak_rss_mb(exe, stdin):
    """Peak resident memory of one run of exe, in MB (from the kernel's rusage of that child)."""
    proc = subprocess.Popen([str(exe)], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    proc.stdin.write(stdin)
    proc.stdin.close()
    _, _, usage = os.wait4(proc.pid, 0)
    proc.returncode = 0  # already reaped by wait4
    return usage.ru_maxrss / 1024


def check_error_program(vulpc, vp: pathlib.Path):
    source = vp.read_text()
    expected = directives(source, "EXPECT-ERROR")
    if not expected:
        return "no '# EXPECT-ERROR:' directive"
    code, out, err = run([vulpc, "--check", str(vp)])
    problems = []
    if code != 65:
        problems.append(f"expected exit code 65 (compile error), got {code}")
    for text in expected:
        if text not in err:
            problems.append(f"compiler output does not contain {text!r}")
    if problems:
        problems.append("compiler output:\n" + err)
    return "\n".join(problems) or None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vulpc", required=True)
    ap.add_argument("--mode", choices=["jit", "aot", "errors", "examples"], required=True)
    ap.add_argument("--workdir", default=None)
    ap.add_argument("--filter", default="")
    args = ap.parse_args()

    vulpc = str(pathlib.Path(args.vulpc).resolve())
    workdir = pathlib.Path(args.workdir or (ROOT / "build" / f"tests-{args.mode}"))
    workdir.mkdir(parents=True, exist_ok=True)

    if args.mode == "errors":
        files = sorted((ROOT / "tests" / "errors").glob("*.vp"))
    elif args.mode == "examples":
        files = sorted((ROOT / "examples").glob("*.vp"))
    else:
        files = sorted((ROOT / "tests" / "e2e").glob("*.vp"))
    files = [f for f in files if args.filter in f.name]

    failures = 0
    for vp in files:
        if args.mode == "errors":
            problem = check_error_program(vulpc, vp)
        elif args.mode == "examples":
            problem = check_program(vulpc, vp, "jit", workdir) or check_program(vulpc, vp, "aot", workdir)
        else:
            problem = check_program(vulpc, vp, args.mode, workdir)
        if problem:
            failures += 1
            print(f"[FAIL] {vp.name}\n" + "\n".join("    " + line for line in problem.splitlines()))
        else:
            print(f"[ ok ] {vp.name}")
    print(f"\n{len(files) - failures}/{len(files)} {args.mode} tests passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
