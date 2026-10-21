#!/usr/bin/env python3
"""Render the report's SVG figures and Markdown tables from results.json / ablation.json.

No plotting library is needed: the SVG is written directly.
Colors: the first three slots of a validated categorical palette, by language
(Vulp = blue, C++ = orange, Python = aqua), the same in every figure.
"""
import json
import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))
FIG = os.path.join(HERE, "..", "figures")

COLOR = {"vulp": "#2a78d6", "cpp": "#eb6834", "py": "#1baf7a"}
LANG_NAME = {"vulp": "Vulp", "cpp": "C++", "py": "Python"}
SURFACE, INK, INK2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"
FONT = "font-family='-apple-system, Segoe UI, Helvetica, Arial, sans-serif'"
PRETTY = {"sieve": "Sieve (n = 2·10⁷)", "sort_io": "Sort + I/O (10⁶ ints)",
          "lcs": "LCS (3000 × 3000)", "dijkstra": "Dijkstra (10⁵ V, 5·10⁵ E)",
          "testcases": "2·10⁵ testcases", "map_count": "Map count (10⁶ ops)"}


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def text(x, y, s, size=12, color=INK2, anchor="start", weight="normal"):
    return (f"<text x='{x:.1f}' y='{y:.1f}' font-size='{size}' fill='{color}' text-anchor='{anchor}' "
            f"font-weight='{weight}' {FONT}>{esc(s)}</text>")


def bar(x, y, w, h, color):
    """Horizontal bar anchored at x, rounded only at the data end (4px)."""
    r = min(4, h / 2, w)
    if w <= 0:
        return ""
    return (f"<path d='M{x:.1f},{y:.1f} h{w - r:.1f} a{r},{r} 0 0 1 {r},{r} v{h - 2 * r:.1f} "
            f"a{r},{r} 0 0 1 {-r},{r} h{-(w - r):.1f} z' fill='{color}'/>")


def bar_between(x1, x2, y, h, color, data_end_right):
    """Bar from x1 to x2 with the 4px rounding on its data end only (the end away from 1x)."""
    w = max(x2 - x1, 2)
    r = min(4, h / 2, w)
    if data_end_right:
        return bar(x1, y, w, h, color)
    return (f"<path d='M{x1 + w:.1f},{y:.1f} h{-(w - r):.1f} a{r},{r} 0 0 0 {-r},{r} v{h - 2 * r:.1f} "
            f"a{r},{r} 0 0 0 {r},{r} h{w - r:.1f} z' fill='{color}'/>")


def legend(x, y, langs):
    out = []
    for lang in langs:
        out.append(f"<rect x='{x}' y='{y - 9}' width='10' height='10' rx='2' fill='{COLOR[lang]}'/>")
        out.append(text(x + 15, y, LANG_NAME[lang], 12, INK))
        x += 22 + 8 * len(LANG_NAME[lang]) + 14
    return out


def svg(width, height, body, title, desc):
    return (f"<svg xmlns='http://www.w3.org/2000/svg' width='{width}' height='{height}' "
            f"viewBox='0 0 {width} {height}' role='img' aria-labelledby='t d'>"
            f"<title id='t'>{esc(title)}</title><desc id='d'>{esc(desc)}</desc>"
            f"<rect width='{width}' height='{height}' rx='8' fill='{SURFACE}'/>" + "".join(body) + "</svg>\n")


def by_label(prob, label):
    return next(i for i in prob["impls"] if i["label"] == label)


def main_impls(name, prob):
    """The three headline implementations of a problem (for LCS: the table versions)."""
    if name == "lcs":
        return [by_label(prob, "Vulp (table)"), by_label(prob, "C++ (table)"), by_label(prob, "Python")]
    return [next(i for i in prob["impls"] if i["lang"] == lang) for lang in ("vulp", "cpp", "py")]


def fig_runtime(res):
    """Runtime relative to C++ on a log axis (C++ = 1x)."""
    names = list(res["problems"])
    left, right, top, row = 190, 40, 70, 46
    width = 760
    plot_w = width - left - right
    lo, hi = 0.5, 32.0
    height = top + row * len(names) + 50

    def X(v):
        return left + plot_w * (math.log2(v) - math.log2(lo)) / (math.log2(hi) - math.log2(lo))

    body = [text(20, 28, "Runtime relative to C++ (g++ -O2)", 16, INK, weight="600"),
            text(20, 48, "Median of 7 runs, log scale. Bars grow from C++ = 1×: right = slower, left = faster.", 12)]
    body += legend(width - 200, 28, ["vulp", "py"])
    for t in (0.5, 1, 2, 4, 8, 16, 32):
        x = X(t)
        body.append(f"<line x1='{x:.1f}' y1='{top - 6}' x2='{x:.1f}' y2='{top + row * len(names)}' "
                    f"stroke='{INK2 if t == 1 else GRID}' stroke-width='1'/>")
        body.append(text(x, top + row * len(names) + 18, f"{t:g}×", 11, INK2, "middle"))
    for k, name in enumerate(names):
        vp, cpp, py = main_impls(name, res["problems"][name])
        y = top + k * row
        body.append(text(left - 12, y + 22, PRETTY[name], 12, INK, "end"))
        for j, impl in enumerate((vp, py)):
            ratio = impl["time_s"] / cpp["time_s"]
            by = y + 6 + j * 16
            x1, x2 = sorted((X(1), X(ratio)))
            body.append(bar_between(x1, x2, by, 14, COLOR[impl["lang"]], ratio >= 1))
            label_x, anchor = (x2 + 6, "start") if ratio >= 1 else (x1 - 6, "end")
            body.append(text(label_x, by + 11, f"{ratio:.2f}×", 11, INK, anchor))
    body.append(text(left, height - 10, "LCS uses the bottom-up table versions; Vulp's dp fn variant is in the LCS figure.", 11))
    return svg(width, height, body, "Runtime relative to C++",
               "Horizontal bars per problem for Vulp and Python, relative to C++ on a log scale.")


def fig_code_size(res):
    names = list(res["problems"])
    left, right, top, row = 190, 60, 70, 58
    width = 760
    plot_w = width - left - right
    vmax = max(i["tokens"] for n in names for i in main_impls(n, res["problems"][n]))
    height = top + row * len(names) + 40
    body = [text(20, 28, "Source size in tokens (smaller is shorter)", 16, INK, weight="600"),
            text(left, height - 14, "Vulp's Dijkstra includes a hand-written binary heap: V1 has no priority queue.", 11),
            text(20, 48, "Comments and blank lines excluded.", 12)]
    body += legend(width - 290, 28, ["vulp", "cpp", "py"])
    for k, name in enumerate(names):
        y = top + k * row
        body.append(text(left - 12, y + 26, PRETTY[name], 12, INK, "end"))
        for j, impl in enumerate(main_impls(name, res["problems"][name])):
            by = y + 4 + j * 15
            w = plot_w * impl["tokens"] / vmax
            body.append(bar(left, by, w, 13, COLOR[impl["lang"]]))
            body.append(text(left + w + 6, by + 10.5, str(impl["tokens"]), 10.5, INK2))
    body.append(f"<line x1='{left}' y1='{top - 4}' x2='{left}' y2='{top + row * len(names) - 8}' stroke='{INK2}'/>")
    return svg(width, height, body, "Source size in tokens",
               "Grouped horizontal bars of token counts per problem for Vulp, C++ and Python.")


def fig_lcs(res, abl):
    prob = res["problems"]["lcs"]
    rows = [(i["label"], i["lang"], i["time_s"], i["rss_mib"]) for i in prob["impls"]]
    for w in abl["widths"]:
        if w["variant"] in ("i32 table", "i32 table + --no-checks"):
            rows.append(("Vulp (" + w["variant"] + ")", "vulp", w["time_s"], w["rss_mib"]))
    rows.sort(key=lambda r: r[2])
    left, right, top, row = 230, 150, 66, 30
    width = 760
    plot_w = width - left - right
    vmax = max(r[2] for r in rows)
    height = top + row * len(rows) + 30
    body = [text(20, 28, "LCS of two 3000-character strings: time and peak memory", 16, INK, weight="600"),
            text(20, 48, "Same answer from every program. Median of 7 runs.", 12)]
    body += legend(width - 290, 48, ["vulp", "cpp", "py"])
    for k, (label, lang, t, m) in enumerate(rows):
        label = {"Vulp (table)": "Vulp (int table)"}.get(label, label)
        y = top + k * row
        body.append(text(left - 12, y + 15, label, 12, INK, "end"))
        w = plot_w * t / vmax
        body.append(bar(left, y + 4, max(w, 2), 16, COLOR[lang]))
        body.append(text(left + max(w, 2) + 6, y + 16, f"{t:.3f} s · {m:.0f} MiB", 11, INK))
    return svg(width, height, body, "LCS variants",
               "Horizontal bars of runtime for each LCS implementation, labelled with time and memory.")


def fig_build(res):
    """Build (compile+link) time per problem: vulpc vs g++."""
    names = list(res["problems"])
    left, right, top, row = 190, 60, 70, 40
    width = 760
    plot_w = width - left - right
    rows = []
    for n in names:
        p = res["problems"][n]
        vp = next(i for i in p["impls"] if i["lang"] == "vulp")
        cpp = next(i for i in p["impls"] if i["lang"] == "cpp")
        rows.append((n, vp["compile_s"], cpp["compile_s"]))
    vmax = max(max(r[1], r[2]) for r in rows)
    height = top + row * len(rows) + 30
    body = [text(20, 28, "Time to build an executable (compile + link)", 16, INK, weight="600"),
            text(20, 48, "vulpc -O2 -o  vs  g++ -O2 -std=c++20 (with <bits/stdc++.h>). Median of 3.", 12)]
    body += legend(width - 200, 28, ["vulp", "cpp"])
    for k, (n, v, c) in enumerate(rows):
        y = top + k * row
        body.append(text(left - 12, y + 18, PRETTY[n], 12, INK, "end"))
        for j, (val, lang) in enumerate(((v, "vulp"), (c, "cpp"))):
            by = y + 4 + j * 15
            w = plot_w * val / vmax
            body.append(bar(left, by, w, 13, COLOR[lang]))
            body.append(text(left + w + 6, by + 10.5, f"{val:.2f} s", 10.5, INK2))
    return svg(width, height, body, "Build time", "Grouped bars of build time per problem for vulpc and g++.")


def tables(res, abl):
    out = ["<!-- generated by bench/make_figures.py from bench/results.json and bench/ablation.json -->", ""]
    out.append("| Problem | Implementation | Time (s) | Min–max (s) | vs C++ | Peak RSS (MiB) | Lines | Tokens | Build (s) | JIT run (s) |")
    out.append("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    for name, p in res["problems"].items():
        cpp = next(i for i in p["impls"] if i["lang"] == "cpp")
        for i in p["impls"]:
            out.append(f"| {PRETTY[name]} | {i['label']} | {i['time_s']:.3f} | {i['time_min_s']:.3f}–{i['time_max_s']:.3f} | "
                       f"{i['time_s'] / cpp['time_s']:.2f}× | {i['rss_mib']:.1f} | {i['lines']} | {i['tokens']} | "
                       + (f"{i['compile_s']:.2f}" if i["compile_s"] is not None else "–") + " | "
                       + (f"{i['jit_total_s']:.3f}" if "jit_total_s" in i else "–") + " |")
    out += ["", "| Program | With checks (s) | `--no-checks` (s) | Check overhead |", "|---|---:|---:|---:|"]
    for r in abl["checks"]:
        out.append(f"| {PRETTY[r['problem']]} — {r['label']} | {r['checks_s']:.3f} | {r['no_checks_s']:.3f} | "
                   f"{(r['checks_s'] / r['no_checks_s'] - 1) * 100:+.0f}% |")
    out += ["", "| LCS table variant | Time (s) | Peak RSS (MiB) |", "|---|---:|---:|"]
    for w in abl["widths"]:
        out.append(f"| {w['variant']} | {w['time_s']:.3f} | {w['rss_mib']:.1f} |")
    return "\n".join(out) + "\n"


def main():
    res = json.load(open(os.path.join(HERE, "results.json")))
    abl = json.load(open(os.path.join(HERE, "ablation.json")))
    os.makedirs(FIG, exist_ok=True)
    for name, content in (("runtime_vs_cpp.svg", fig_runtime(res)), ("code_size.svg", fig_code_size(res)),
                          ("lcs_variants.svg", fig_lcs(res, abl)), ("build_time.svg", fig_build(res))):
        with open(os.path.join(FIG, name), "w") as f:
            f.write(content)
        print("wrote", name)
    with open(os.path.join(HERE, "results_tables.md"), "w") as f:
        f.write(tables(res, abl))
    print("wrote results_tables.md")


if __name__ == "__main__":
    main()
