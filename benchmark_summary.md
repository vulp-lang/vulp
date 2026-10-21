# Vulp benchmark summary

**Group 14 | Phase 1, Review 2**
Vulp is our LLVM-based language and compiler for competitive programming. The latest measurements compare native Vulp programs with C++ and Python on the same inputs.

## Six-task results

Median elapsed time in seconds. Lower is better.

| Task | Vulp | C++ | Python | Vulp / C++ |
|---|---:|---:|---:|---:|
| Sieve, primes through 20 million | 0.041 | 0.035 | 0.532 | 1.17× |
| Sort and I/O, 1 million integers | 0.100 | 0.111 | 0.456 | 0.90× |
| LCS, two 3000-character strings | 0.033 | 0.024 | 0.645 | 1.37× |
| Dijkstra, 100,000 vertices | 0.083 | 0.086 | 0.628 | 0.96× |
| 200,000 testcases | 0.055 | 0.055 | 0.305 | 0.99× |
| Map counting, 1 million operations | 0.275 | 0.194 | 0.275 | 1.42× |

The LCS comparison uses **32-bit table elements in both Vulp and C++**, with Vulp's normal runtime checks enabled. Peak memory is **38.1 MiB for Vulp** and **37.5 MiB for C++**. Vulp uses fewer source tokens than C++ in five of the six tasks. For sorting, the counts are 22 versus 118 tokens; Dijkstra is the exception because the Vulp program includes a handwritten heap.

## `dp fn` result

For `fib(40)`, Vulp's automatically memoized `dp fn` takes **0.00157 s**, compared with **0.198 s** for plain Vulp recursion, **0.107 s** for plain C++ recursion, and **8.26 s** for plain Python recursion. These are identical recurrences without manual caching in the plain versions. The tiny `dp fn` time is close to process-startup overhead. On dense LCS, a checked Vulp `i32` table is faster and smaller than generic `dp fn` memoization: **0.033 s and 38.1 MiB** versus **1.581 s and 675.7 MiB**.

## Measurement method

- Reference laptop: AMD Ryzen 9 8940HX, Linux; Vulp with LLVM 18.1.3, C++ with GCC 13.3 (`-O2`), and CPython 3.12.3.
- Six-task figures are medians of seven interleaved runs pinned to CPU 7 with `taskset`. Every language received the same generated input, and complete output hashes were checked before timing.
- A Python harness uses `time.perf_counter()` for wall time, including process startup. GNU `/usr/bin/time` reports peak resident memory through `%M`, converted from KiB to MiB. Fibonacci uses three fresh-process rounds.

## Files to share

- [Full Phase 1 Review 2 report with charts and source examples](docs/report/phase1_review2/report.pdf)
- [LaTeX source and supporting figures](docs/report/phase1_review2/)
- [Six-task runtime chart](docs/report/phase1_review2/figures/laptop_runtime.png)
- [Raw six-task results](docs/report/bench/laptop_results.json)
- [Fibonacci results and sources](docs/report/bench/dp_fib/)

This is a six-task evaluation. C and Java implementations, a larger contest suite, and a study of writing time remain future work.
