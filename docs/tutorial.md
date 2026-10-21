# Vulp tutorial

This tutorial takes you from building the compiler to solving a typical contest problem.
Every program shown here is in [`examples/`](../examples) or is short enough to type in.
The full rules are in the [language reference](language.md).

1. [Build the compiler](#1-build-the-compiler)
2. [Hello, world — JIT run vs. native executable](#2-hello-world)
3. [The compiler's options](#3-the-compilers-options)
4. [Variables, types and integer widths](#4-variables-types-and-integer-widths)
5. [Control flow](#5-control-flow)
6. [Reading input the competitive-programming way](#6-reading-input)
7. [Multiple testcases with `TC:`](#7-multiple-testcases-with-tc)
8. [Functions and `dp fn`](#8-functions-and-dp-fn)
9. [Arrays, vec, string, map](#9-arrays-vec-string-and-map)
10. [Structs and value semantics](#10-structs-and-value-semantics)
11. [A complete problem](#11-a-complete-problem)
12. [When something goes wrong](#12-when-something-goes-wrong)
13. [Running the test-suite](#13-running-the-test-suite)

---

## 1. Build the compiler

You need CMake (≥ 3.20), a C++20 compiler (g++ 13 or clang 18), Ninja (optional) and the
LLVM 18 development package (`llvm-18-dev` on Ubuntu, installed at `/usr/lib/llvm-18`).

```bash
cd vulp
cmake -S . -B build -G Ninja          # add -DVULP_LLVM_DIR=/path/to/lib/cmake/llvm for another LLVM 18
cmake --build build
```

This produces, inside `build/`:

- `vulpc` — the compiler,
- `libvulprt.a` — the runtime library that is linked into every Vulp executable,
- `vulp_unit_tests` — the unit tests.

Optionally put the compiler on your `PATH` for this shell:

```bash
export PATH="$PWD/build:$PATH"
```

## 2. Hello, world

Create `hello.vp`:

```text
println("Hello from Vulp!");
```

A Vulp program is just a list of statements; there is no `main` function to write.
Statements end with `;` and `#` starts a comment.

**Run it directly** — `vulpc` compiles it to machine code in memory with the LLVM JIT and
runs it straight away (this is the default action):

```console
$ vulpc hello.vp
Hello from Vulp!
```

**Or build a native executable** with `-o`, the way you would for a judge:

```console
$ vulpc hello.vp -o hello
$ ./hello
Hello from Vulp!
```

Both paths use the same front end, the same LLVM optimisations (`-O2` by default) and the
same runtime library, so they behave identically.

## 3. The compiler's options

```text
vulpc [options] <file.vp>

  (no action)       JIT-compile and run          vulpc prog.vp < input.txt
  -o <file>         build a native executable    vulpc prog.vp -o prog
  -c                build an object file         vulpc -c prog.vp -o prog.o
  --emit-llvm       print the LLVM IR            vulpc --emit-llvm -O0 prog.vp
  --tokens          print the tokens (lexer output)
  --ast             print the syntax tree (parser output)
  --typed-ast       print the syntax tree with every expression's type
  --check           only report errors
  -O0 .. -O3        optimisation level (default -O2)
  --no-checks       drop array bounds / division-by-zero checks
  --native          tune the executable for this CPU
  -v                show what every compiler stage did
```

The inspection flags are handy for learning what the compiler sees:

```console
$ cat tiny.vp
int x = 1 + 2 * 3;
$ vulpc --ast tiny.vp
(var int (x (+ 1 (* 2 3))))
$ vulpc --typed-ast tiny.vp
(var int (x (+ 1:i64 (* 2:i64 3:i64):i64):i64))
```

## 4. Variables, types and integer widths

```text
int a = 10, b;              # int is 64 bits; b starts at 0
float pi = 3.14159;
char c = 'x';
bool ok = a > 5;
string name = "vulp";
println(a, " ", b, " ", pi, " ", c, " ", ok, " ", name);   # 10 0 3.14159 x true vulp
```

`print`/`println` glue their arguments together with no spaces, so add `" "` where you want
one.

Integers can be any width from 1 to 256 bits. Vulp rounds the width **up** to the next of
8, 16, 32, 64, 128 or 256 bits. You can also describe an integer by the range of values
it holds and let Vulp pick the width:

```text
i21 small = 1000000;            # stored in 32 bits
u3 tiny = 5;                    # stored in 8 bits (unsigned)
int<0..1000000007> m;           # 31 bits needed -> stored as i32
i128 big = 1;
for i in [1..30]: big *= 10;    # 10^30 fits in 128 bits
i256 f = 1;
for i in [1..50]: f *= i;       # 50! fits in 256 bits
println(big, " ", f);
```

`examples/09_big_numbers.vp` prints `50!` and multiplies two 128-bit numbers read from the
input. The rounded width is how the value is *stored*: arithmetic on integers narrower than
64 bits is done in 64 bits, so `int<0..100000> x; int y = x * x;` cannot overflow, and a
result only wraps when you store it back into a narrow variable. `char`s turn into numbers
in arithmetic (`'c' - 'a'` is `2`). Convert explicitly with a type name: `int(3.9)`,
`float(n)`, `char(65)`, `i32(x)`, `string(42)`.

## 5. Control flow

Every `if`, `elif`, `else`, `while`, `for` and `TC` header ends with a colon, followed by
one statement or a `{ }` block:

```text
for i in [1..15]: {                 # ranges are inclusive: 1, 2, ..., 15
    if i % 15 == 0: println("FizzBuzz");
    elif i % 3 == 0: println("Fizz");
    elif i % 5 == 0: println("Buzz");
    else: println(i);
}

int n = 27, steps = 0;
while n != 1: {
    if n % 2 == 0: n /= 2;
    else: n = 3 * n + 1;
    steps++;
}

for (int i = 0; i < 10; i += 3): print(i, " ");   # C-style: 0 3 6 9
```

`for x in container` walks arrays, vecs, strings (one char at a time) and maps (keys in
sorted order). `break` and `continue` work in every loop. Conditions must be `bool`:
write `if n % 2 == 1:` rather than `if n % 2:`.

## 6. Reading input

Contest input is a stream of whitespace-separated tokens. Vulp reads it with a fast
buffered reader. The shortest form declares and reads in one go:

```text
read int n, k;              # two integers
read int[n] a;              # an array of n integers
read string s;              # a word
read int[n][m] grid;        # n rows of m integers
```

You can also read into variables, elements and fields that already exist:

```text
int x, y;
read(x, y);
read(a[0]);
```

Output is buffered too, and flushed automatically at the end of the program — and before
the program waits for more input, so interactive problems just work.

## 7. Multiple testcases with `TC:`

Most problems start with the number of testcases. `TC:` reads that number and runs its
block that many times:

```text
# examples/03_testcases.vp
TC: {
    read int n;
    read int[n] a;
    int sum = 0, best = a[0];
    for x in a: {
        sum += x;
        best = max(best, x);
    }
    println(sum, " ", best);
}
```

```console
$ printf '2\n3\n1 2 3\n2\n-5 4\n' | vulpc examples/03_testcases.vp
6 3
-1 4
```

Inside `TC:`, `continue` skips to the next testcase.

## 8. Functions and `dp fn`

```text
fn gcd(int a, int b) -> int {
    if b == 0: return a;
    return gcd(b, a % b);
}

fn greet(string who) {               # no `->`: the function returns nothing
    println("hi ", who);
}
```

Put `dp` in front of `fn` and Vulp **memoises** the function: results are cached by
argument, so a plain recursive definition becomes a dynamic-programming solution:

```text
dp fn fib(i32 n) -> i32 {
    if n <= 1:
        return n;
    else: {
        i32 a = fib(n - 1), b = fib(n - 2);
        return a + b;
    }
}
println(fib(46));                    # instant: 1836311903
```

A `dp fn` is meant to be a pure function of its arguments, so the compiler checks that it
never writes a global variable and never prints or reads (also through the functions it
calls). Its parameters must be integers, chars or bools and it must return a scalar.

It **may read** globals — handy for memoised recursion over the input. In that case the
cache is cleared automatically before every `TC:` iteration, so one testcase's answers
never leak into the next:

```text
# examples/08_lcs_testcases.vp
string a, b;
dp fn lcs(int i, int j) -> int {
    if i == a.len() || j == b.len(): return 0;
    if a[i] == b[j]: return 1 + lcs(i + 1, j + 1);
    return max(lcs(i + 1, j), lcs(i, j + 1));
}
TC: {
    read(a, b);
    println(lcs(0, 0));
}
```

`vulpc -v` tells you which policy each `dp fn` got:

```console
$ vulpc -v --check examples/08_lcs_testcases.vp
...
[vulpc] dp fn lcs: memoised; cache cleared before every TC iteration (reads globals)
```

Programs run on a 1 GiB stack, so even recursion a million calls deep is fine.

## 9. Arrays, vec, string and map

```text
int[5] a;                       # five zeroes; length fixed at creation
int[] b = [3, 1, 4, 1, 5];      # array literal
int[] r = [1..10];              # range array: 1 2 ... 10
vec<int> v;                     # growable
v.push(7);
v.push(2);
v.sort();                       # 2 7
println(v, " ", v.len(), " ", v.back());   # 2 7 2 7

int n = 3;
vec<int>[n + 1] adj;            # adjacency list: n+1 empty vecs
adj[1].push(2);                 # edge 1 -> 2

string s = "hello";
s[0] = 'j';                     # strings are mutable
s += " world";
println(s, " ", s.len());       # jello world 11

map<string, int> count;         # ordered map
count["apple"] += 1;            # missing keys start at 0
println(count["pear"]);         # reading a missing key gives 0 and does not insert it
if count.has("pear"): println("never");
for k in count: println(k, " ", count[k]);   # keys come out sorted
```

Array indexes are checked; `a[5]` on a 5-element array stops the program with
`vulp runtime error (line N): index 5 out of bounds for length 5`.

See `examples/05_graph_bfs.vp` (BFS with `vec<int>[n+1]`), `examples/06_knapsack.vp`
(a 2-D table) and `examples/07_word_count.vp` (a map).

## 10. Structs and value semantics

```text
struct Point { int x, y; }
Point p = Point(3, 4);
Point q = p;          # a copy
q.x = 100;
println(p, " | ", q); # 3 4 | 100 4
```

All Vulp values behave like values: assigning or passing a vec, string, map or struct makes
an independent copy (deep, including nested containers). Because nothing is shared, Vulp also
frees memory automatically — an overwritten value, a loop-local container, a function's
locals — so copying inside loops does not make memory grow. To change the caller's variable
or avoid copying a big container, use a reference parameter with `&`:

```text
fn add_edge(vec<int>[]& adj, int u, int v) {
    adj[u].push(v);
    adj[v].push(u);
}
```

`examples/10_structs.vp` reads a list of students into a `vec<Student>`.

## 11. A complete problem

> *Given `t` testcases, each with an array of `n` integers, print the length of the longest
> strictly increasing contiguous run and how many runs have that length.*

```text
TC: {
    read int n;
    read int[n] a;
    int best = 0, ways = 0, cur = 0;
    for i in [0..n-1]: {
        if i > 0 && a[i] > a[i - 1]: cur++;
        else: cur = 1;
        if cur > best: { best = cur; ways = 1; }
        elif cur == best: ways++;
    }
    println(best, " ", ways);
}
```

```console
$ vulpc runs.vp -o runs
$ printf '2\n6\n1 2 3 1 2 3\n3\n5 4 3\n' | ./runs
3 2
1 3
```

Note how `&&` short-circuits: `a[i - 1]` is never evaluated when `i == 0`.

## 12. When something goes wrong

Compile errors point at the exact spot and the compiler keeps going to report several
errors at once:

```console
$ cat broken.vp
int total = 0;
int n = 5;
print(totl);
if n % 2: println("odd");
$ vulpc broken.vp
broken.vp:3:7: semantic error: unknown variable 'totl'
    print(totl);
          ^
broken.vp:4:6: semantic error: condition of 'if' must be bool, found i64
    if n % 2: println("odd");
         ^
vulpc: 2 error(s); compilation stopped after the semantic analysis.
```

Runtime errors print the line and exit with code 1 after flushing everything printed so far.
To see what the compiler generates, use `--emit-llvm` (try `-O0` for the unoptimised IR).

Exit codes of `vulpc` itself are listed in [error_codes.md](error_codes.md).

## 13. Running the test-suite

```bash
cd build && ctest --output-on-failure
```

This runs the unit tests (lexer, parser, types, semantic analysis, runtime, code
generation), every program in `tests/e2e` both through the JIT and as a native executable,
the compile-error tests in `tests/errors`, and every example in `examples/`. You can also
run one group directly, e.g.:

```bash
python3 tests/run_tests.py --vulpc build/vulpc --mode jit --filter t14
./build/vulp_unit_tests parser
```
