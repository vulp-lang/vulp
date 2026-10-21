#include "TestFramework.hpp"
#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"

namespace {
    struct ParseResult {
        std::string ast;
        std::string errors;
        size_t error_count = 0;
    };

    ParseResult parse(const std::string& src) {
        Diagnostics diag("test.vp", src);
        auto tokens = Lexer::tokenize(src, diag);
        Parser parser(std::move(tokens), diag);
        parser.parse();
        const AST::Program program = parser.take_program();
        return {AST::print_program(program), diag.format(), diag.count()};
    }

    // Parses a single expression statement and returns its S-expression.
    std::string expr(const std::string& src) {
        const auto r = parse(src + ";");
        if (r.error_count) return "ERROR: " + r.errors;
        return r.ast.substr(0, r.ast.size() - 1); // drop trailing newline
    }

    std::string stmt(const std::string& src) {
        const auto r = parse(src);
        if (r.error_count) return "ERROR: " + r.errors;
        return r.ast.substr(0, r.ast.size() - 1);
    }
} // namespace

// ---- 2.2 expressions ----

TEST_CASE("parser: literals") {
    CHECK_EQ(expr("42"), std::string("42"));
    CHECK_EQ(expr("3.5"), std::string("3.5"));
    CHECK_EQ(expr("'a'"), std::string("'a'"));
    CHECK_EQ(expr("'\\n'"), std::string("'\\n'"));
    CHECK_EQ(expr("\"hi\\tthere\""), std::string("\"hi\\tthere\""));
    CHECK_EQ(expr("true"), std::string("true"));
    CHECK_EQ(expr("false"), std::string("false"));
}

TEST_CASE("parser: arithmetic precedence and associativity") {
    CHECK_EQ(expr("1 + 2 * 3"), std::string("(+ 1 (* 2 3))"));
    CHECK_EQ(expr("(1 + 2) * 3"), std::string("(* (+ 1 2) 3)"));
    CHECK_EQ(expr("a - b - c"), std::string("(- (- a b) c)"));
    CHECK_EQ(expr("a / b % c"), std::string("(% (/ a b) c)"));
}

TEST_CASE("parser: comparison and logical precedence") {
    CHECK_EQ(expr("a < b && c >= d || e"), std::string("(|| (&& (< a b) (>= c d)) e)"));
    CHECK_EQ(expr("a || b && c"), std::string("(|| a (&& b c))"));
    CHECK_EQ(expr("a + 1 == b * 2"), std::string("(== (+ a 1) (* b 2))"));
}

TEST_CASE("parser: bitwise operators bind tighter than comparisons") {
    CHECK_EQ(expr("x & 1 == 0"), std::string("(== (& x 1) 0)"));
    CHECK_EQ(expr("a | b ^ c & d"), std::string("(| a (^ b (& c d)))"));
    CHECK_EQ(expr("1 << n + 1"), std::string("(<< 1 (+ n 1))"));
    CHECK_EQ(expr("x >> 2 & 3"), std::string("(& (>> x 2) 3)"));
}

TEST_CASE("parser: unary operators") {
    CHECK_EQ(expr("-a * b"), std::string("(* (- a) b)"));
    CHECK_EQ(expr("!a && b"), std::string("(&& (! a) b)"));
    CHECK_EQ(expr("~x"), std::string("(~ x)"));
    CHECK_EQ(expr("- -x"), std::string("(- (- x))"));
    CHECK_EQ(expr("-x.y"), std::string("(- (. x y))"));
}

TEST_CASE("parser: assignment is right associative, compound assignments") {
    CHECK_EQ(expr("a = b = c"), std::string("(= a (= b c))"));
    CHECK_EQ(expr("x += 1"), std::string("(+= x 1)"));
    CHECK_EQ(expr("a[i] <<= k + 1"), std::string("(<<= (index a i) (+ k 1))"));
    CHECK_EQ(expr("m[k] %= 7"), std::string("(%= (index m k) 7)"));
}

TEST_CASE("parser: postfix call / index / field / method / ++") {
    CHECK_EQ(expr("f()"), std::string("(call f)"));
    CHECK_EQ(expr("f(a, b + 1)"), std::string("(call f a (+ b 1))"));
    CHECK_EQ(expr("a[i][j]"), std::string("(index (index a i) j)"));
    CHECK_EQ(expr("p.x"), std::string("(. p x)"));
    CHECK_EQ(expr("ps[i].x"), std::string("(. (index ps i) x)"));
    CHECK_EQ(expr("v.push(3)"), std::string("(method v push 3)"));
    CHECK_EQ(expr("g[u].len()"), std::string("(method (index g u) len)"));
    CHECK_EQ(expr("i++"), std::string("(post++ i)"));
    CHECK_EQ(expr("a[i]--"), std::string("(post-- (index a i))"));
}

TEST_CASE("parser: array and range literals") {
    CHECK_EQ(expr("[1, 2, 3]"), std::string("(array 1 2 3)"));
    CHECK_EQ(expr("[1, 2,]"), std::string("(array 1 2)"));
    CHECK_EQ(expr("[]"), std::string("(array)"));
    CHECK_EQ(expr("[1..10]"), std::string("(range 1 10)"));
    CHECK_EQ(expr("[0..n-1]"), std::string("(range 0 (- n 1))"));
    CHECK_EQ(expr("[[1, 2], [3]]"), std::string("(array (array 1 2) (array 3))"));
}

TEST_CASE("parser: casts with type keywords") {
    CHECK_EQ(expr("i32(x)"), std::string("(cast i32 x)"));
    CHECK_EQ(expr("int(c - 'a')"), std::string("(cast int (- c 'a'))"));
    CHECK_EQ(expr("char(65) == 'A'"), std::string("(== (cast char 65) 'A')"));
    CHECK_EQ(expr("float(n) / 2.0"), std::string("(/ (cast float n) 2.0)"));
    CHECK_EQ(expr("u8(x)"), std::string("(cast u8 x)"));
}

// ---- 2.3 types ----

TEST_CASE("parser: scalar declarations") {
    CHECK_EQ(stmt("int x = 5, y;"), std::string("(var int (x 5) (y))"));
    CHECK_EQ(stmt("char c = 'z';"), std::string("(var char (c 'z'))"));
    CHECK_EQ(stmt("float f; bool b = true; string s = \"hi\";"),
             std::string("(var float (f))\n(var bool (b true))\n(var string (s \"hi\"))"));
}

TEST_CASE("parser: arbitrary-width and range integer types") {
    CHECK_EQ(stmt("i21 a; u3 b; i256 c;"), std::string("(var i21 (a))\n(var u3 (b))\n(var i256 (c))"));
    CHECK_EQ(stmt("int<0..1000000007> x;"), std::string("(var int<0..1000000007> (x))"));
    CHECK_EQ(stmt("int<-5..5> y;"), std::string("(var int<-5..5> (y))"));
    CHECK_EQ(stmt("int<0..0xFF> z;"), std::string("(var int<0..0xFF> (z))"));
}

TEST_CASE("parser: generic container types and >> splitting") {
    CHECK_EQ(stmt("vec<int> v;"), std::string("(var vec<int> (v))"));
    CHECK_EQ(stmt("vec<vec<int>> g;"), std::string("(var vec<vec<int>> (g))"));
    CHECK_EQ(stmt("map<string, int> m;"), std::string("(var map<string,int> (m))"));
    CHECK_EQ(stmt("map<int, vec<i32>> m;"), std::string("(var map<int,vec<i32>> (m))"));
    CHECK_EQ(stmt("vec<vec<vec<char>>> cube;"), std::string("(var vec<vec<vec<char>>> (cube))"));
}

TEST_CASE("parser: array types with sizes") {
    CHECK_EQ(stmt("int[n] a;"), std::string("(var int[n] (a))"));
    CHECK_EQ(stmt("i32[n][m + 1] grid;"), std::string("(var i32[n][(+ m 1)] (grid))"));
    CHECK_EQ(stmt("int[] a = [1, 2, 3];"), std::string("(var int[] (a (array 1 2 3)))"));
    CHECK_EQ(stmt("vec<int>[n] adj;"), std::string("(var vec<int>[n] (adj))"));
    CHECK_EQ(stmt("bool[n + 1] seen;"), std::string("(var bool[(+ n 1)] (seen))"));
}

TEST_CASE("parser: struct-typed declarations") {
    const std::string src = "struct P { int x; }\nP p; P[n] ps; P q = P(1);";
    CHECK_EQ(stmt(src), std::string("(struct P (int x))\n(var P (p))\n(var P[n] (ps))\n(var P (q (call P 1)))"));
}

TEST_CASE("parser: unknown type name still parses as a declaration") {
    CHECK_EQ(stmt("Foo f;"), std::string("(var Foo (f))"));
}

// ---- 2.4 statements ----

TEST_CASE("parser: read declarations and read statements") {
    CHECK_EQ(stmt("read int n, m;"), std::string("(read-var int (n) (m))"));
    CHECK_EQ(stmt("read int[n] a;"), std::string("(read-var int[n] (a))"));
    CHECK_EQ(stmt("read string s;"), std::string("(read-var string (s))"));
    CHECK_EQ(stmt("read(a, b[i], p.x);"), std::string("(read a (index b i) (. p x))"));
}

TEST_CASE("parser: print and println") {
    CHECK_EQ(stmt("print(a, \" \", b);"), std::string("(print a \" \" b)"));
    CHECK_EQ(stmt("println();"), std::string("(println)"));
    CHECK_EQ(stmt("println(x + 1);"), std::string("(println (+ x 1))"));
}

TEST_CASE("parser: if / elif / else") {
    CHECK_EQ(stmt("if a: x = 1;"), std::string("(if a (= x 1))"));
    CHECK_EQ(stmt("if a: { x = 1; } else: x = 2;"), std::string("(if a (block (= x 1)) (= x 2))"));
    CHECK_EQ(stmt("if a: x = 1; elif b: x = 2; elif c: x = 3; else: x = 4;"),
             std::string("(if a (= x 1) (if b (= x 2) (if c (= x 3) (= x 4))))"));
}

TEST_CASE("parser: while loops") {
    CHECK_EQ(stmt("while i < n: { i += 1; }"), std::string("(while (< i n) (block (+= i 1)))"));
    CHECK_EQ(stmt("while x: ;"), std::string("(while x (block))"));
}

TEST_CASE("parser: for-in loops") {
    CHECK_EQ(stmt("for i in [1..10]: println(i);"), std::string("(for-in i (range 1 10) (println i))"));
    CHECK_EQ(stmt("for x in arr: { s += x; }"), std::string("(for-in x arr (block (+= s x)))"));
}

TEST_CASE("parser: C-style for loops") {
    CHECK_EQ(stmt("for (int i = 0; i < n; i++): s += i;"),
             std::string("(for (var int (i 0)) (< i n) (post++ i) (+= s i))"));
    CHECK_EQ(stmt("for (i = 0; i < n; i += 2): {}"), std::string("(for (= i 0) (< i n) (+= i 2) (block))"));
    CHECK_EQ(stmt("for ( ; ; ) : { break; }"), std::string("(for _ _ _ (block (break)))"));
}

TEST_CASE("parser: TC block") {
    CHECK_EQ(stmt("TC: { read int n; println(n); }"), std::string("(TC (block (read-var int (n)) (println n)))"));
}

TEST_CASE("parser: return / break / continue / blocks / empty statements") {
    CHECK_EQ(stmt("{ break; continue; ; }"), std::string("(block (break) (continue))"));
    CHECK_EQ(stmt("fn f() { return; }"), std::string("(fn f () (block (return)))"));
}

// ---- 2.5 top level ----

TEST_CASE("parser: functions") {
    CHECK_EQ(stmt("fn add(int a, int b) -> int { return a + b; }"),
             std::string("(fn add ((int a) (int b)) -> int (block (return (+ a b))))"));
    CHECK_EQ(stmt("fn show(vec<int>& v, int[] a) { println(v); }"),
             std::string("(fn show ((vec<int>& v) (int[] a)) (block (println v)))"));
}

TEST_CASE("parser: the dp fib example from the task") {
    const std::string src = R"(dp fn fib(i32 n) -> i32 { # dp added ahead of the function
    if n <= 1:
        return n;
    else: {
        i32 a = fib(n - 1), b = fib(n - 2);
        return a + b;
    }
})";
    CHECK_EQ(stmt(src), std::string("(dp-fn fib ((i32 n)) -> i32 (block (if (<= n 1) (return n) (block (var i32 (a "
                                    "(call fib (- n 1))) (b (call fib (- n 2)))) (return (+ a b))))))"));
}

TEST_CASE("parser: dp is only a keyword in front of fn") {
    CHECK_EQ(stmt("int[n][m] dp; dp[1][2] = dp[0][2] + 1;"),
             std::string("(var int[n][m] (dp))\n(= (index (index dp 1) 2) (+ (index (index dp 0) 2) 1))"));
    CHECK_EQ(stmt("dp fn f(int x) -> int { return x; }"), std::string("(dp-fn f ((int x)) -> int (block (return x)))"));
    CHECK_EQ(stmt("int dp = 3; println(dp);"), std::string("(var int (dp 3))\n(println dp)"));
}

TEST_CASE("parser: structs") {
    CHECK_EQ(stmt("struct Edge { int to, w; bool used; vec<int> tags; }"),
             std::string("(struct Edge (int to) (int w) (bool used) (vec<int> tags))"));
    CHECK_EQ(stmt("struct E {}"), std::string("(struct E)"));
}

// ---- 2.6 errors & recovery ----

TEST_CASE("parser error: missing semicolon points after the previous token") {
    const auto r = parse("int x = 5\nprintln(x);");
    CHECK_EQ(r.error_count, 1u);
    CHECK_CONTAINS(r.errors, "test.vp:1:10: parser error: expected ';' after statement");
}

TEST_CASE("parser error: missing ':' after if condition") {
    const auto r = parse("if x > 1 { y = 2; }");
    CHECK_CONTAINS(r.errors, "expected ':' after the if condition, found '{'");
}

TEST_CASE("parser error: unclosed block") {
    const auto r = parse("fn f() {\n int x = 1;\n");
    CHECK_CONTAINS(r.errors, "unclosed '{'");
}

TEST_CASE("parser error: else without if") {
    const auto r = parse("else: x = 1;");
    CHECK_CONTAINS(r.errors, "'else' without a matching 'if'");
}

TEST_CASE("parser error: sizes and refs where they are not allowed") {
    CHECK_CONTAINS(parse("fn f(int[5] a) {}").errors, "array sizes are not allowed here");
    CHECK_CONTAINS(parse("int& x;").errors, "'&' reference types are only allowed for function parameters");
    CHECK_CONTAINS(parse("read int x = 5;").errors, "'read' declarations cannot have an initializer");
}

TEST_CASE("parser error: bad expressions") {
    CHECK_CONTAINS(parse("x = ;").errors, "expected an expression, found ';'");
    CHECK_CONTAINS(parse("x = (1 + 2;").errors, "expected ')'");
    CHECK_CONTAINS(parse("x = a[1;").errors, "expected ']' after index");
    CHECK_CONTAINS(parse("(f)(1);").errors, "only named functions can be called");
}

TEST_CASE("parser error: function bodies must be blocks and structs/functions only at top level") {
    CHECK_CONTAINS(parse("fn f() return 1;").errors, "expected '{' to start the function body");
    CHECK_CONTAINS(parse("while x: fn g() {}").errors, "functions and structs can only be declared at the top level");
}

TEST_CASE("parser error: absurdly deep nesting is an error, not a crash") {
    const int n = 150000;
    CHECK_CONTAINS(parse("int x = " + std::string(n, '(') + "1" + std::string(n, ')') + ";").errors,
                   "nested too deeply");
    std::string chain = "int y = 1";
    for (int i = 0; i < n; i++) chain += " + 1";
    CHECK_CONTAINS(parse(chain + ";").errors, "nested too deeply");
    CHECK_CONTAINS(parse(std::string(n, '{') + std::string(n, '}')).errors, "nested too deeply");
    // deep but reasonable nesting is fine
    CHECK_EQ(parse("int z = " + std::string(5000, '(') + "1" + std::string(5000, ')') + ";").error_count, 0u);
}

TEST_CASE("parser error: recovery reports several independent errors") {
    const auto r = parse("int x = ;\nint y = 2;\nprintln(y;\nfn f() {\n  z = ;\n  return 1;\n}\nint ok = 1;");
    CHECK_EQ(r.error_count, 3u);
    CHECK_CONTAINS(r.ast, "(var int (y 2))");
    CHECK_CONTAINS(r.ast, "(fn f () (block (return 1)))");
    CHECK_CONTAINS(r.ast, "(var int (ok 1))");
}
