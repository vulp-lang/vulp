# Vulp

Vulp (vulp-lang) is a small statically typed language for **competitive programming**, with
an LLVM-based compiler, `vulpc`. Programs can be run straight away through the LLVM JIT or
compiled to native executables.

```text
dp fn fib(i32 n) -> i32 {        # `dp` = memoise this pure function
    if n <= 1:
        return n;
    else: {
        i32 a = fib(n - 1), b = fib(n - 2);
        return a + b;
    }
}

TC: {                            # read t, run the block t times
    read int n;
    read int[n] a;
    int best = 0;
    for x in a: best = max(best, x);
    println(fib(n), " ", best);
}
```

## Features (V1)

- Integers of any width `i1`…`i256` / `u1`…`u256`, rounded up to 8/16/32/64/128/256 bits,
  and range integers `int<lo..hi>` whose width is chosen from the range.
- `int` (64-bit), `char`, `bool`, `float`, `string`, fixed arrays `T[n]` / `T[n][m]`,
  `vec<T>`, ordered `map<K,V>`, `struct`s, range arrays `[1..10]` (inclusive).
- `if` / `elif` / `else`, `while`, C-style `for ( ; ; ):` and `for x in ...:`, `break`,
  `continue`.
- Functions with by-value or `&` reference parameters, recursion on a 1 GiB stack.
- `dp fn`: compiler-checked pure functions with automatic memoisation.
- `TC:` testcase blocks; `dp` caches that depend on input are reset per testcase.
- Fast buffered `read`/`print`/`println`, including `read int[n] a;`.
- Value semantics with deep copies; bounds and division checks (optional).
- `vulpc` can JIT-run a program, emit a native executable, an object file or LLVM IR, and
  dump tokens, the AST or the typed AST.

## Quick start

```bash
cmake -S . -B build -G Ninja      # needs CMake, a C++20 compiler and LLVM 18 (llvm-18-dev)
cmake --build build
./build/vulpc examples/01_hello.vp                         # JIT: compile in memory and run
./build/vulpc examples/03_testcases.vp -o tc && ./tc < examples/03_testcases.in   # native binary
(cd build && ctest --output-on-failure)                    # run all tests
```

## Documentation

- [docs/tutorial.md](docs/tutorial.md) — step-by-step guide: building, running, compiling, writing programs
- [docs/language.md](docs/language.md) — language reference
- [docs/architecture.md](docs/architecture.md) — how the compiler is put together
- [docs/error_codes.md](docs/error_codes.md) — exit codes
- [docs/PLAN.md](docs/PLAN.md) — the step-by-step implementation plan and its status
- [docs/report/REPORT.md](docs/report/REPORT.md) — project report: examples and benchmarks against C++ and Python
- [docs/presentation/vulp_v1.pptx](docs/presentation/vulp_v1.pptx) — slide deck (rebuilt by `docs/presentation/build_deck.py`)
- [examples/](examples) — runnable programs with sample input and output

## Layout

```text
include/, src/   compiler: lexer, parser, ast, sema, codegen, driver
runtime/         runtime library linked into JIT runs and executables
tests/           unit tests, end-to-end programs, compile-error programs, test runner
examples/        example programs (+ .in / .out)
docs/            documentation
```

Vulp was previously planned under the name CPIR; the CPIR concept documents in the parent
directory describe the longer-term research direction (range-query selection, DP table
layouts, complexity contracts), which is outside this V1.
