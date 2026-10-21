#!/usr/bin/env python3
"""Build the Vulp V1 slide deck (vulp_v1.pptx) from the benchmark results.

Every slide is described once as a list of positioned elements (text, code, boxes, images,
tables). The same description is written to the .pptx and, with --preview DIR, drawn to PNG
files with Pillow, so the layout can be checked without PowerPoint. The preview uses DejaVu
fonts, which are wider than the deck's Calibri / Consolas, so text that fits in the preview
fits in the deck. Charts are the report's SVG figures, rendered to PNG with ImageMagick.

Usage: python3 build_deck.py [--preview DIR]
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import textwrap

from pptx import Presentation
from pptx.dml.color import RGBColor
from pptx.enum.shapes import MSO_SHAPE
from pptx.enum.text import MSO_ANCHOR, MSO_AUTO_SIZE, PP_ALIGN
from pptx.util import Inches, Pt

HERE = os.path.dirname(os.path.abspath(__file__))
REPORT = os.path.join(HERE, "..", "report")
IMG = os.path.join(HERE, "img")

W, H = 13.333, 7.5
INK, INK2, MUTED = "0b0b0b", "52514e", "8a8984"
BLUE, ORANGE, AQUA = "2a78d6", "eb6834", "1baf7a"
SURFACE, CODEBG, LINE, TINT = "fcfcfb", "f2f1ee", "e4e3df", "eaf2fc"
SANS, MONO = "Calibri", "Consolas"
PAD = 0.08  # text-frame inset (inches), the same in the deck and the preview

res = json.load(open(os.path.join(REPORT, "bench", "results.json")))
abl = json.load(open(os.path.join(REPORT, "bench", "ablation.json")))


def impl(problem, label=None, lang=None):
    for i in res["problems"][problem]["impls"]:
        if (label and i["label"] == label) or (lang and i["lang"] == lang and not label):
            return i
    raise KeyError((problem, label, lang))


# ---------------------------------------------------------------- slide description helpers

def T(x, y, w, h, paras, align="l", anchor="t", fill=None):
    """Text box. paras: list of (text, size, {bold, color, mono, space}) or plain strings."""
    return {"k": "text", "x": x, "y": y, "w": w, "h": h, "paras": paras, "align": align,
            "anchor": anchor, "fill": fill}


def P(text, size=18, color=INK, bold=False, mono=False, space=6, bullet=False):
    return {"text": text, "size": size, "color": color, "bold": bold, "mono": mono,
            "space": space, "bullet": bullet}


def CODE(x, y, w, h, code, size=13, title=None):
    els = [{"k": "rect", "x": x, "y": y, "w": w, "h": h, "fill": CODEBG, "radius": True}]
    top = y
    if title:
        els.append(T(x + 0.1, y + 0.06, w - 0.2, 0.38, [P(title, 12, INK2, bold=True, space=0)]))
        top = y + 0.36
    lines = textwrap.dedent(code).strip("\n").split("\n")
    els.append(T(x + 0.1, top + 0.04, w - 0.2, h - (top - y) - 0.08,
                 [P(line if line else " ", size, INK, mono=True, space=0) for line in lines]))
    return els


def BOX(x, y, w, h, fill, radius=True, line=None):
    return {"k": "rect", "x": x, "y": y, "w": w, "h": h, "fill": fill, "radius": radius, "line": line}


def STAT(x, y, w, h, big, small, color=BLUE, big_size=34):
    return [BOX(x, y, w, h, SURFACE, line=LINE),
            {"k": "rect", "x": x, "y": y + 0.12, "w": 0.07, "h": h - 0.24, "fill": color, "radius": False},
            T(x + 0.2, y + 0.06, w - 0.3, 0.84, [P(big, big_size, INK, bold=True, space=0)], anchor="b"),
            T(x + 0.2, y + 0.88, w - 0.3, h - 0.94, [P(small, 13, INK2, space=0)])]


def IMAGE(x, y, w, path):
    from PIL import Image
    pw, ph = Image.open(path).size
    return {"k": "image", "x": x, "y": y, "w": w, "h": w * ph / pw, "path": path}


def TABLE(x, y, w, rows, col_w, size=13, row_h=0.36, header=True, align=None):
    return {"k": "table", "x": x, "y": y, "w": w, "h": row_h * len(rows), "rows": rows,
            "col_w": col_w, "size": size, "row_h": row_h, "header": header,
            "align": align or ["l"] * len(rows[0])}


def slide(title, els, notes="", kicker=None):
    head = []
    if kicker:
        head.append(T(0.6, 0.32, 12, 0.4, [P(kicker.upper(), 12, BLUE, bold=True, space=0)]))
    head.append(T(0.6, 0.62 if kicker else 0.45, 12.1, 0.75, [P(title, 30, INK, bold=True, space=0)]))
    return {"els": head + els, "notes": notes}


# ---------------------------------------------------------------- figures

def render_figures():
    os.makedirs(IMG, exist_ok=True)
    out = {}
    for name in ("runtime_vs_cpp", "code_size", "build_time"):
        src = os.path.join(REPORT, "figures", name + ".svg")
        dst = os.path.join(IMG, name + ".png")
        subprocess.run(["convert", "-density", "220", src, dst], check=True)
        out[name] = dst
    return out


# ---------------------------------------------------------------- the deck

def build_slides(fig):
    tc_v, tc_c, tc_p = impl("testcases", lang="vulp"), impl("testcases", lang="cpp"), impl("testcases", lang="py")
    lcs_dp = impl("lcs", "Vulp (dp fn)")
    lcs_i32 = impl("lcs", "Vulp (i32 table)")
    lcs_cpp = impl("lcs", "C++ (table)")
    lcs_memo = impl("lcs", "C++ (memo)")
    builds_v = [i["compile_s"] for p in res["problems"].values() for i in p["impls"] if i["lang"] == "vulp"]
    builds_c = [i["compile_s"] for p in res["problems"].values() for i in p["impls"] if i["lang"] == "cpp"]
    S = []

    # 1. title
    S.append({"els": [
        BOX(0, 0, W, H, SURFACE, radius=False),
        {"k": "rect", "x": 0.6, "y": 1.55, "w": 0.12, "h": 2.35, "fill": BLUE, "radius": False},
        T(0.95, 1.3, 6.4, 1.5, [P("Vulp", 66, INK, bold=True, space=0)]),
        T(0.95, 2.8, 6.0, 1.3, [P("A language and compiler for competitive programming", 26, INK2, space=0)]),
        T(0.95, 4.3, 6.0, 1.3, [P("V1 project report", 18, INK, bold=True, space=2),
                                 P("Vibhaas · October 2026", 16, INK2, space=2),
                                 P("github.com/vulp-lang/vulp", 16, BLUE, space=0)]),
        *CODE(7.45, 1.55, 5.3, 4.0, """
            dp fn fib(int n) -> int {
                if n <= 1: return n;
                return fib(n - 1) + fib(n - 2);
            }

            TC: {
                read int n;
                read int[n] a;
                int best = a[0];
                for x in a: best = max(best, x);
                println(fib(n), " ", best);
            }
            """, 14),
    ], "notes": "Vulp is a small statically typed language for competitive programming, compiled with "
                "LLVM. This talk shows the language through examples, then measures it against C++ and "
                "Python on six classic problems."})

    # 2. motivation
    cards = [("C++", ORANGE, "Fast, but verbose",
              ["Every solution repeats fast-I/O setup, the testcase loop and memo tables.",
               "Out-of-bounds and overflow bugs fail silently."]),
             ("Python", AQUA, "Short, but slow",
              [f"4–26× slower than C++ on our benchmarks.",
               "Often over the time limit; deep recursion needs workarounds."]),
             ("Vulp", BLUE, "Short and fast",
              ["Contest constructs built into the language: TC:, read, dp fn, any-width integers.",
               "Compiled with LLVM: C++ speed, with runtime checks."])]
    els = []
    for k, (name, color, head, lines) in enumerate(cards):
        x = 0.6 + k * 4.1
        els += [BOX(x, 1.75, 3.85, 4.2, SURFACE, line=LINE),
                {"k": "rect", "x": x, "y": 1.75, "w": 3.85, "h": 0.1, "fill": color, "radius": False},
                T(x + 0.25, 2.05, 3.4, 0.5, [P(name, 16, color if color != AQUA else "0f7a54", bold=True, space=0)]),
                T(x + 0.25, 2.5, 3.4, 0.6, [P(head, 21, INK, bold=True, space=0)]),
                T(x + 0.25, 3.3, 3.4, 2.5, [P(l, 16, INK2, space=10, bullet=True) for l in lines])]
    S.append(slide("Contest code today: fast or short, rarely both", els,
                   "Contestants choose between C++ (fast but boilerplate-heavy and unsafe) and Python "
                   "(short but slow). Vulp aims to be both short and fast.", "Motivation"))

    # 3. at a glance
    feats = ["Integers iN / uN of 1–256 bits, rounded up to 8, 16, 32, 64, 128 or 256; int<lo..hi>",
             "int, char, bool, float, string, arrays, vec, map, struct, ranges [1..n]",
             "if / elif / else, while, for ( ; ; ), for x in …",
             "Functions, & references, recursion on a 1 GiB stack",
             "dp fn: compiler-checked pure functions, memoised automatically",
             "TC: testcase blocks, fast read / print / println",
             "Run from source (LLVM JIT) or build a native executable"]
    S.append(slide("Vulp at a glance", [
        *CODE(0.6, 1.6, 6.0, 5.2, """
            int MOD = 1_000_000_007;

            dp fn ways(int n) -> int {      # memoised
                if n <= 1: return 1;
                return (ways(n - 1) + ways(n - 2)) % MOD;
            }

            TC: {                           # t testcases
                read int n;
                read int[n] a;
                a.sort();
                println(ways(n), " ", a);
            }
            """, 14, "example.vp"),
        T(7.0, 1.6, 5.8, 5.3, [P(f, 17, INK, space=11, bullet=True) for f in feats]),
    ], "The whole V1 feature list on one slide. Everything on the left is a complete program: "
       "no main, no includes, no I/O setup.", "Language"))

    # 4. TC example
    S.append(slide("The testcase loop", [
        *CODE(0.6, 1.55, 5.0, 4.4, """
            TC: {
                read int n;
                read int[n] a;
                int best = a[0], cur = 0;
                for x in a: {
                    cur = max(cur + x, x);
                    best = max(best, cur);
                }
                println(best);
            }
            """, 14, f"Vulp · {tc_v['lines']} lines · {tc_v['tokens']} tokens"),
        *CODE(5.85, 1.55, 6.9, 4.4, """
            #include <bits/stdc++.h>
            using namespace std;
            int main() {
                ios::sync_with_stdio(false); cin.tie(nullptr);
                int t; cin >> t;
                while (t--) {
                    int n; cin >> n;
                    vector<long long> a(n);
                    for (auto& x : a) cin >> x;
                    long long best = a[0], cur = 0;
                    for (long long x : a) {
                        cur = max(cur + x, x); best = max(best, cur);
                    }
                    cout << best << "\\n";
                }
            }
            """, 12, f"C++ · {tc_c['lines']} lines · {tc_c['tokens']} tokens"),
        T(0.6, 6.15, 12.1, 0.55, [P(f"200,000 testcases:   Vulp {tc_v['time_s']:.3f} s   ·   "
                                    f"C++ {tc_c['time_s']:.3f} s   ·   Python {tc_p['time_s']:.3f} s", 17, INK, bold=True, space=0)],
          fill=TINT, anchor="m"),
    ], "TC: reads the testcase count and runs the block that many times. read uses a buffered "
       "reader, so there are no sync_with_stdio tricks, and it is as fast as the tuned C++.", "Examples"))

    # 5. dp fn
    S.append(slide("dp fn: write the recurrence, get the cache", [
        *CODE(0.6, 1.55, 6.0, 2.2, """
            dp fn fib(int n) -> int {
                if n <= 1: return n;
                return fib(n - 1) + fib(n - 2);
            }
            read int n;
            println(fib(n));
            """, 15),
        *STAT(0.6, 3.95, 2.9, 1.6, "0.198 s", "Vulp plain fib(40)", ORANGE),
        *STAT(3.7, 3.95, 2.9, 1.6, "1.57 ms", "Vulp dp fn fib(40), 41 states", BLUE),
        T(0.6, 5.75, 6.0, 0.6, [P("Plain C++: 0.107 s; plain Python: 8.26 s.", 16, INK2, space=0)]),
        T(7.0, 1.5, 5.8, 0.55, [P("Checked by the compiler", 18, INK, bold=True, space=0)]),
        T(7.0, 2.05, 5.8, 2.3, [P("Parameters: integers, chars or bools", 16, INK2, space=8, bullet=True),
                                P("Result: a scalar", 16, INK2, space=8, bullet=True),
                                P("No global writes or I/O, including through callees", 16, INK2, space=8, bullet=True),
                                P("May read globals: then its cache is cleared before every testcase, in O(1)", 16, INK2, space=8, bullet=True)]),
        *CODE(7.0, 4.55, 5.75, 1.25, """
            $ vulpc -v examples/08_lcs_testcases.vp
            [vulpc] dp fn lcs: memoised; cache cleared
                    before every TC iteration (reads globals)
            """, 12),
    ], "For fib(40), dp fn takes 0.00157 s, close to process startup. The compiler checks the "
       "function's cache rules and resets input-dependent caches between testcases.", "Examples"))

    # 6. safety
    S.append(slide("Mistakes caught early, with clear messages", [
        T(0.6, 1.45, 6, 0.5, [P("At compile time (exit code 65)", 16, INK, bold=True, space=0)]),
        *CODE(0.6, 1.95, 12.15, 2.35, """
            type.vp:2:6: semantic error: condition of 'if' must be bool, found i64
                if n % 2: println("odd");
                     ^
            purity.vp:3:5: semantic error: dp function 'f' must not modify global variables (its results are cached)
            dangle.vp:6:11: semantic error: cannot pass an element of 'all' by reference together with 'all' itself:
                            'grow' could resize or replace the container and leave the element reference dangling
                grow(all, all[0]);
                          ^
            """, 13),
        T(0.6, 4.55, 6, 0.5, [P("At run time (exit code 1)", 16, INK, bold=True, space=0)]),
        *CODE(0.6, 5.05, 7.2, 0.6, "vulp runtime error (line 3): index 3 out of bounds for length 3", 13),
        T(8.1, 4.95, 4.65, 1.3, [P("Bounds, division by zero, empty pop, end of input, input overflow. "
                                  "Cost: 11% on dense LCS, 0-2% elsewhere.", 15, INK2, space=0)]),
    ], "Type errors, dp purity violations and dangling references are rejected before the program "
       "runs. Runtime errors print the line and stop instead of producing a wrong answer.", "Examples"))

    # 7. integers
    S.append(slide("Integers of any width", [
        *CODE(0.6, 1.55, 6.4, 3.0, """
            int<0..1000000007> m = 1000000007;   # -> i32
            i21 small = 1000000;                 # -> i32
            i256 f = 1;
            for i in [1..50]: f *= i;            # 50!
            println("50! = ", f);
            u128 two = 1;
            two <<= 127;
            println("2^127 = ", two);
            """, 14, "widths.vp"),
        *CODE(0.6, 5.55, 12.15, 0.85, """
            50! = 30414093201713378043612608166064768844377641568960512000000000000
            2^127 = 170141183460469231731687303715884105728
            """, 13),
        T(7.3, 1.5, 5.45, 3.9, [P("Any width from 1 to 256 bits, rounded up to 8, 16, 32, 64, 128 or 256", 17, INK, space=12, bullet=True),
                                 P("int<lo..hi> picks the smallest width that holds the range", 17, INK, space=12, bullet=True),
                                 P("Widths are storage: arithmetic on narrow types runs in 64 bits, so x * x cannot overflow", 17, INK, space=12, bullet=True),
                                 P("An i32 DP table halves memory and runs 1.4× faster than int", 17, INK, space=12, bullet=True)]),
    ], "Big-number problems up to 256 bits need no library. Narrow types are a memory and speed "
       "tool, as the ablation later shows.", "Examples"))

    # 8. architecture
    stages = [("Lexer", "tokens"), ("Parser", "Pratt · AST"), ("Sema", "types · purity · aliasing"),
              ("Codegen", "LLVM IR"), ("LLVM -O2", "optimise")]
    els = []
    for k, (name, sub) in enumerate(stages):
        x = 0.6 + k * 2.05
        els += [BOX(x, 1.7, 1.8, 1.15, TINT, line=None),
                T(x, 1.78, 1.8, 0.5, [P(name, 17, INK, bold=True, space=0)], align="c"),
                T(x, 2.22, 1.8, 0.6, [P(sub, 11, INK2, space=0)], align="c")]
        els.append(T(x + 1.75, 1.95, 0.35, 0.6, [P("›", 20, MUTED, space=0)], align="c"))
    els += [BOX(10.85, 1.7, 1.9, 0.52, SURFACE, line=BLUE),
            T(10.85, 1.72, 1.9, 0.48, [P("JIT: run now", 14, INK, bold=True, space=0)], align="c", anchor="m"),
            BOX(10.85, 2.33, 1.9, 0.52, SURFACE, line=BLUE),
            T(10.85, 2.35, 1.9, 0.48, [P("native binary", 14, INK, bold=True, space=0)], align="c", anchor="m")]
    els.append(TABLE(0.6, 3.3, 6.6, [
        ["Component", "Lines of C++"],
        ["Lexer", "471"], ["Parser + AST", "1,136"], ["Semantic analysis", "1,733"],
        ["Code generation", "1,945"], ["Driver, diagnostics, CLI", "502"], ["Runtime library", "1,031"],
        ["Total", "6,818"]], [4.4, 2.2], 13, 0.4, align=["l", "r"]))
    els.append(T(7.6, 3.3, 5.15, 3.5, [
        P("dp fn → a hash table, the body, and a wrapper every call goes through; O(1) reset per testcase", 15, INK2, space=10, bullet=True),
        P("Value semantics without a GC: one owner per buffer, generated copy / free helpers", 15, INK2, space=10, bullet=True),
        P("One code generator, two back ends: ORC JIT or object file + libvulprt.a", 15, INK2, space=10, bullet=True)]))
    S.append(slide("How the compiler works", els,
                   "A classic pipeline modelled on the vex compiler, lowering a typed AST straight to "
                   "LLVM IR. About 6,800 lines of C++ in total.", "Compiler"))

    # 9. benchmark method
    S.append(slide("Benchmark: six classic problems, three languages", [
        TABLE(0.6, 1.55, 7.6, [
            ["Problem", "Input", "Stresses"],
            ["Sieve", "primes ≤ 2·10⁷", "tight array loops"],
            ["Sort + I/O", "10⁶ integers", "parsing, sort, output"],
            ["LCS", "two 3000-char strings", "2-D DP, 9·10⁶ states"],
            ["Dijkstra", "10⁵ V, 5·10⁵ E", "structs, heap, refs"],
            ["Testcases", "2·10⁵ small tests", "TC: and small reads"],
            ["Map count", "10⁶ ops, 64-bit keys", "ordered map"]], [1.75, 2.75, 3.1], 14, 0.5),
        T(8.6, 1.55, 4.15, 5.2, [
            P("Vulp: vulpc -O2", 15, INK, space=8, bullet=True),
            P("C++: g++ -O2, idiomatic contest style", 15, INK, space=8, bullet=True),
            P("Python 3.12: idiomatic, built-ins", 15, INK, space=8, bullet=True),
            P("7 interleaved rounds, pinned to one CPU; median reported", 15, INK, space=8, bullet=True),
            P("All outputs byte-identical (SHA-256), or the run aborts", 15, INK, space=8, bullet=True),
            P("Seeded inputs; one script reproduces everything", 15, INK, space=8, bullet=True)]),
    ], "Each problem has a Vulp, C++ and Python version. The harness checks that all of them print "
       "the same bytes before timing. Runs are interleaved and pinned to one CPU on the reference laptop.",
        "Evaluation"))

    # 10. runtime
    S.append(slide("Runtime: Vulp vs C++ and Python", [
        IMAGE(0.5, 1.45, 8.2, fig["runtime_vs_cpp"]),
        T(9.0, 1.6, 3.8, 5.2, [
            P("Within 17% of C++ on four tasks", 16, INK, space=10, bullet=True),
            P("Vulp is 4.6x to 19.5x faster than Python on five tasks", 16, INK, space=10, bullet=True),
            P("LCS: i32 tables, 1.37x C++ time; about 38 MiB each", 16, INK, space=10, bullet=True),
            P("Map counting is 1.42x C++ time and nearly equal to Python", 16, INK, space=10, bullet=True)]),
    ], "Map allocations and LCS table representation account for the largest gaps in these runs.", "Results"))

    # 11. code size
    sort_v = impl("sort_io", lang="vulp")
    S.append(slide("Code: 30–81% fewer tokens than C++", [
        IMAGE(0.5, 1.45, 8.2, fig["code_size"]),
        *CODE(9.0, 1.6, 3.75, 1.75, """
            read int n;
            read int[n] a;
            a.sort();
            println(a);
            """, 15, f"All of sort + I/O: {sort_v['tokens']} tokens"),
        T(9.0, 3.6, 3.75, 3.2, [
            P("Shortest of the three languages on 5 of 6 problems", 16, INK, space=10, bullet=True),
            P("Dijkstra is longer: V1 has no priority queue, so the program carries a 30-line heap", 16, INK, space=10, bullet=True)]),
    ], "Tokens, not lines, so formatting does not matter. The Dijkstra outlier points at the most "
       "useful library addition for V2.", "Results"))

    # 12. build
    S.append(slide("Build in 0.05 s, or run straight from source", [
        IMAGE(0.5, 1.45, 8.2, fig["build_time"]),
        *STAT(9.0, 1.5, 3.75, 1.5, f"{min(builds_v):.2f}–{max(builds_v):.2f} s", "vulpc: build a native executable", BLUE),
        *STAT(9.0, 3.15, 3.75, 1.5, f"{min(builds_c):.2f}–{max(builds_c):.2f} s", "g++ -O2 with <bits/stdc++.h>", ORANGE),
        *STAT(9.0, 4.8, 3.75, 1.5, "+16-35 ms", "JIT: compile in memory and run", BLUE),
    ], "Vulp builds take 0.05-0.06 s and the C++ builds take 0.96-1.05 s. The C++ sources "
       "include bits/stdc++.h. JIT startup adds roughly 16-35 ms to execution.", "Results"))

    # 13. dp fn price
    S.append(slide("Memoization and dense tables", [
        TABLE(0.4, 1.65, 8.3, [
            ["Implementation", "Time (s)", "Peak memory (MiB)"],
            ["Vulp dp fn", f"{lcs_dp['time_s']:.3f}", f"{lcs_dp['rss_mib']:.1f}"],
            ["Vulp i32 table", f"{lcs_i32['time_s']:.3f}", f"{lcs_i32['rss_mib']:.1f}"],
            ["C++ bottom-up table", f"{lcs_cpp['time_s']:.3f}", f"{lcs_cpp['rss_mib']:.1f}"],
            ["C++ recursion with unordered_map", f"{lcs_memo['time_s']:.3f}", f"{lcs_memo['rss_mib']:.1f}"]],
            [4.2, 1.5, 2.6], 14, 0.62, align=["l", "r", "r"]),
        T(8.9, 1.65, 3.8, 5.1, [
            P("LCS fills a dense 3000 by 3000 table", 16, INK, space=10, bullet=True),
            P(f"Vulp dp fn takes {lcs_dp['time_s']:.3f} s and uses {lcs_dp['rss_mib']:.1f} MiB", 16, INK, space=10, bullet=True),
            P(f"The Vulp i32 table is {lcs_dp['time_s'] / lcs_i32['time_s']:.0f}x faster and uses {lcs_dp['rss_mib'] / lcs_i32['rss_mib']:.0f}x less memory", 16, INK, space=10, bullet=True),
            P("dp fn is useful when recursion revisits states; a dense table suits this workload", 16, INK, space=10, bullet=True)]),
    ], "On this dense problem, a table avoids hash lookups and stores each value in a compact array. "
       "The Fibonacci comparison shows the benefit of caching when plain recursion repeats work.", "Results"))

    # 14. ablations
    rows = [["Program", "Checks", "--no-checks", "Cost"]]
    names = {"sieve": "Sieve", "sort_io": "Sort + I/O", "lcs": "LCS", "dijkstra": "Dijkstra",
             "testcases": "Testcases", "map_count": "Map count"}
    for r in abl["checks"]:
        label = names[r["problem"]] + (" (dp fn)" if "dp fn" in r["label"] else " (table)" if "table" in r["label"] else "")
        rows.append([label, f"{r['checks_s']:.3f} s", f"{r['no_checks_s']:.3f} s",
                     f"{(r['checks_s'] / r['no_checks_s'] - 1) * 100:+.0f}%"])
    wrows = [["LCS table element", "Time", "Memory"]]
    for wv in abl["widths"]:
        wrows.append([wv["variant"].replace("int (64-bit) table", "int").replace("i32 table", "i32"),
                      f"{wv['time_s']:.3f} s", f"{wv['rss_mib']:.0f} MiB"])
    S.append(slide("Ablations: checks are cheap, widths matter", [
        T(0.6, 1.4, 6.5, 0.5, [P("Runtime checks on vs off", 16, INK, bold=True, space=0)]),
        TABLE(0.6, 1.9, 6.6, rows, [2.5, 1.4, 1.5, 1.2], 13, 0.4, align=["l", "r", "r", "r"]),
        T(7.6, 1.4, 5.2, 0.5, [P("int vs i32 elements (LCS table)", 16, INK, bold=True, space=0)]),
        TABLE(7.6, 1.9, 5.15, wrows, [2.75, 1.2, 1.2], 13, 0.4, align=["l", "r", "r"]),
        T(7.6, 4.2, 5.15, 2.4, [
            P("Checks cost 11% on dense LCS and 0-2% elsewhere", 16, INK, space=10, bullet=True),
            P("In the width experiment, i32 uses about half the memory and is 1.5x faster than int", 16, INK, space=10, bullet=True)]),
    ], "The measured i32 table uses 38.1 MiB and takes 0.033 s; the 64-bit table uses 72.6 MiB and takes 0.048 s.", "Results"))

    # 15. quality
    S.append(slide("Tested at every step, reviewed until satisfied", [
        *STAT(0.6, 1.5, 2.9, 1.6, "142", "unit tests", BLUE),
        *STAT(3.68, 1.5, 2.9, 1.6, "34 × 3", "end-to-end programs: JIT, binary, poisoned frees", BLUE),
        *STAT(6.76, 1.5, 2.9, 1.6, "88", "AddressSanitizer runs, 0 reports", BLUE),
        *STAT(9.84, 1.5, 2.9, 1.6, "26", "review findings, all fixed + tested", BLUE),
        TABLE(0.6, 3.4, 12.15, [
            ["Round", "Findings", "Examples"],
            ["1", "10", "memory never freed (1 GB loops), MIN / -1, O(capacity) dp reset, input overflow"],
            ["2", "7", "dangling & references after a resize, float → i256 crash, leaked temporaries"],
            ["3–5", "7 + 1", "aliasing bypass f(v, v), recursive-struct crash, evaluation order"],
            ["6–7", "0 + 1", "satisfied: 300 generated programs, 110 under ASan; + 1 self-found fix (erase key)"]],
            [1.0, 1.3, 9.85], 13, 0.46),
    ], "An independent reviewer agent attacked the compiler in a loop until it found nothing more. "
       "Every finding became a regression test.", "Quality"))

    # 16. limitations
    S.append(slide("Limitations and next steps", [
        T(0.6, 1.45, 5.9, 0.55, [P("Not in V1", 18, INK, bold=True, space=0)]),
        T(0.6, 2.0, 5.9, 4.6, [P(t, 16, INK2, space=9, bullet=True) for t in (
            "Lambdas, generics, custom comparators",
            "Priority queue and sorted set",
            "Fixed-precision float output, line input",
            "Runtime checks of int<lo..hi>",
            "The & rules are conservative: a few safe calls are rejected")]),
        T(6.9, 1.45, 5.9, 0.55, [P("Next, guided by the measurements", 18, INK, bold=True, space=0)]),
        T(6.9, 2.0, 5.9, 4.6, [P(t, 16, INK, space=9, bullet=True) for t in (
            "pq<T> and set<T>: removes Dijkstra's hand-written heap",
            "Memo table specialised by key size, one probe per miss: target parity with C++ (2.7x slower on LCS)",
            "Map values stored inline, for k, v in m: reduce the 1.42x gap",
            "Suggest i32 when DP values provably fit (about half the memory in LCS)")]),
    ], "Each next step is tied to a number measured in this report.", "Wrap-up"))

    # 17. try it
    S.append(slide("Try it", [
        *CODE(0.6, 1.55, 8.0, 1.95, """
            cmake -S . -B build -G Ninja && cmake --build build
            ./build/vulpc examples/01_hello.vp           # JIT
            ./build/vulpc examples/03_testcases.vp -o tc  # binary
            ./tc < examples/03_testcases.in
            (cd build && ctest)                          # all tests
            python3 docs/report/bench/run_bench.py       # benchmarks
            """, 14),
        T(9.0, 1.55, 3.8, 3.4, [
            P("github.com/vulp-lang/vulp", 18, BLUE, bold=True, space=12),
            P("docs/tutorial.md: step-by-step guide", 15, INK2, space=8, bullet=True),
            P("docs/language.md: reference", 15, INK2, space=8, bullet=True),
            P("docs/report/REPORT.md: this evaluation", 15, INK2, space=8, bullet=True),
            P("examples/: 10 runnable programs", 15, INK2, space=8, bullet=True)]),
        T(0.6, 4.6, 12.1, 1.0, [P("Thank you", 30, INK, bold=True, space=0)]),
    ], "Needs CMake, a C++20 compiler and LLVM 18. Every number in this deck is reproduced by the "
       "scripts in docs/report/bench.", "Wrap-up"))
    return S


# ---------------------------------------------------------------- pptx writer

def rgb(h):
    return RGBColor.from_string(h)


def write_pptx(slides, path):
    prs = Presentation()
    prs.slide_width, prs.slide_height = Inches(W), Inches(H)
    blank = prs.slide_layouts[6]
    for n, s in enumerate(slides, 1):
        sl = prs.slides.add_slide(blank)
        bg = sl.background.fill
        bg.solid()
        bg.fore_color.rgb = rgb(SURFACE)
        for e in s["els"]:
            k = e["k"]
            if k == "rect":
                shp = sl.shapes.add_shape(MSO_SHAPE.ROUNDED_RECTANGLE if e.get("radius") else MSO_SHAPE.RECTANGLE,
                                          Inches(e["x"]), Inches(e["y"]), Inches(e["w"]), Inches(e["h"]))
                if e.get("radius"):
                    shp.adjustments[0] = min(0.5, 0.08 / min(e["w"], e["h"]))
                shp.fill.solid()
                shp.fill.fore_color.rgb = rgb(e["fill"])
                if e.get("line"):
                    shp.line.color.rgb = rgb(e["line"])
                    shp.line.width = Pt(1)
                else:
                    shp.line.fill.background()
                shp.shadow.inherit = False
            elif k == "text":
                tb = sl.shapes.add_textbox(Inches(e["x"]), Inches(e["y"]), Inches(e["w"]), Inches(e["h"]))
                if e.get("fill"):
                    tb.fill.solid()
                    tb.fill.fore_color.rgb = rgb(e["fill"])
                tf = tb.text_frame
                tf.word_wrap = True
                tf.auto_size = MSO_AUTO_SIZE.NONE
                for side in ("margin_left", "margin_right", "margin_top", "margin_bottom"):
                    setattr(tf, side, Inches(PAD))
                tf.vertical_anchor = {"t": MSO_ANCHOR.TOP, "m": MSO_ANCHOR.MIDDLE, "b": MSO_ANCHOR.BOTTOM}[e["anchor"]]
                for i, p in enumerate(e["paras"]):
                    para = tf.paragraphs[0] if i == 0 else tf.add_paragraph()
                    para.alignment = {"l": PP_ALIGN.LEFT, "c": PP_ALIGN.CENTER, "r": PP_ALIGN.RIGHT}[e["align"]]
                    para.space_after = Pt(p["space"])
                    para.line_spacing = 1.0 if p["mono"] else 1.1
                    text = ("•  " + p["text"]) if p["bullet"] else p["text"]
                    run = para.add_run()
                    run.text = text
                    f = run.font
                    f.size = Pt(p["size"])
                    f.bold = p["bold"]
                    f.color.rgb = rgb(p["color"])
                    f.name = MONO if p["mono"] else SANS
            elif k == "image":
                sl.shapes.add_picture(e["path"], Inches(e["x"]), Inches(e["y"]), Inches(e["w"]), Inches(e["h"]))
            elif k == "table":
                rows, cols = len(e["rows"]), len(e["rows"][0])
                gt = sl.shapes.add_table(rows, cols, Inches(e["x"]), Inches(e["y"]), Inches(e["w"]), Inches(e["h"]))
                tbl = gt.table
                tbl.first_row = e["header"]
                tbl.horz_banding = False
                for c, cw in enumerate(e["col_w"]):
                    tbl.columns[c].width = Inches(cw)
                for r in range(rows):
                    tbl.rows[r].height = Inches(e["row_h"])
                    for c in range(cols):
                        cell = tbl.cell(r, c)
                        cell.fill.solid()
                        cell.fill.fore_color.rgb = rgb(TINT if (r == 0 and e["header"]) else SURFACE if r % 2 else "f6f5f2")
                        cell.margin_left = cell.margin_right = Inches(PAD)
                        cell.margin_top = cell.margin_bottom = Inches(0.03)
                        cell.vertical_anchor = MSO_ANCHOR.MIDDLE
                        tf = cell.text_frame
                        tf.word_wrap = True
                        para = tf.paragraphs[0]
                        para.alignment = {"l": PP_ALIGN.LEFT, "r": PP_ALIGN.RIGHT, "c": PP_ALIGN.CENTER}[e["align"][c]]
                        run = para.add_run()
                        run.text = e["rows"][r][c]
                        run.font.size = Pt(e["size"])
                        run.font.name = SANS
                        run.font.bold = r == 0 and e["header"]
                        run.font.color.rgb = rgb(INK)
        # footer
        ft = sl.shapes.add_textbox(Inches(0.6), Inches(7.0), Inches(12.1), Inches(0.35))
        ft.text_frame.margin_left = ft.text_frame.margin_right = Inches(PAD)
        p = ft.text_frame.paragraphs[0]
        r = p.add_run()
        r.text = f"Vulp V1  ·  {n}"
        r.font.size, r.font.name = Pt(10), SANS
        r.font.color.rgb = rgb(MUTED)
        if s.get("notes"):
            sl.notes_slide.notes_text_frame.text = s["notes"]
    prs.save(path)


# ---------------------------------------------------------------- preview renderer (layout check)

def preview(slides, outdir):
    from PIL import Image, ImageDraw, ImageFont
    dpi = 100
    fonts = "/usr/share/fonts/truetype/dejavu/"

    def font(size, bold, mono):
        name = ("DejaVuSansMono" if mono else "DejaVuSans") + ("-Bold" if bold else "") + ".ttf"
        return ImageFont.truetype(fonts + name, int(round(size * dpi / 72)))

    def px(v):
        return int(round(v * dpi))

    problems = []
    os.makedirs(outdir, exist_ok=True)
    for n, s in enumerate(slides, 1):
        img = Image.new("RGB", (px(W), px(H)), "#" + SURFACE)
        d = ImageDraw.Draw(img)
        for e in s["els"]:
            k = e["k"]
            box = (px(e["x"]), px(e["y"]), px(e["x"] + e["w"]), px(e["y"] + e["h"]))
            if e["x"] + e["w"] > W + 0.01 or e["y"] + e["h"] > H + 0.01:
                problems.append(f"slide {n}: element off the slide: {k} at {e['x']},{e['y']}")
            if k == "rect":
                d.rounded_rectangle(box, radius=8 if e.get("radius") else 0, fill="#" + e["fill"],
                                    outline=("#" + e["line"]) if e.get("line") else None)
            elif k == "image":
                im = Image.open(e["path"]).convert("RGB").resize((box[2] - box[0], box[3] - box[1]))
                img.paste(im, box[:2])
            elif k == "text":
                if e.get("fill"):
                    d.rectangle(box, fill="#" + e["fill"])
                inner_w = px(e["w"] - 2 * PAD)
                lines = []  # (text, font, color, height, gap_after)
                for p in e["paras"]:
                    f = font(p["size"], p["bold"], p["mono"])
                    text = ("•  " + p["text"]) if p["bullet"] else p["text"]
                    lh = p["size"] * dpi / 72 * (1.2 if p["mono"] else 1.32)
                    words, cur, wrapped = text.split(" "), "", []
                    if p["mono"]:
                        wrapped = [text]
                        if d.textlength(text, font=f) > inner_w:
                            problems.append(f"slide {n}: code line too wide: {text.strip()[:60]!r}")
                    else:
                        for w_ in words:
                            trial = (cur + " " + w_) if cur else w_
                            if d.textlength(trial, font=f) <= inner_w or not cur:
                                cur = trial
                            else:
                                wrapped.append(cur)
                                cur = ("   " if p["bullet"] else "") + w_
                        wrapped.append(cur)
                    for i, wl in enumerate(wrapped):
                        lines.append((wl, f, p["color"], lh, p["space"] * dpi / 72 if i == len(wrapped) - 1 else 0))
                total = sum(l[3] + l[4] for l in lines) - (lines[-1][4] if lines else 0)
                avail = px(e["h"] - 2 * PAD)
                if total > avail + 1:
                    problems.append(f"slide {n}: text overflows its box by {(total - avail) / dpi:.2f} in: "
                                    f"{e['paras'][0]['text'][:50]!r}")
                y = px(e["y"] + PAD)
                if e["anchor"] == "m":
                    y += max(0, (avail - total) / 2)
                elif e["anchor"] == "b":
                    y += max(0, avail - total)
                for wl, f, color, lh, gap in lines:
                    tw = d.textlength(wl, font=f)
                    x = px(e["x"] + PAD)
                    if e["align"] == "c":
                        x = px(e["x"]) + (px(e["w"]) - tw) / 2
                    elif e["align"] == "r":
                        x = px(e["x"] + e["w"] - PAD) - tw
                    d.text((x, y), wl, font=f, fill="#" + color)
                    y += lh + gap
            elif k == "table":
                f = font(e["size"], False, False)
                fb = font(e["size"], True, False)
                y = px(e["y"])
                for r, row in enumerate(e["rows"]):
                    x = px(e["x"])
                    for c, cell in enumerate(row):
                        cw = px(e["col_w"][c])
                        fill = TINT if (r == 0 and e["header"]) else SURFACE if r % 2 else "f6f5f2"
                        d.rectangle((x, y, x + cw, y + px(e["row_h"])), fill="#" + fill, outline="#ffffff")
                        ff = fb if (r == 0 and e["header"]) else f
                        tw = d.textlength(cell, font=ff)
                        if tw > cw - px(2 * PAD):
                            problems.append(f"slide {n}: table cell too wide: {cell[:50]!r}")
                        tx = x + px(PAD) if e["align"][c] == "l" else x + cw - px(PAD) - tw
                        d.text((tx, y + (px(e["row_h"]) - e["size"] * dpi / 72 * 1.2) / 2), cell, font=ff, fill="#" + INK)
                        x += cw
                    y += px(e["row_h"])
                if abs(sum(e["col_w"]) - e["w"]) > 0.01:
                    problems.append(f"slide {n}: table column widths do not add up")
        d.text((px(0.6 + PAD), px(7.0 + PAD)), f"Vulp V1  ·  {n}", font=font(10, False, False), fill="#" + MUTED)
        img.save(os.path.join(outdir, f"slide_{n:02d}.png"))
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview", help="also draw PNG previews of every slide into this directory")
    args = ap.parse_args()
    if not shutil.which("convert"):
        sys.exit("ImageMagick `convert` is needed to render the figures")
    slides = build_slides(render_figures())
    out = os.path.join(HERE, "vulp_v1.pptx")
    write_pptx(slides, out)
    print("wrote", out, f"({len(slides)} slides)")
    if args.preview:
        problems = preview(slides, args.preview)
        for p in problems:
            print("LAYOUT:", p)
        print(f"previews in {args.preview}; {len(problems)} layout problem(s)")


if __name__ == "__main__":
    main()
