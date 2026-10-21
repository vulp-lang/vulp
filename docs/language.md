# Vulp language reference (V1)

Vulp is a small, statically typed, procedural language for competitive programming. It
compiles to native code through LLVM, either in memory (JIT) or to an executable.
This document describes exactly what the V1 compiler accepts. For a gentler
introduction see [tutorial.md](tutorial.md).

- [Lexical structure](#lexical-structure)
- [Program structure](#program-structure)
- [Types](#types)
- [Variables and declarations](#variables-and-declarations)
- [Expressions and operators](#expressions-and-operators)
- [Statements and control flow](#statements-and-control-flow)
- [Functions](#functions)
- [`dp` functions](#dp-functions)
- [The `TC:` testcase block](#the-tc-testcase-block)
- [Input and output](#input-and-output)
- [Containers](#containers-arrays-vec-string-map)
- [Structs](#structs)
- [Value semantics and references](#value-semantics-and--references)
- [Builtins](#builtins)
- [Runtime behaviour](#runtime-behaviour)
- [Not in V1](#not-in-v1)

## Lexical structure

- Source files use the extension `.vp`. They are read as bytes (ASCII/UTF-8).
- `#` starts a comment that runs to the end of the line.
- Statements end with `;`. Blocks are `{ ... }`.
- Identifiers: letters, digits and `_`, not starting with a digit. Names are case sensitive.
- Keywords: `fn struct if elif else while for in return break continue TC read print println true false`
  and the type names `int char bool float string vec map`. `dp` is special only directly
  in front of `fn`, so `dp` is still a normal variable name (`int[n] dp;` works).
- `iN` / `uN` with `1 <= N <= 256` (`i8`, `i21`, `u64`, `i256`, ...) are type names.

### Literals

| Literal | Examples | Notes |
|---|---|---|
| integer | `42`, `1_000_000`, `0xFF` | `_` separators allowed. Type `int` (64-bit) unless the value needs more bits (then `i128` / `i256`). |
| float | `3.14`, `2.5e-3`, `1e9` | Always `float`. Use `1_000_000_000` (or `int(1e9)`) for an integer. |
| char | `'a'`, `'\n'`, `'\''` | One byte. |
| string | `"hi\tthere\n"` | Escapes: `\n \t \r \0 \\ \" \'` |
| bool | `true`, `false` | |
| array | `[1, 2, 3]`, `[]` | `[]` needs a known type, e.g. `vec<int> v = [];` |
| range array | `[1..10]`, `['a'..'z']` | **Inclusive** on both ends; empty when `lo > hi`. |

## Program structure

A program is a sequence of top-level items:

- statements, which run from top to bottom (they form the program's `main`);
- `fn` / `dp fn` function definitions;
- `struct` definitions.

Functions and structs can only be defined at the top level. They can be used before
their definition (mutual recursion works). Variables declared at the top level
(outside any block) are **globals**; a function can use the globals declared **above** it.

```text
int MOD = 1_000_000_007;        # global

fn add(int a, int b) -> int { return (a + b) % MOD; }

read int n;                     # top-level statements run in order
println(add(n, n));
```

## Types

| Type | Meaning | Stored as |
|---|---|---|
| `int` | 64-bit signed integer | `i64` |
| `iN`, `uN` (1 ≤ N ≤ 256) | signed / unsigned integer of at least N bits | next of 8, 16, 32, 64, 128, 256 bits |
| `int<lo..hi>` | integer whose values lie in `[lo, hi]` (constants) | smallest **signed** 8/16/32/64/128/256-bit type that holds both bounds |
| `char` | a byte, printed as a character | 8-bit unsigned |
| `bool` | `true` / `false` | 1 bit |
| `float` | IEEE double | 64-bit |
| `string` | mutable byte string | heap buffer |
| `T[]` | fixed-length array (length chosen at runtime) | heap buffer |
| `vec<T>` | growable array | heap buffer |
| `map<K, V>` | ordered map | balanced tree |
| `Name` | a `struct` | fields in order |

### Integer width rounding

The width you ask for is rounded **up** to the next power of two in `{8, 16, 32, 64, 128, 256}`:

```text
i1, i7      -> i8            u3   -> u8
i21         -> i32           i100 -> i128
i200        -> i256          i64  -> i64
int<0..100>           -> i8   (needs 8 signed bits)
int<0..255>           -> i16  (255 needs 9 signed bits)
int<-5..300>          -> i16
int<0..1000000007>    -> i32
int<0..100000000000000000000> -> i128   (10^20 needs 68 bits)
```

The rounded width is the **storage** width (memory per variable / array element). In
arithmetic, integers narrower than 64 bits are first promoted to `int`, so
`int<0..100000> x; int y = x * x;` is exact; a result wraps only when it is stored back
into a narrow variable (`i8 b = 100; b = b * 3;` leaves `b == 44`). Values are **not**
checked against a declared range at runtime in V1. A constant that does not fit the
storage width is a compile error (`int<0..100> p = 1000;`), and so is reading an input
integer that does not fit it.

## Variables and declarations

```text
int x = 5, y, z = x * 2;       # several names, optional initialisers
read int n, m;                 # declare and read from input (see Input)
int[n] a;                      # array of n zeroes
int[n][m] grid;                # n rows of m zeroes
vec<int>[n + 1] adj;           # array of n+1 empty vecs (adjacency list)
int[] b = [3, 1, 4];           # length from the initialiser
string s;                      # ""
map<string, int> freq;         # empty map
```

- A variable without an initialiser starts at zero / false / empty. This also happens each
  time a declaration inside a loop runs.
- Array sizes are arbitrary integer expressions evaluated at runtime. Sized dimensions
  must come first (`int[n][]` is fine, `int[][n]` is not). A declaration cannot have both a
  size and an initialiser.
- Redeclaring a name in the same scope is an error; inner blocks may shadow outer names.
- Every block `{ }`, every `if`/`while`/`for` body and every `for` header opens a new scope.

## Expressions and operators

Precedence, from lowest to highest (bitwise operators bind tighter than comparisons, as in
Python):

| Level | Operators | Associativity |
|---|---|---|
| 1 | `=  +=  -=  *=  /=  %=  &=  \|=  ^=  <<=  >>=` | right |
| 2 | `\|\|` | left |
| 3 | `&&` | left |
| 4 | `==  !=  <  <=  >  >=` | left |
| 5 | `\|` | left |
| 6 | `^` | left |
| 7 | `&` | left |
| 8 | `<<  >>` | left |
| 9 | `+  -` | left |
| 10 | `*  /  %` | left |
| 11 | unary `-  !  ~` | right |
| 12 | postfix `a[i]  f(x)  p.field  v.method()  x++  x--` | left |

So `x & 1 == 0` means `(x & 1) == 0`, and `1 << n + 1` means `1 << (n + 1)`.

### Typing rules

- **Integer arithmetic** (`+ - * / % & | ^ << >>`, unary `-` and `~`): operands narrower than
  64 bits (and chars) are promoted to `int` first. Then both sides are converted to a common
  type: an integer literal takes the other operand's type when it fits; otherwise the wider
  type wins; with equal widths the unsigned type wins (so `u64 + int` is `u64`, and `i128`
  or `i256` operands keep their width).
- Because of the promotion, an expression mixing narrow unsigned values can exceed `int`:
  `u32 z = 4294967295; z * z` is computed in `int` and wraps there. Convert one operand to
  `u64` (`u64(z) * z`) or `i128` when you need the full product.
- Overflow wraps (two's complement), including `MIN / -1` (gives `MIN`) and `MIN % -1`
  (gives `0`). `/` and `%` truncate toward zero (like C). Shifts keep the (promoted) left
  operand's type; `>>` is arithmetic for signed and logical for unsigned values; shifting by
  the bit width or more, or by a negative amount, shifts everything out (`0`, or `-1` for
  `>>` of a negative value).
- **chars** promote to `int` in arithmetic (`'c' - 'a'` is `2`, an `int`) but compare as
  characters (`c >= 'a'`).
- **floats**: mixing an integer and a float gives a float. `%` is integer-only.
- **comparisons** work on numbers, chars, and strings (lexicographic, bytewise); `==`/`!=`
  also on bools. The result is `bool`.
- **logic**: `&&`, `||`, `!` take bools; `&&` and `||` short-circuit.
- **strings**: `s + t` and `s + c` (char) build a new string.
- Conditions of `if`, `while` and `for` must be `bool` (write `n % 2 == 1`, not `n % 2`).

### Conversions

Implicit (assignment, initialisation, arguments, returns, array elements):
integer ↔ integer (truncates or extends), integer/char → `float`, `vec<T>` ↔ `T[]`.

Explicit, with a type name used like a function:

```text
int(3.99)      # 3   (float -> int truncates toward zero ...
int(1e30)      # 9223372036854775807   ... and saturates at the type's limits; NaN -> 0)
i8(300)        # 44  (integer -> integer wraps)
char(65)       # 'A'
float(7) / 2   # 3.5
bool(5)        # true (non-zero)
int(true)      # 1
string(42)     # "42"  -- string(x) works for every integer width, char, bool, float and string
```

## Statements and control flow

Every control-flow header ends with `:` followed by one statement — usually a `{ }` block.

```text
if x > 0: println("positive");
elif x == 0: println("zero");
else: { println("negative"); }

while i < n: { i += 1; }

for (int i = 0; i < n; i++): { ... }   # C-style; each part is optional: for ( ; ; ): { }
for i in [0..n-1]: { ... }             # inclusive range, no array is built
for x in arr: { ... }                  # arrays, vecs, strings (chars), maps (sorted keys)
```

- `break` leaves the innermost loop (or `TC:` block); `continue` jumps to its next iteration.
- A `for x in ...` variable is a **copy** of the element; assigning to it does not change
  the container or the iteration. Range loops evaluate both bounds once.
- Looping over a vec reads its length every iteration, so elements pushed during the loop
  are visited. Looping over a map iterates over a snapshot of its keys.
- An expression followed by `;` is a statement (`f(x);`, `x++;`, `v.push(1);`).

## Functions

```text
fn gcd(int a, int b) -> int {
    while b != 0: { int t = a % b; a = b; b = t; }
    return a;
}

fn show(vec<int>& v) {           # no `->`: returns nothing
    println(v);
}
```

- Parameters are local variables initialised with a **copy** of the argument, unless the
  parameter type ends in `&` (see [references](#value-semantics-and--references)).
- A function with a return type must return a value on every path (the compiler checks,
  treating `while true:` loops without `break` and `exit(...)` as non-returning).
- Calling a function before its definition is fine; using a global declared below the
  function is not.

## `dp` functions

```text
dp fn fib(i32 n) -> i32 {
    if n <= 1: return n;
    return fib(n - 1) + fib(n - 2);
}
```

`dp` marks a function whose result depends only on its arguments, and tells the compiler to
**cache** results: every call first looks the arguments up in a hash table; on a miss the
body runs and the result is stored. Recursive calls go through the cache, so naive
recursive recurrences run in time proportional to the number of distinct states.

Rules (checked at compile time):

- Parameters must be integers, chars or bools (they form the cache key), not references.
- The return type must be a scalar: an integer, char, bool or float.
- The function must not modify global variables or perform input/output — neither
  directly nor through the functions it calls (including writes through `&` arguments).
- Reading global variables **is** allowed. Then the cache is cleared automatically at the
  start of every `TC:` iteration, so answers from one testcase never leak into the next.
  A `dp fn` that reads no globals (like `fib`) keeps its cache for the whole run.
  (`vulpc -v` reports which policy each `dp fn` got.)

Mutating a global that a `dp fn` reads *outside* of a `TC:` block does not clear its cache.

## The `TC:` testcase block

```text
TC: {
    read int n;
    read int[n] a;
    println(...);
}
```

`TC:` reads one integer `t` from the input and runs its body `t` times. Each iteration
starts with fresh local variables. `continue` moves to the next testcase; `break` stops
early. `TC:` blocks may not be nested and may not appear inside functions.

## Input and output

### Output

- `print(a, b, ...)` writes its arguments one after another with **no separators**;
  `println(...)` does the same and then writes a newline. `println()` writes just a newline.
- Formats: integers in decimal (all widths up to 256 bits); `char` as the character;
  `bool` as `true`/`false`; strings verbatim; `float` as the shortest decimal that reads back
  exactly, in plain notation when the decimal exponent is in [-4, 16) and in scientific
  notation otherwise: `0.1`, `100`, `1500.25`, `0.0001`, `1e-05`, `1e+16` (like Python,
  without a trailing `.0`).
- Arrays and vecs print their elements separated by single spaces; if the elements are
  themselves arrays/vecs or structs, each element goes on its own line. Structs print
  their fields separated by spaces. Maps cannot be printed directly (loop over them).
- Output is buffered and flushed when the program ends (also on `exit()` and runtime
  errors) and whenever the program is about to wait for more input, so interactive
  problems work without explicit flushing.

### Input ("standard CP read")

Input is read as whitespace-separated tokens through a fast buffered reader.

```text
read int n, k;          # declare and read
read int[n] a;          # declare an array of n elements and read all of them
read int[n][m] grid;    # n*m numbers, row by row
read string s;          # one whitespace-delimited token
read char c;            # next non-blank character
read(x, a[i], p.y);     # read into existing variables, elements or fields
read(v);                # an existing array/vec: reads v.len() elements
```

Readable types: integers of every width, `char`, `float`, `string`, arrays/vecs of readable
types, and structs whose fields are readable (read field by field). Reading past the end
of input, or an integer that does not fit the type it is read into, is a runtime error.

## Containers: arrays, vec, string, map

| Operation | `T[]` | `vec<T>` | `string` | `map<K,V>` |
|---|---|---|---|---|
| `x[i]` read / write | ✓ | ✓ | ✓ (char) | ✓ (see below) |
| `x.len()` | ✓ | ✓ | ✓ | ✓ |
| `x.push(e)` | – | ✓ | ✓ (char) | – |
| `x.pop()` | – | ✓ returns the last element | ✓ | – |
| `x.back()` | ✓ | ✓ | ✓ | – |
| `x.clear()` | – | ✓ | ✓ | ✓ |
| `x.sort()` | ✓ | ✓ | ✓ | – (always sorted) |
| `m.has(k)` / `m.erase(k)` | – | – | – | ✓ |
| `for e in x` | elements | elements | chars | keys in sorted order |

- Indexes are checked: an out-of-range index stops the program with a runtime error.
- Map indexing: **reading** `m[k]` for a missing key gives the value type's zero value and
  does **not** insert `k`; **writing** (`m[k] = v`, `m[k] += 1`, `m[k]++`, `m[k].push(x)`,
  `read(m[k])`, passing `m[k]` to a `&` parameter) inserts a zero value first.
- `sort()` sorts ascending; it supports numbers up to 128 bits, chars, bools and strings.
- Map keys may be integers up to 64 bits, chars, bools or strings. Values can be anything,
  including vecs and other maps (`map<int, vec<int>> g; g[u].push(v);`).

## Structs

```text
struct Edge {
    int to;
    i32 w;
    vec<int> tags;       # fields may own containers
}
struct Node { int val; vec<Node> kids; }   # recursion through a container is fine

Edge e = Edge(2, 7, [1, 2]);   # constructor: all fields, in order
Edge z;                        # or Edge(): every field zero / empty
e.tags.push(3);
println(e.to, " ", e.tags);
```

A struct may not contain itself by value (directly or through other structs).

## Value semantics and `&` references

Every value in Vulp behaves like a value: assigning, passing to a parameter, returning,
pushing into a vec, storing in a map, or binding a `for` variable makes an **independent
deep copy** — including all nested vecs, strings and maps:

```text
vec<int> a = [1, 2];
vec<int> b = a;      # copy
b.push(3);           # a is still [1, 2]
```

To let a function modify the caller's variable, or to avoid copying a large container,
declare the parameter with `&`:

```text
fn dfs(int u, vec<int>[]& adj, bool[]& seen) {
    seen[u] = true;
    for v in adj[u]: if !seen[v]: dfs(v, adj, seen);
}
```

The argument for a `&` parameter must be a variable, element or field of exactly the
parameter's type. The compiler rejects calls in which a reference could dangle — where the
callee could resize, replace or erase a container while holding a reference into it:

- an element (`a[i]`, `m[k]`, `s.items[j]`) may not be passed by reference **together with**
  a reference to the container that holds it (`grow(all, all[0])` is rejected; element indices
  are assumed to be possibly equal, so `f(g[i], g[j][0])` is rejected too);
- inside a function, two `&` parameters, or a `&` parameter and a global, may be the same
  object (after a call like `f(v, v)` or `f(G)`), so an element of one may not be passed
  together with the other when the other could contain it;
- an element of a **global** may not be passed by reference to a function that modifies that
  global, directly or through the functions it calls (also when the element is reached
  through `&` parameters that refer to the global).

The rules are conservative: they may reject some calls that would happen to be safe.
`swap(a[i], a[j])`, `inc(cnt[x])`, `relax(dist[v], d)`, `f(g[0], g[1])`, `f(v, v)` and passing
whole containers are all fine.
Arguments are evaluated left to right, but the addresses of `&` arguments are taken only
after all argument values have been computed.

### Evaluation order

- Operands, call arguments, print arguments and index expressions run left to right.
- An assignment (`=` and every compound form) evaluates its right side first, then the
  index expressions of its target, then stores: in `a[i()] += f()`, `f()` runs before `i()`.
- A method call evaluates its arguments first, then the index expressions of its receiver:
  `m[key()].push(next())` runs `next()` before `key()`.
- A place's address is only computed after all of its index / key expressions have run, so
  an index expression that grows or replaces a container never leaves a stale address.

### Memory

Because every heap buffer has exactly one owner, Vulp frees memory deterministically:
the old value of a variable, element or field is freed when it is overwritten, a
declaration that runs again (inside a loop or a `TC:` block) frees the previous value, and
a function frees its locals and by-value parameters when it returns. Loops that copy
containers therefore run in constant memory. Temporaries (function results, literals,
concatenations) are freed after use; `f()[0]` or `mk().field` copies the element out and
then frees the temporary.

`for x in a[i]:` evaluates `i` once and then follows the current contents of `a[i]` (the
loop sees elements pushed meanwhile, and reassigning `i` in the body does not switch
lists); `for x in f():` walks a private copy.

## Builtins

| Builtin | Meaning |
|---|---|
| `min(a, b, ...)`, `max(a, b, ...)` | smallest / largest of two or more numbers; arguments of one type keep it, mixed types follow the arithmetic rules (so `max(a, b)` agrees with `a < b`) |
| `abs(x)` | absolute value of an integer or float |
| `exit(code)` | flush output and stop the program with the given exit code |

## Runtime behaviour

- The program runs on a 1 GiB stack, so recursion a million levels deep is fine.
- Runtime errors (index out of bounds, division by zero, `pop()`/`back()` on an empty
  container, reading past the end of input or an input integer that does not fit its type)
  flush the output written so far, print `vulp runtime error (line N): ...` on stderr and
  exit with code 1.
- `vulpc --no-checks` removes the bounds and division-by-zero checks for maximum speed
  (division by zero then crashes the program).
- Memory is released as described in [Memory](#memory). Set `VULP_DEBUG_FREE=1` to make the
  runtime poison freed blocks instead (a debugging aid used by the test-suite).

## Not in V1

Deliberately out of scope for this version: lambdas and first-class functions, generics,
modules, classes, exceptions, formatted float output (fixed precision), sorting structs or
custom comparators, `range_q` / `dp_table` / graph abstractions from the CPIR plan,
complexity contracts, reading whole lines (input is token based), and runtime checking of
`int<lo..hi>` ranges. Programs may nest blocks and expressions at most 100000 levels deep (a chain of more than
100000 operators, such as `1 + 1 + ... + 1`, also counts as that deep).
