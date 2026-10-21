# Vulp V1 — implementation plan

Small, discrete steps. Every step lands with tests (unit tests in `tests/unit`,
end-to-end programs in `tests/e2e`, negative programs in `tests/errors`).

Legend: `[x]` done, `[ ]` pending.

## Phase 0 — environment & skeleton
- [x] 0.1 Toolchain check: cmake 3.28, ninja, g++ 13, system LLVM 18.1 (`/usr/lib/llvm-18`), ORC JIT + object emission sanity build
- [x] 0.2 Directory layout, CMake (LLVM pinned to system LLVM 18), unit-test framework, e2e runner, ctest wiring

## Phase 1 — lexer
- [x] 1.1 Tokens, source locations, diagnostics with caret snippets
- [x] 1.2 Operators & punctuation (compound assignment, shifts, `..`, `->`, `++`, `--`)
- [x] 1.3 Literals: decimal/hex ints with `_`, floats with exponent, chars, strings with escapes; `#` comments
- [x] 1.4 Identifiers & keywords, `iN`/`uN` integer-type tokens, `TC`

## Phase 2 — parser (Pratt expressions, vex style)
- [x] 2.1 AST + S-expression printer
- [x] 2.2 Expressions: literals, names, unary, binary precedence, grouping, assignment & compound, postfix call/index/field/method/`++`/`--`, array & range literals, casts
- [x] 2.3 Types: scalars, `iN`/`uN`, `int<lo..hi>`, `vec<T>`, `map<K,V>`, struct names, array suffixes `T[n][m]`, `>>` splitting, `&` refs
- [x] 2.4 Statements: declarations (multi), `read T x;`, read/print/println, if/elif/else, while, for-in, C-style for, `TC:`, return/break/continue, blocks
- [x] 2.5 Top level: `fn`, `dp fn`, `struct`
- [x] 2.6 Error recovery and messages

## Phase 3 — semantic analysis
- [x] 3.1 Type system: interning, power-of-two width rounding, range ints, mangling
- [x] 3.2 Scopes, globals vs locals, struct & function registration
- [x] 3.3 Expression typing: literal coercion, operators, assignment compatibility, calls/ctors/casts/builtins, index/field/methods
- [x] 3.4 Statement checks: bool conditions, loop context, returns, TC placement, read/print operands
- [x] 3.5 `dp fn` checks and purity analysis (transitive global reads/writes/I-O)

## Phase 4 — runtime library (C++, linked into JIT and AOT binaries)
- [x] 4.1 Buffered output; print ints (8..256 bit, signed/unsigned), floats, chars, strings
- [x] 4.2 Buffered input; read ints of every width, floats, chars, tokens; auto-flush before blocking reads
- [x] 4.3 Vec/string helpers (alloc, push, clone, concat, compare)
- [x] 4.4 Ordered map (int / unsigned / string keys)
- [x] 4.5 Memo table for `dp fn`
- [x] 4.6 Entry point on a big stack, runtime errors

## Phase 5 — LLVM code generation
- [x] 5.1 Module skeleton, `vulp_main`, JIT driver, hello world
- [x] 5.2 Integer arithmetic, comparisons, short-circuit logic, printing
- [x] 5.3 Local & global variables, assignment, compound assignment, `++`/`--`
- [x] 5.4 if/elif/else, while, C-style for, break/continue
- [x] 5.5 Functions, calls, recursion, returns
- [x] 5.6 AOT object emission + linking; e2e runner exercises JIT and AOT
- [x] 5.7 Wide & rounded integers (`i21`, `int<lo..hi>`, i128, i256), unsigned ops, casts
- [x] 5.8 char, float, string
- [x] 5.9 read statement and `read T x;` declarations
- [x] 5.10 Arrays (`T[n]`, `T[n][m]`), array/range literals, bounds checks, for-in
- [x] 5.11 vec methods, deep-copy value semantics
- [x] 5.12 Structs (decl, constructor, fields, copy, print/read)
- [x] 5.13 Maps (index, has, erase, len, ordered iteration, copy)
- [x] 5.14 `dp fn` memoization, `TC:` block, per-testcase cache reset
- [x] 5.15 `&` reference parameters
- [x] 5.16 Builtins: min, max, abs, exit, sort

## Phase 6 — docs & examples
- [x] 6.1 Language reference (`docs/language.md`)
- [x] 6.2 Tutorial (`docs/tutorial.md`) and examples (`examples/`)
- [x] 6.3 README

## Phase 7 — independent review loop
- [x] 7.1 Reviewer subagent pass, fix findings, repeat until satisfied

## Review log

### Round 1 — NOT SATISFIED → fixed
| # | Finding | Fix |
|---|---|---|
| 1 | round floats printed as `1e+02` | shortest round-trip digits (`std::to_chars`) laid out like Python's repr; 10x faster |
| 2 | `dp` cache clear cost O(table capacity) per testcase | memo entries carry a generation tag: O(1) clear (`t28_dp_reset_perf`, time-limited) |
| 3 | `MIN / -1`, `MIN % -1`, oversized shifts undefined | defined wrapping division; shifts by ≥ width shift everything out (`t19_int_semantics`) |
| 4 | out-of-range float→int nondeterministic | saturating conversions, NaN → 0 |
| 5 | `dp fn` could grow a global map by reading `g[k]` | map reads never insert; writes do (`t29_map_reads`) |
| 6 | memory never freed (1 GB for simple loops) | deterministic frees on overwrite / re-declaration / function exit / dropped temporaries; poison mode `VULP_DEBUG_FREE=1` in ctest (`t17_ownership`, `t30_memory` capped at 64 MB) |
| 7 | compiler segfault on 50k-term expressions | compiler runs on a 1 GiB stack; long diagnostic lines clipped |
| 8 | silent integer input overflow | range-checked reads (`t27_rt_input_overflow`) |
| 9 | range-int arithmetic wraps at storage width | integers narrower than 64 bits promote to `int` in arithmetic |
| 10 | `string(x)` narrower than printing | `string()` of every scalar |

### Round 2 — NOT SATISFIED (10/10 round-1 fixes confirmed) → fixed
| # | Finding | Fix |
|---|---|---|
| 1 | `&` argument into a container could dangle when the callee resized / replaced it (via another `&` or a global) | compile-time rules (container + its element in one call; element of a global the callee writes; forwarded hazards), `&` addresses taken after argument evaluation (`e18_ref_dangling`, `t31`) |
| 2 | runtime float → `i256`/`u256` cast aborted LLVM | hand-written saturation for widths above 128 bits (`t19`) |
| 3 | `min`/`max` ignored the promotion rule | mixed argument types follow the arithmetic rules |
| 4 | dp fn could insert into a global map via `rd(G[n])` | passing a map element by `&` counts as a write (`e19_dp_map_ref`) |
| 5 | temporaries like `f()[0]`, `mk().v.len()` leaked per iteration | element-of-temporary evaluation frees the temporary (`t32_temporary_memory`, 32 MB cap) |
| 6 | 2M nested parentheses crashed the compiler | parser nesting limit of 100000 levels with a clear error |
| 7 | `u32 * u32` wraps through `int` | documented, with the `u64(z) * z` idiom |

Verification beyond the test-suite: every e2e test and example was also run under an
AddressSanitizer build of the generated code and the runtime, at -O0 and -O2 (78 runs, no
reports).

### Round 3 — NOT SATISFIED (7/7 round-2 fixes confirmed) → fixed
| # | Finding | Fix |
|---|---|---|
| 1 | the `&` rules could be bypassed through aliasing (`f(v, v)` or `f(G)` and then container + element inside the callee) | two `&` parameters, or a parameter and a global, of related types are treated as possibly the same object (`e20_ref_param_aliasing`, `t33_refs_allowed`) |
| 2 | the rules reject some safe code | documented as conservative |
| 3 | `max(q, -1)` with `u64 q` rejected while `q < -1` compiles | min/max convert non-fitting literals like comparisons do |

### Round 4 — NOT SATISFIED (aliasing bypass confirmed closed) → fixed
| # | Finding | Fix |
|---|---|---|
| 1 | compiler crashed on recursive structs (`struct Node { vec<Node> kids; ... }`) in the new aliasing check | `type_reaches` tracks visited structs (`e20_ref_param_aliasing`, unit tests) |
| 2 | chains of more than 100000 operators are now rejected | documented as part of the nesting limit |
| + | (found while waiting) `a[0][f()]` took the address of `a[0]` before running `f()` | every place evaluates all index / key values first, then computes its address (`t34_index_evaluation_order`) |

### Round 5 — NOT SATISFIED (recursion fix confirmed, ~60 evaluation-order cases safe) → fixed
| # | Finding | Fix |
|---|---|---|
| 1 | `v[g()] += s`: `g()` could overwrite the borrowed right side before the append | a borrowed right side of `+=` (and a borrowed map key of `has`/`erase`) is copied when the target / receiver has side effects (`t35_borrowed_operands`) |
| 2 | evaluation order undocumented | "Evaluation order" section in `docs/language.md` |

### Round 6 — SATISFIED
No blocking findings (300 generated evaluation-order programs, 110 of them under ASan, plus
the full regression sweep). Remaining items are documented, conservative design limits: the
`&` rules reject some safe calls, method-call argument order differs from C++17, `u32 * u32`
wraps through `int`, `int<lo..hi>` ranges are not checked at run time, and nesting is capped
at 100000 levels.

After the review I found and fixed one more case myself: `m.erase(m[k].name)` read its key
after freeing the value that contained it; `erase` now always copies a borrowed key
(`t35_borrowed_operands`).

### Round 7 — SATISFIED (confirmation of the post-review `erase` fix on the final sources)
