#include "TestFramework.hpp"
#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"
#include "sema/Semantics.hpp"

namespace {
    struct SemaResult {
        std::string typed;   // typed AST
        std::string errors;
        size_t error_count = 0;
        std::vector<std::pair<std::string, bool>> dp_resets; // (dp fn name, reset per testcase)
    };

    SemaResult analyze(const std::string& src) {
        Diagnostics diag("test.vp", src);
        auto tokens = Lexer::tokenize(src, diag);
        Parser parser(std::move(tokens), diag);
        parser.parse();
        AST::Program program = parser.take_program();
        if (diag.has_errors()) return {"", diag.format(), diag.count(), {}};
        Sema::TypeContext types;
        SemanticAnalyzer sema(program, diag, types);
        sema.analyze();
        SemaResult r{AST::print_program_typed(program), diag.format(), diag.count(), {}};
        for (const auto& fn : sema.program_info().fns) {
            if (fn->is_dp) r.dp_resets.emplace_back(fn->name, fn->reset_cache_per_testcase);
        }
        return r;
    }

    // Typed S-expression of the last top-level statement.
    std::string last(const std::string& src) {
        const auto r = analyze(src);
        if (r.error_count) return "ERROR: " + r.errors;
        std::string t = r.typed.substr(0, r.typed.size() - 1);
        const auto pos = t.rfind('\n');
        return pos == std::string::npos ? t : t.substr(pos + 1);
    }

    bool ok(const std::string& src) {
        return analyze(src).error_count == 0;
    }

    std::string errors(const std::string& src) {
        return analyze(src).errors;
    }
} // namespace

// ---- 3.1 integer widths & range ints ----

TEST_CASE("sema: iN types are rounded up to a power of two") {
    CHECK_EQ(last("i21 x; x;"), std::string("x:i32"));
    CHECK_EQ(last("u3 x; x;"), std::string("x:u8"));
    CHECK_EQ(last("i100 x; x;"), std::string("x:i128"));
    CHECK_EQ(last("i200 x; x;"), std::string("x:i256"));
    CHECK_EQ(last("i1 x; x;"), std::string("x:i8"));
    CHECK_EQ(last("int x; x;"), std::string("x:i64"));
}

TEST_CASE("sema: range ints choose their width from the range") {
    CHECK_EQ(last("int<0..100> a; a;"), std::string("a:i8"));
    CHECK_EQ(last("int<0..1000000007> b; b;"), std::string("b:i32"));
    CHECK_EQ(last("int<-5..300> c; c;"), std::string("c:i16"));
    CHECK_EQ(last("int<0..1000000000000000000> d; d;"), std::string("d:i64"));
    CHECK_EQ(last("int<0..100000000000000000000> e; e;"), std::string("e:i128"));
    CHECK_CONTAINS(errors("int<10..1> x;"), "empty range");
}

// ---- 3.3 expressions ----

TEST_CASE("sema: integer literals adapt to the other operand") {
    CHECK_EQ(last("u64 x; x + 1;"), std::string("(+ x:u64 1:u64):u64"));
    CHECK_EQ(last("i128 x; x * 1000;"), std::string("(* x:i128 1000:i128):i128"));
    CHECK_EQ(last("u64 x; x + -1;"), std::string("(+ x:u64 (- 1:i64):i64):u64")); // does not fit: stays i64
    CHECK_EQ(last("i8 x; x == 5;"), std::string("(== x:i8 5:i8):bool"));      // comparisons keep the width
}

TEST_CASE("sema: integers narrower than 64 bits promote to int in arithmetic") {
    CHECK_EQ(last("i8 x = 5; x + 1;"), std::string("(+ x:i8 1:i64):i64"));
    CHECK_EQ(last("int<0..100000> x; x * x;"), std::string("(* x:i32 x:i32):i64"));
    CHECK_EQ(last("u32 a; i16 b; a - b;"), std::string("(- a:u32 b:i16):i64"));
    CHECK_EQ(last("u8 b; ~b;"), std::string("(~ b:u8):i64"));
    CHECK_EQ(last("i32 a; abs(a);"), std::string("(call abs a:i32):i64"));
    CHECK_EQ(last("u16 a; a << 20;"), std::string("(<< a:u16 20:i64):i64"));
    CHECK(ok("i8 x = 5; x += 1000; x++; i8 y = x * 2;")); // results wrap when stored back
}

TEST_CASE("sema: mixed integer widths use the wider type, unsigned on ties") {
    CHECK_EQ(last("i32 a; i64 b; a + b;"), std::string("(+ a:i32 b:i64):i64"));
    CHECK_EQ(last("i64 a; u64 b; a < b;"), std::string("(< a:i64 b:u64):bool"));
    CHECK_EQ(last("i32 a; float f; a * f;"), std::string("(* a:i32 f:float):float"));
}

TEST_CASE("sema: chars promote to int in arithmetic but not in comparisons") {
    CHECK_EQ(last("char c = 'x'; c - 'a';"), std::string("(- c:char 'a':char):i64"));
    CHECK_EQ(last("char c; c == 'b';"), std::string("(== c:char 'b':char):bool"));
    CHECK_EQ(last("char c; c >= 'a' && c <= 'z';"),
             std::string("(&& (>= c:char 'a':char):bool (<= c:char 'z':char):bool):bool"));
}

TEST_CASE("sema: large literals get wide types") {
    CHECK_EQ(last("9223372036854775807;"), std::string("9223372036854775807:i64"));
    CHECK_EQ(last("9223372036854775808;"), std::string("9223372036854775808:i128"));
    CHECK(ok("i128 x = 100000000000000000000000;"));
    CHECK(ok("u64 m = 0xFFFFFFFFFFFFFFFF;"));
    CHECK_CONTAINS(errors("u8 x = 300;"), "integer constant does not fit in u8");
    CHECK_CONTAINS(errors("i8 x = -129;"), "integer constant does not fit in i8");
    CHECK(ok("i8 x = -128;"));
}

TEST_CASE("sema: implicit conversions") {
    CHECK(ok("i32 a = 5; int b = a; i8 c = b; char d = 65; float f = b;"));
    CHECK(ok("vec<int> v = [1, 2]; int[] a = v; vec<int> w = a;"));
    CHECK_CONTAINS(errors("int x = \"hi\";"), "type mismatch in initialization of 'x': expected i64, found string");
    CHECK_CONTAINS(errors("float f = 1.5; int x = f;"), "expected i64, found float");
    CHECK_CONTAINS(errors("vec<i32> v; vec<int> w = v;"), "expected vec<i64>, found vec<i32>");
    CHECK_CONTAINS(errors("bool b = 1;"), "expected bool, found i64");
}

TEST_CASE("sema: operators type-check their operands") {
    CHECK_CONTAINS(errors("int x; x && true;"), "operator '&&' needs bool operands");
    CHECK_CONTAINS(errors("string s; s - s;"), "operator '-' needs numbers");
    CHECK_CONTAINS(errors("float f; f % 2.0;"), "operator '%' needs integers");
    CHECK_CONTAINS(errors("bool b; b < true;"), "cannot order these types");
    CHECK_CONTAINS(errors("int x; !x;"), "'!' needs a bool");
    CHECK_EQ(last("string a, b; a + b;"), std::string("(+ a:string b:string):string"));
    CHECK_EQ(last("string a; a + 'c';"), std::string("(+ a:string 'c':char):string"));
    CHECK_EQ(last("string a, b; a < b;"), std::string("(< a:string b:string):bool"));
    CHECK_EQ(last("int x; 1 << x;"), std::string("(<< 1:i64 x:i64):i64"));
}

TEST_CASE("sema: assignment and compound assignment") {
    CHECK(ok("int x; x = 5; x += 2; x <<= 1; x++;"));
    CHECK(ok("string s; s += 'c'; s += \"xy\";"));
    CHECK(ok("char c = 'a'; c += 1;"));
    CHECK_CONTAINS(errors("int x; x += \"a\";"), "needs numbers");
    CHECK_CONTAINS(errors("fn f() -> int { return 1; } f() = 3;"), "cannot assign to a temporary value");
    CHECK_CONTAINS(errors("float f; f++;"), "++ needs an integer");
    CHECK_CONTAINS(errors("5 = 3;"), "cannot assign to a temporary value");
}

TEST_CASE("sema: names, scopes and redeclaration") {
    CHECK_CONTAINS(errors("x = 1;"), "unknown variable 'x'");
    CHECK_CONTAINS(errors("int x; int x;"), "redeclaration of 'x' in the same scope");
    CHECK(ok("int x; { int x = 2; x = 3; }"));
    CHECK_CONTAINS(errors("{ int y = 1; } y = 2;"), "unknown variable 'y'");
    CHECK_CONTAINS(errors("int x = x;"), "unknown variable 'x'");
    CHECK_CONTAINS(errors("fn f() {} f;"), "'f' is a function");
}

TEST_CASE("sema: functions see globals declared before them only") {
    CHECK(ok("int g = 1; fn f() -> int { return g; }"));
    CHECK_CONTAINS(errors("fn f() -> int { return g; } int g = 1;"), "unknown variable 'g'");
    CHECK(ok("fn a() -> int { return b(); } fn b() -> int { return 1; }")); // forward calls are fine
}

TEST_CASE("sema: calls are checked") {
    CHECK_CONTAINS(errors("nope(1);"), "unknown function 'nope'");
    CHECK_CONTAINS(errors("fn f(int a) {} f();"), "function 'f' expects 1 argument(s), got 0");
    CHECK_CONTAINS(errors("fn f(int a) {} f(\"s\");"), "for parameter 'a' of 'f'");
    CHECK_CONTAINS(errors("int x; x(1);"), "'x' is a variable, not a function");
    CHECK_CONTAINS(errors("fn f() {} int x = f();"), "returns nothing");
    CHECK_CONTAINS(errors("fn f() {} println(f());"), "cannot print the result of a function that returns nothing");
}

TEST_CASE("sema: builtins min / max / abs / exit") {
    CHECK_EQ(last("i32 a; min(a, 5);"), std::string("(call min a:i32 5:i32):i32"));
    CHECK_EQ(last("char a, b; max(a, b);"), std::string("(call max a:char b:char):char"));
    CHECK_EQ(last("int a; float b; max(a, b, 0);"), std::string("(call max a:i64 b:float 0:i64):float"));
    CHECK_EQ(last("i32 a; abs(a);"), std::string("(call abs a:i32):i64"));
    CHECK_CONTAINS(errors("min(1);"), "min() needs at least two arguments");
    CHECK_CONTAINS(errors("string s; abs(s);"), "abs() needs a number");
    CHECK(ok("exit(0);"));
    CHECK_CONTAINS(errors("fn min(int a) {}"), "'min' is a builtin function");
}

TEST_CASE("sema: casts") {
    CHECK_EQ(last("float f; int(f);"), std::string("(cast int f:float):i64"));
    CHECK_EQ(last("int x; char(x);"), std::string("(cast char x:i64):char"));
    CHECK_EQ(last("int x; string(x);"), std::string("(cast string x:i64):string"));
    CHECK_EQ(last("char c; float(c);"), std::string("(cast float c:char):float"));
    CHECK(ok("bool b = bool(1); int x = int(b);"));
    CHECK_CONTAINS(errors("int(\"a\");"), "cannot convert string to i64");
    CHECK(ok("string a = string(1.5); string b = string(true); i256 big; string c = string(big);"));
    CHECK_CONTAINS(errors("vec<int> v; string(v);"), "cannot convert vec<i64> to string");
}

TEST_CASE("sema: indexing") {
    CHECK_EQ(last("string s; s[0];"), std::string("(index s:string 0:i64):char"));
    CHECK_EQ(last("vec<vec<int>> g; g[1][2];"), std::string("(index (index g:vec<vec<i64>> 1:i64):vec<i64> 2:i64):i64"));
    CHECK_EQ(last("map<string, int> m; m[\"a\"];"), std::string("(index m:map<string, i64> \"a\":string):i64"));
    CHECK_CONTAINS(errors("int x; x[0];"), "cannot index a value of type i64");
    CHECK_CONTAINS(errors("int[5] a; a[1.5];"), "index must be an integer");
    CHECK_CONTAINS(errors("map<int, int> m; m[\"k\"];"), "as map key");
}

TEST_CASE("sema: array declarations") {
    CHECK_EQ(last("int n = 3; int[n][n + 1] g; g;"), std::string("g:i64[][]"));
    CHECK_EQ(last("vec<int>[4] adj; adj;"), std::string("adj:vec<i64>[]"));
    CHECK_CONTAINS(errors("int[2] a = [1, 2];"), "cannot combine an array size with an initializer");
    CHECK_CONTAINS(errors("int[1.5] a;"), "index must be an integer");
    CHECK_CONTAINS(errors("int n; int[][n] a;"), "a sized array dimension cannot follow an unsized one");
}

TEST_CASE("sema: array and range literals") {
    CHECK_EQ(last("[1, 2, 3];"), std::string("(array 1:i64 2:i64 3:i64):vec<i64>"));
    CHECK_EQ(last("[1, 'a'];"), std::string("(array 1:i64 'a':char):vec<i64>")); // converted at codegen
    CHECK_EQ(last("i32[] a = [1, 2]; a;"), std::string("a:i32[]"));
    CHECK_EQ(last("[1..10];"), std::string("(range 1:i64 10:i64):vec<i64>"));
    CHECK_EQ(last("['a'..'e'];"), std::string("(range 'a':char 'e':char):vec<char>"));
    CHECK_CONTAINS(errors("[1, \"a\"];"), "incompatible types i64 and string");
    CHECK_CONTAINS(errors("[];"), "cannot infer the element type of an empty array literal");
    CHECK(ok("vec<int> v = [];"));
    CHECK_CONTAINS(errors("[1.5..3];"), "range bounds must be integers");
}

TEST_CASE("sema: container methods") {
    CHECK_EQ(last("vec<int> v; v.len();"), std::string("(method v:vec<i64> len):i64"));
    CHECK_EQ(last("vec<int> v; v.pop();"), std::string("(method v:vec<i64> pop):i64"));
    CHECK_EQ(last("string s; s.back();"), std::string("(method s:string back):char"));
    CHECK_EQ(last("map<int, int> m; m.has(3);"), std::string("(method m:map<i64, i64> has 3:i64):bool"));
    CHECK(ok("vec<int> v; v.push(1); v.clear(); v.sort(); string s; s.push('a'); map<int,int> m; m.erase(1);"));
    CHECK_CONTAINS(errors("int[3] a; a.push(1);"), "cannot push to a fixed-size array");
    CHECK_CONTAINS(errors("vec<int> v; v.push(\"x\");"), "in push()");
    CHECK_CONTAINS(errors("vec<int> v; v.has(1);"), "type vec<i64> has no method 'has'");
    CHECK_CONTAINS(errors("vec<int> v; v.len;"), "use .len()");
    CHECK_CONTAINS(errors("map<int,int> m; m.sort();"), "has no method 'sort'");
    CHECK_CONTAINS(errors("vec<i256> v; v.sort();"), "sort() supports numbers up to 128 bits");
    CHECK_CONTAINS(errors("struct P { int x; } vec<P> v; v.sort();"), "not P");
}

TEST_CASE("sema: map key types") {
    CHECK(ok("map<int, string> a; map<string, int> b; map<char, int> c; map<i32, vec<int>> d;"));
    CHECK_CONTAINS(errors("map<float, int> m;"), "map keys must be integers");
    CHECK_CONTAINS(errors("map<i128, int> m;"), "map keys must be integers (up to 64 bits)");
}

TEST_CASE("sema: structs") {
    const std::string pt = "struct P { int x, y; } ";
    CHECK_EQ(last(pt + "P p; p.x;"), std::string("(. p:P x):i64"));
    CHECK_EQ(last(pt + "P(1, 2);"), std::string("(call P 1:i64 2:i64):P"));
    CHECK(ok(pt + "P p = P(); P q = p; P[3] ps; ps[0].y = 4;"));
    CHECK_CONTAINS(errors(pt + "P(1);"), "construct it with no arguments or with all of them");
    CHECK_CONTAINS(errors(pt + "P p; p.z;"), "struct 'P' has no field 'z'");
    CHECK_CONTAINS(errors("int x; x.y;"), "has no fields");
    CHECK_CONTAINS(errors("struct A { B b; } struct B { A a; }"), "contains itself by value");
    CHECK(ok("struct Node { int val; vec<Node> kids; }"));
    CHECK_CONTAINS(errors("struct P { int x, x; }"), "duplicate field 'x'");
    CHECK_CONTAINS(errors("Q q;"), "unknown type 'Q'");
}

// ---- 3.4 statements ----

TEST_CASE("sema: conditions must be bool") {
    CHECK_CONTAINS(errors("if 1: {}"), "condition of 'if' must be bool, found i64");
    CHECK_CONTAINS(errors("while \"x\": {}"), "condition of 'while' must be bool");
    CHECK_CONTAINS(errors("for (int i = 0; i; i++): {}"), "condition of 'for' must be bool");
}

TEST_CASE("sema: loop variables") {
    CHECK_EQ(last("for i in [1..10]: { i; }"), std::string("(for-in i (range 1:i64 10:i64):vec<i64> (block i:i64))"));
    CHECK_EQ(last("string s; for c in s: { c; }"), std::string("(for-in c s:string (block c:char))"));
    CHECK_EQ(last("map<string, int> m; for k in m: { k; }"),
             std::string("(for-in k m:map<string, i64> (block k:string))"));
    CHECK_CONTAINS(errors("int x; for i in x: {}"), "cannot iterate over a value of type i64");
    CHECK_CONTAINS(errors("for i in [1..3]: {} i;"), "unknown variable 'i'");
}

TEST_CASE("sema: break / continue / return placement") {
    CHECK_CONTAINS(errors("break;"), "'break' outside of a loop");
    CHECK_CONTAINS(errors("if true: continue;"), "'continue' outside of a loop");
    CHECK(ok("while true: { if true: break; continue; }"));
    CHECK(ok("TC: { continue; }"));
    CHECK_CONTAINS(errors("return 1;"), "'return' outside of a function");
    CHECK_CONTAINS(errors("fn f() { return 1; }"), "function 'f' does not return a value");
    CHECK_CONTAINS(errors("fn f() -> int { return; }"), "must return a value of type i64");
}

TEST_CASE("sema: missing returns are detected") {
    CHECK_CONTAINS(errors("fn f(int x) -> int { if x > 0: return 1; }"), "can reach the end without returning");
    CHECK(ok("fn f(int x) -> int { if x > 0: return 1; else: return 2; }"));
    CHECK(ok("fn f(int x) -> int { if x > 0: return 1; elif x < 0: return -1; else: { return 0; } }"));
    CHECK(ok("fn f() -> int { while true: { } }"));
    CHECK_CONTAINS(errors("fn f() -> int { while true: { break; } }"), "can reach the end");
    CHECK(ok("fn f() -> int { exit(1); }"));
}

TEST_CASE("sema: TC placement") {
    CHECK(ok("TC: { read int n; println(n); }"));
    CHECK_CONTAINS(errors("fn f() { TC: {} }"), "TC blocks cannot appear inside functions");
    CHECK_CONTAINS(errors("TC: { TC: {} }"), "TC blocks cannot be nested");
}

TEST_CASE("sema: functions and structs only at top level") {
    CHECK_CONTAINS(errors("{ fn f() {} }"), "functions can only be declared at the top level");
    CHECK_CONTAINS(errors("{ struct S { int x; } }"), "structs can only be declared at the top level");
    CHECK_CONTAINS(errors("fn f() {} fn f() {}"), "redefinition of function 'f'");
}

TEST_CASE("sema: read and print operands") {
    CHECK(ok("int x; string s; char c; float f; int[3] a; read(x, s, c, f, a, a[0]);"));
    CHECK(ok("struct P { int x; float y; } P p; read(p); println(p);"));
    CHECK_CONTAINS(errors("bool b; read(b);"), "cannot read a value of type bool");
    CHECK_CONTAINS(errors("read(5);"), "cannot read into a temporary value");
    CHECK_CONTAINS(errors("map<int,int> m; println(m);"), "cannot print a value of type map<i64, i64>");
    CHECK_CONTAINS(errors("read int[] a;"), "reading an array needs a size");
    CHECK(ok("read int n; read int[n] a; read string s; println(n, a, s);"));
}

TEST_CASE("sema: reference parameters") {
    CHECK(ok("fn inc(int& x) { x += 1; } int a; inc(a);"));
    CHECK_CONTAINS(errors("fn inc(int& x) { x += 1; } inc(5);"), "cannot pass to reference parameter 'x' a temporary");
    CHECK_CONTAINS(errors("fn inc(int& x) { x += 1; } i32 a; inc(a);"), "needs a variable of type i64, found i32");
}

TEST_CASE("sema: reference arguments may not dangle") {
    // a container and one of its elements in the same call
    CHECK_CONTAINS(errors("fn g(vec<vec<int>>& all, vec<int>& a) { all.push([1]); a.push(2); } "
                          "vec<vec<int>> all = [[1]]; g(all, all[0]);"),
                   "cannot pass an element of 'all' by reference together with 'all' itself");
    CHECK_CONTAINS(errors("fn f(map<int, vec<int>>& m, vec<int>& e) { m.erase(1); e.push(5); } "
                          "map<int, vec<int>> m; f(m, m[1]);"),
                   "cannot pass an element of 'm' by reference");
    CHECK_CONTAINS(errors("fn f(vec<int>& row, int& x) { row.clear(); x = 1; } vec<vec<int>> g = [[1]]; f(g[0], g[0][0]);"),
                   "together with the part of 'g' that contains it");
    // an element of a global handed to a function that modifies that global
    CHECK_CONTAINS(errors("vec<vec<int>> GV = [[1], [2]]; fn h(vec<int>& e) -> int { GV = [[5]]; e.push(4); return 1; } "
                          "println(h(GV[1]));"),
                   "cannot pass an element of global 'GV' by reference to 'h', which modifies 'GV'");
    // ... also when the element is taken from a reference parameter that aliases the global
    CHECK_CONTAINS(errors("vec<vec<int>> GV = [[1], [2]]; fn h(vec<int>& e) { GV = [[5]]; e.push(4); } "
                          "fn k(vec<vec<int>>& p) { h(p[1]); } k(GV);"),
                   "cannot pass global 'GV' by reference to 'k'");
    // ... or when two reference parameters (or a parameter and a global) may be the same object
    const std::string g = "fn g(vec<vec<int>>& c, vec<int>& e) { c.clear(); e.push(5); } ";
    CHECK_CONTAINS(errors(g + "fn f(vec<vec<int>>& a, vec<vec<int>>& b) { g(a, b[0]); } vec<vec<int>> v = [[1]]; f(v, v);"),
                   "'a' and 'b' may be the same object");
    CHECK_CONTAINS(errors("vec<vec<int>> G = [[1]]; " + g + "fn f(vec<vec<int>>& a) { g(a, G[0]); } f(G);"),
                   "'a' and 'G' may be the same object");
    CHECK_CONTAINS(errors("vec<vec<int>> G = [[1]]; " + g + "fn f(vec<vec<int>>& a) { g(G, a[0]); } f(G);"),
                   "'G' and 'a' may be the same object");
    CHECK_CONTAINS(errors("struct X { vec<vec<int>> rows; } fn g(X& c, vec<int>& e) { c.rows.clear(); e.push(1); } "
                          "fn f(X& a, X& b) { g(a, b.rows[0]); }"),
                   "may be the same object");
    CHECK_CONTAINS(errors("struct Node { int v; vec<Node> kids; vec<int> vals; } "
                          "fn g(Node& c, vec<int>& e) { c.kids.clear(); e.push(1); } "
                          "fn f(Node& a, Node& b) { g(a, b.kids[0].vals); }"),
                   "may be the same object");
    // recursive structs (struct -> vec<struct> -> struct) terminate
    CHECK_CONTAINS(errors("struct Node { vec<Node> kids; vec<int> vals; } fn g(Node& n, int& e) { e = 1; } "
                          "fn f(Node& a, Node& b) { g(a, b.vals[0]); }"),
                   "may be the same object");
    CHECK_CONTAINS(errors("struct A { vec<B> bs; string s; } struct B { vec<A> as; } A GA; "
                          "fn g(A& a, char& c) { c = 'z'; } fn f(A& p) { g(GA, p.s[0]); }"),
                   "may be the same object");
    // unrelated parts of possibly-aliased parameters are fine
    CHECK(ok("struct X { string name; vec<vec<int>> rows; } fn g(string& s, vec<int>& e) { s = \"\"; e.push(1); } "
             "fn f(X& a, X& b) { g(a.name, b.rows[0]); }"));
    CHECK(ok("fn g(vec<int>& a, int& x) { a.push(1); x = 2; } fn f(vec<int>& p, vec<vec<int>>& q) { g(p, q[0][0]); }"));
    // all of these are fine
    CHECK(ok("fn swap(int& a, int& b) { int t = a; a = b; b = t; } int[3] a; swap(a[0], a[2]); swap(a[1], a[1]);"));
    CHECK(ok("fn inc(int& x) { x += 1; } int[3] cnt; int total; fn bump() { inc(cnt[1]); total += 1; } bump();"));
    CHECK(ok("fn f(vec<int>& a, vec<int>& b) { a.push(1); b.push(2); } vec<vec<int>> g = [[1], [2]]; vec<int> v; "
             "f(g[0], g[1]); f(v, v); f(v, g[0]);"));
    CHECK(ok("struct S { vec<int> a, b; } fn f(S& s, vec<int>& x) { s.a = [1]; x.push(1); } S s; f(s, s.b);"));
    CHECK(ok("vec<vec<int>> GV = [[1]]; int counter; fn h(vec<int>& e) { counter += 1; e.push(1); } h(GV[0]);"));
    CHECK(ok("vec<vec<int>> GV = [[1]]; fn h(vec<vec<int>>& all) { all = [[2]]; } h(GV);"));
}

TEST_CASE("sema: min / max follow the arithmetic promotion rules for mixed types") {
    CHECK_EQ(last("i8 a; u8 b; max(a, b);"), std::string("(call max a:i8 b:u8):i64"));
    CHECK_EQ(last("i32 a; min(a, 5);"), std::string("(call min a:i32 5:i32):i32"));
    CHECK_EQ(last("u64 a; int b; max(a, b);"), std::string("(call max a:u64 b:i64):u64"));
    CHECK_EQ(last("char a; int b; max(a, b);"), std::string("(call max a:char b:i64):i64"));
    CHECK_EQ(last("u64 q; max(q, -1);"), std::string("(call max q:u64 (- 1:i64):i64):u64")); // like q < -1
}

// ---- 3.5 dp functions & purity ----

TEST_CASE("sema: the dp fib example type-checks") {
    const std::string src = R"(dp fn fib(i32 n) -> i32 {
    if n <= 1:
        return n;
    else: {
        i32 a = fib(n - 1), b = fib(n - 2);
        return a + b;
    }
}
println(fib(10));)";
    const auto r = analyze(src);
    CHECK_EQ(r.error_count, 0u);
    CHECK_EQ(r.dp_resets.size(), 1u);
    CHECK(!r.dp_resets[0].second); // pure: cache kept across testcases
}

TEST_CASE("sema: dp signature restrictions") {
    CHECK_CONTAINS(errors("dp fn f(string s) -> int { return 1; }"), "parameters of dp function 'f' must be integers");
    CHECK_CONTAINS(errors("dp fn f(int& x) -> int { return 1; }"), "cannot be references");
    CHECK_CONTAINS(errors("dp fn f(int x) { }"), "must return a scalar value");
    CHECK_CONTAINS(errors("dp fn f(int x) -> vec<int> { vec<int> v; return v; }"), "must return a scalar value");
    CHECK(ok("dp fn f(int a, char b, bool c, u8 d) -> float { return 1.5; }"));
}

TEST_CASE("sema: dp functions may not write globals or do I/O") {
    CHECK_CONTAINS(errors("int g; dp fn f(int x) -> int { g = x; return x; }"),
                   "dp function 'f' must not modify global variables");
    CHECK_CONTAINS(errors("vec<int> g; dp fn f(int x) -> int { g.push(x); return x; }"), "must not modify global");
    CHECK_CONTAINS(errors("dp fn f(int x) -> int { println(x); return x; }"), "must not perform input/output");
    // transitively through a helper
    CHECK_CONTAINS(errors("int g; fn h() { g += 1; } dp fn f(int x) -> int { h(); return x; }"),
                   "must not modify global");
    CHECK_CONTAINS(errors("fn h() { println(1); } dp fn f(int x) -> int { h(); return x; }"),
                   "must not perform input/output");
    // passing a global to a reference parameter the callee writes through
    CHECK_CONTAINS(errors("int g; fn inc(int& v) { v += 1; } dp fn f(int x) -> int { inc(g); return x; }"),
                   "must not modify global");
}

TEST_CASE("sema: reading a global map in a dp function is a read, writing it is a write") {
    const auto reads = analyze("map<int, int> g; dp fn f(int n) -> int { return g[n] + g[n + 1]; }");
    CHECK_EQ(reads.error_count, 0u);
    CHECK(reads.dp_resets[0].second);
    CHECK_CONTAINS(errors("map<int, int> g; dp fn f(int n) -> int { g[n] = 1; return n; }"), "must not modify global");
    CHECK_CONTAINS(errors("map<int, int> g; dp fn f(int n) -> int { g[n]++; return n; }"), "must not modify global");
    // taking g[n] by reference inserts n, so it is a write too
    CHECK_CONTAINS(errors("map<int, int> g; fn rd(int& x) -> int { return x; } dp fn f(int n) -> int { return rd(g[n]); }"),
                   "must not modify global");
}

TEST_CASE("sema: dp caches reset per testcase only when globals are read") {
    const auto pure = analyze("dp fn f(int x) -> int { if x < 2: return x; return f(x - 1) + f(x - 2); }");
    CHECK_EQ(pure.error_count, 0u);
    CHECK(!pure.dp_resets[0].second);

    const auto reads = analyze("int[5] a; dp fn f(int i) -> int { if i < 0: return 0; return a[i] + f(i - 1); }");
    CHECK_EQ(reads.error_count, 0u);
    CHECK(reads.dp_resets[0].second);

    // reading through a read-only reference helper is fine and also counts as a read
    const auto via_ref = analyze("int[5] a; fn get(int[]& arr, int i) -> int { return arr[i]; } "
                                 "dp fn f(int i) -> int { return get(a, i); }");
    CHECK_EQ(via_ref.error_count, 0u);
    CHECK(via_ref.dp_resets[0].second);

    // locals of a dp function may be mutated freely
    CHECK(ok("dp fn f(int n) -> int { vec<int> v; v.push(n); int s = 0; for x in v: s += x; return s; }"));
}
