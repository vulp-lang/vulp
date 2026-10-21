# Vulp compiler architecture (V1)

```text
 prog.vp
   │  src/lexer      Lexer::tokenize           -> std::vector<Token>
   ▼
 tokens
   │  src/parser     Parser (Pratt expressions) -> AST::Program        (include/ast/Ast.hpp)
   ▼
 AST
   │  src/sema       SemanticAnalyzer           -> typed AST + Sema::ProgramInfo
   ▼                 (types, scopes, literal coercion, dp purity, return checks)
 typed AST
   │  src/codegen    CodeGen                    -> llvm::Module
   ▼
 LLVM IR ──► LLVM -O2 pipeline (src/driver) ──┬─► ORC LLJIT: run in memory      (vulpc prog.vp)
                                              └─► object file ─► c++ linker      (vulpc prog.vp -o prog)
                                                   + runtime/libvulprt.a
```

The structure follows the vex compiler (`../vexc`): a hand-written lexer, a Pratt parser
over `std::variant` AST nodes visited with `std::visit` + `overloaded`, a semantic pass that
annotates the AST, and an LLVM back end. Unlike vex, Vulp has no custom SSA IR in V1: the
typed AST is lowered straight to LLVM IR.

## Source layout

| Path | Contents |
|---|---|
| `include/misc`, `src/misc` | `Diagnostics` (file:line:col messages with a caret), exit codes |
| `include/lexer`, `src/lexer` | tokens, keywords, `iN`/`uN` type names, literals |
| `include/ast`, `src/ast` | AST node types, S-expression printer (`--ast`, `--typed-ast`) |
| `include/parser`, `src/parser` | Pratt expression parser, statements, per-statement error recovery |
| `include/sema`, `src/sema` | interned types, width rounding, scopes, type rules, purity analysis |
| `include/codegen`, `src/codegen` | LLVM lowering: `CodeGen.cpp` (types, functions, dp wrappers), `CodeGenExpr.cpp`, `CodeGenStmt.cpp`, `CodeGenHelpers.cpp` (conversions, deep copies, printing, reading) |
| `include/driver`, `src/driver` | pipeline, optimisation, JIT, object emission, linking |
| `runtime/` | C-ABI runtime library (I/O, sequences, maps, memo tables, entry point) |
| `tests/unit` | unit tests for every stage (dependency-free framework) |
| `tests/e2e`, `tests/errors` | end-to-end programs and compile-error programs |
| `examples/` | example programs (also run by the test-suite) |

## Front end

- **Lexer**: 1-based line/column tracking, keeps going after errors. `dp` is lexed as an
  identifier and only treated as a keyword by the parser in front of `fn`.
- **Parser**: binding powers are Python-like (bitwise above comparisons). Statements are
  parsed with per-statement recovery (skip to `;`, `}` or a statement keyword), so several
  syntax errors are reported at once. `struct` names are pre-scanned so `Point p;` parses
  as a declaration.
- **Semantic analysis**:
  - Types are interned (`Sema::TypeContext`), so type equality is pointer equality.
  - `iN`/`uN` round up to `{8,16,32,64,128,256}`; `int<lo..hi>` uses the smallest signed
    width holding both bounds (`TypeContext::range_bits`, via `APInt::getSignificantBits`).
  - Integer literals are typed `i64` (or wider when needed) and are re-typed to the other
    operand's type when they fit; constants that do not fit their target are errors.
  - Arithmetic promotes chars and integers narrower than 64 bits to `int` (`arith_type`);
    narrow widths are storage widths.
  - Every variable gets a `VarInfo` (global / local / parameter / reference); every call is
    resolved to a function, struct constructor or builtin.
  - Purity: each function records whether it reads globals, writes globals (assignment,
    `++`, `read`, mutating methods rooted at a global) or does I/O, plus its callees and
    the variables it passes to `&` parameters. A fixpoint propagates these facts; `dp fn`s
    that write globals or do I/O are rejected, and those that read globals are flagged
    `reset_cache_per_testcase`.
  - A conservative "always returns" analysis rejects non-void functions that can fall off
    their end.

## Code generation

| Vulp type | LLVM type |
|---|---|
| `iN`/`uN`/`int<..>` | `i8` … `i256` |
| `char` / `bool` / `float` | `i8` / `i1` / `double` |
| `string`, `vec<T>`, `T[]` | `%vulp.vec = { ptr data, i64 len, i64 cap }` |
| `map<K,V>` | `ptr` to a runtime map (null = empty) |
| `struct S` | `%S.S = { fields }` |

All-zero bytes are a valid value of every type, so zero-initialised globals, `calloc`ed
arrays and fresh map entries need no constructors.

- **Program entry**: top-level statements become `i32 vulp_main()`; top-level declarations
  are internal LLVM globals. Executables get a C `main` that calls
  `vulp_rt_start(vulp_main)`, which runs the program on a 1 GiB-stack thread and flushes
  output.
- **Value semantics and ownership**: every heap buffer has exactly one owner (a variable,
  element, field, map value or temporary).
  - An expression that names existing storage (variable, element, field, assignment result,
    `back()`) yields a *borrowed* value. Whenever a borrowed value of an owning type is
    stored somewhere (initialisation, assignment, argument, return, push, map store,
    constructor argument, `for` variable), codegen calls a generated `fix.<type>` helper that
    replaces every heap buffer reachable from the copy with a fresh copy. Temporaries (call
    results, literals, concatenations) are moved without copying.
  - Generated `free.<type>` helpers release everything a value owns. They run when a value
    is overwritten (after the new value has been computed), when a declaration runs again,
    for owned temporaries after use (print / compare / concat operands, discarded call
    results), and in each function's single exit block for its owned locals and by-value
    parameters (returns store into a slot and branch there). Owning locals are zeroed at
    function entry so the first free is a no-op.
  - Two hazards are handled explicitly: a borrowed left operand of a string comparison or
    concatenation is copied first if the right operand has side effects; and
    `for x in a[i]` freezes the index/key values once and recomputes the element address
    every iteration, so it never holds a pointer into a buffer the body may reallocate.
  - `VULP_DEBUG_FREE=1` makes the runtime poison freed blocks instead of releasing them;
    the test-suite runs every program that way too, so a use-after-free shows up as
    wrong output.
- **References**: `&` arguments are frozen first (their index / key values evaluated in order),
  then all by-value arguments are evaluated, and only then are the `&` addresses computed, so
  evaluating an argument cannot invalidate an address. Semantic analysis rejects the remaining
  hazards: an element passed together with a reference to a container that contains it (also
  across two different roots that may be the same object at runtime: two `&` parameters, or a
  `&` parameter and a global, of struct-field-related types), an element of a global passed
  to a function whose (transitive) set of written globals contains it, and a global passed to
  a parameter whose `ref_hazards` set contains it (the callee hands
  an element of that parameter to code that writes the global). Written-global sets and
  hazard sets are computed to a fixpoint over the call graph together with the purity facts.
- **Temporaries**: an index / field chain rooted at a temporary (`f()[i].x`) is evaluated by
  `with_temp_place`: the element is copied out (if it owns memory) and the temporary freed.
- **Maps**: a write access (`m[k] = ..`, `m[k] += ..`, `m[k].push(..)`, ...) uses
  `vulp_map_index_*` (insert-or-get); a read access uses `vulp_map_get_*` and falls back to
  a zeroed stack slot, so reading never inserts.
- **Integer corner cases**: signed `MIN / -1` is computed as `MIN / 1` (wrapping), shifts by
  `>=` the width select `0` / sign-fill instead of producing LLVM poison, and float to
  integer casts use `llvm.fptosi.sat` / `llvm.fptoui.sat`.
- **References**: a `T&` parameter is passed as a pointer to the caller's storage.
- **`dp fn f`** lowers to three pieces: `@dp.memo.f` (a global pointer to a runtime hash
  table), `@dp.impl.f` (the body), and `@fn.f`, the wrapper every caller (including the
  body itself) uses: it packs the arguments into a zero-padded byte key, calls
  `vulp_memo_get`, and on a miss calls the implementation and `vulp_memo_put`.
- **`TC:`** reads the count with `vulp_read_i64`, loops, and at the top of each iteration
  calls `vulp_memo_clear` for every `dp fn` flagged `reset_cache_per_testcase`.
- **for-in over ranges** counts in a hidden counter and stops after comparing with `hi`, so
  ranges ending at the type's maximum terminate; no array is materialised.
- **Runtime checks**: indexes are compared (unsigned) against the length, integer divisors
  against zero; failures call `noreturn` runtime functions. `--no-checks` omits them.
- Per-type helpers `fix.*`, `print.*` and `read.*` are generated on demand and cached.
- The module is verified with `llvm::verifyModule`; a failure is reported as an internal
  error (exit code 70).

## Runtime library (`runtime/vulp_runtime.{h,cpp}`)

Plain C ABI, written in C++. Linked into `vulpc` itself (exported with `-rdynamic` so the
ORC JIT resolves the symbols through `DynamicLibrarySearchGenerator`) and built as
`libvulprt.a` for executables.

- Output: 64 KiB buffer, written with `write(2)`; integers of every width (128/256-bit
  values are passed by pointer), shortest round-trip floats.
- Input: 64 KiB buffer read with `read(2)`; output is flushed before every refill so
  interactive problems work.
- Sequences: allocation, amortised push, clone, string concat/append/compare, `sort` by
  element kind.
- Maps: `std::map` keyed by `int64_t`, `uint64_t` or `std::string`, values in zeroed blocks.
- Memo tables: open addressing with linear probing over fixed-size byte keys; entries carry
  a generation tag, so clearing a table (once per `TC:` iteration) is O(1).
- Floats are formatted with `std::to_chars` (shortest round-trip digits), then laid out in
  plain or scientific notation like Python's `repr`.
- Integer input is parsed into 256 bits and range-checked against the target type.

## Testing

- `vulp_unit_tests` (tests/unit): lexer, parser, types, sema, runtime and codegen
  (IR-structure checks and in-process JIT runs with captured I/O).
- `tests/run_tests.py`: runs `tests/e2e` through the JIT and as executables, checks that
  `tests/errors` programs are rejected with the expected messages, and runs `examples/`.
- `ctest` in the build directory runs all of the above.

## Compiler process

`vulpc` runs the whole pipeline on a thread with a 1 GiB stack, because the parser,
semantic analysis, code generation and AST destruction recurse over the syntax tree. The
parser caps nesting at 100000 levels (blocks, parentheses, operator chains), which keeps that
recursion well within the stack. The unit-test runner uses the same large stack.

## Toolchain

Built against the system LLVM 18 (`/usr/lib/llvm-18`, Ubuntu `llvm-18-dev`), linked to the
shared `libLLVM`. The CMake option `VULP_LLVM_DIR` points elsewhere if needed.
