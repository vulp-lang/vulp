# Group 14 project progress checklist

**Project:** A domain-specific programming language and compiler for competitive programming
**Review:** BTP Phase 1, Review 2, October 2026
**Basis:** the initial proposal, the current compiler source and documentation, and benchmark results in `docs/report/bench/`.

## Working compiler

- [x] Define the Vulp syntax and core type rules.
- [x] Implement a custom lexer with source locations and diagnostics.
- [x] Implement a Pratt parser and abstract syntax tree.
- [x] Implement semantic analysis for names, types, calls, control flow, references, and `dp fn` purity.
- [x] Generate and verify LLVM IR.
- [x] Support native executables and an LLVM ORC JIT.
- [x] Add buffered token input, output, and `TC:` testcase blocks.
- [x] Implement arrays, vectors, ordered maps, structures, functions, and loops.
- [x] Support signed and unsigned integer storage widths up to 256 bits.
- [x] Support `int<lo..hi>` declarations for width selection.
- [ ] Enforce declared `int<lo..hi>` bounds at runtime or prove them statically. Current range declarations do not enforce the interval.
- [x] Add `dp fn` automatic memoization with purity checks and cache reset when testcase-dependent global reads occur.
- [x] Add unit, end-to-end, expected-error, example, and sanitizer checks. Recorded V1 campaign: 142 unit tests, 34 end-to-end programs, 20 error tests, 10 examples, 88 sanitizer runs.

## Original CPIR proposal

- [ ] Introduce a CP-specific semantic IR that retains query operations, DP dependencies, input constraints, and graph mutability before LLVM lowering. Current V1 lowers the checked AST to LLVM IR.
- [ ] Implement `range_q` selection among prefix arrays, Fenwick trees, segment trees, and sparse tables. Define supported operations and algebraic requirements first.
- [ ] Implement `dp_table` layout selection among dense, sparse, rolling-row, and bitset forms. Prove dependency conditions before applying a rolling-row transformation.
- [ ] Add built-in bitset support and benchmark it on appropriate workloads.
- [ ] Add a graph abstraction and select between adjacency vectors and CSR when the program's update pattern allows it. V1 can express adjacency vectors manually.
- [ ] Evaluate structure-field reordering against source-order layout and specify observable layout behavior.
- [ ] Add a modular arithmetic data type with explicit modulus semantics and test edge cases.
- [ ] Add basic string algorithms from the proposal: rolling hash, KMP, and Z-function. V1 has strings but not these built-ins.
- [ ] Decide whether first-class functions fit the final language design. V1 has named functions only.
- [ ] Add time and memory complexity annotations with a restricted proof system. Report `verified`, `unknown`, and `not established` clearly.
- [ ] Explore the optional small language model estimator only after defining evaluation data and a separate, clearly labelled estimate output. No AI mode is implemented now.
- [ ] Add an explanation mode that reports selected representations and reasons.

## Evaluation

- [x] Benchmark six contest-style task families against C++ and Python on the reference laptop, checking matching outputs and recording runtime, source tokens, build time, and memory. Raw results are in `docs/report/bench/results.json`.
- [x] Add a separate, reproducible `dp fn` comparison against unmemoized Vulp, C++, and Python Fibonacci programs. The benchmark script and raw runs are in `docs/report/bench/dp_fib/`.
- [x] Generate fresh charts for the Fibonacci comparison. The report does not use the old LCS memoization chart.
- [ ] Add C and Java implementations to the cross-language comparison. Neither appears in the current measured suite.
- [ ] Expand from six task families to the proposal's approximate 40 to 50 complete contest problems, with fixed inputs and output checks.
- [ ] Measure writing effort with a defined study protocol; source-token count alone does not measure programmer time.
- [ ] Run ablations for each future CP-specific selection pass against generic lowering and hand-selected C++ implementations.
- [x] Rerun the six-task and LCS width experiments on the reference laptop with seven interleaved rounds pinned to one CPU.
- [ ] Add longer-running or repeated-work measurements for the millisecond-scale `dp fn` case so startup time does not dominate its estimate.

## Suggested next milestone order

1. Specify and implement one range-query abstraction, then compare its compiler-selected representation with hand-written Fenwick and segment trees.
2. Add DP dependency analysis and rolling-row lowering; use LCS and other recurrences to measure memory and time.
3. Add modular arithmetic and graph APIs with correctness tests and contest examples.
4. Define restricted complexity contracts, followed by optional model estimates kept separate from proofs.
5. Expand the benchmark corpus and language baselines before making broad conciseness or speed claims.
