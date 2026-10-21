"""Create the two charts used in the Phase 1 Review 2 report."""

import json
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


HERE = Path(__file__).resolve().parent
FIGURES = HERE / "figures"
FIGURES.mkdir(exist_ok=True)
data = json.loads((HERE / "results_cross_language.json").read_text())["results"]
ns = [int(n) for n in data]
names = ["Vulp dp fn", "Vulp plain", "C++ plain", "Python plain"]
colors = ["#176c59", "#3479b1", "#d17b22", "#a54461"]

plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 10})
fig, ax = plt.subplots(figsize=(7.2, 4.2), layout="constrained")
for name, color in zip(names, colors):
    ys = [data[str(n)][name]["median_s"] for n in ns]
    ax.plot(ns, ys, "o-", linewidth=2.2, markersize=6, label=name, color=color)
ax.set_yscale("log")
ax.set_xticks(ns)
ax.set_xlabel("Fibonacci input n")
ax.set_ylabel("Median elapsed time, seconds (log scale)")
ax.grid(axis="y", which="both", alpha=0.22)
ax.legend(loc="upper left", frameon=False, ncol=2)
fig.savefig(FIGURES / "fib_runtime.png", dpi=230)
plt.close(fig)

fig, ax = plt.subplots(figsize=(7.2, 3.6), layout="constrained")
for name, color in zip(names[1:], colors[1:]):
    speedups = [data[str(n)][name]["median_s"] / data[str(n)]["Vulp dp fn"]["median_s"] for n in ns]
    ax.plot(ns, speedups, "o-", linewidth=2.2, markersize=6, label=name, color=color)
ax.set_yscale("log")
ax.set_xticks(ns)
ax.set_xlabel("Fibonacci input n")
ax.set_ylabel("Time ratio to Vulp dp fn (unitless; log scale)")
ax.grid(axis="y", which="both", alpha=0.22)
ax.legend(loc="upper left", frameon=False, ncol=3)
fig.savefig(FIGURES / "fib_speedup.png", dpi=230)
plt.close(fig)
