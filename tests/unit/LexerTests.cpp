#include "TestFramework.hpp"
#include "lexer/Lexer.hpp"

namespace {
    // Renders tokens as "type:lexeme" separated by spaces (EOF omitted).
    std::string lex(const std::string& src, std::string* errors = nullptr) {
        Diagnostics diag("test.vp", src);
        const auto tokens = Lexer::tokenize(src, diag);
        if (errors) *errors = diag.format();
        std::string out;
        for (const auto& t : tokens) {
            if (t.type == TokenType::END_OF_FILE) break;
            if (!out.empty()) out += ' ';
            out += token_type_to_string(t.type);
            if (t.type == TokenType::IDENTIFIER || t.type == TokenType::INT_LITERAL
                || t.type == TokenType::FLOAT_LITERAL || t.type == TokenType::STRING_LITERAL
                || t.type == TokenType::CHAR_LITERAL || t.type == TokenType::INT_N) {
                out += ":" + t.lexeme;
            }
        }
        return out;
    }

    bool lexes_cleanly(const std::string& src) {
        Diagnostics diag("test.vp", src);
        Lexer::tokenize(src, diag);
        return !diag.has_errors();
    }
} // namespace

// ---- 1.1 tokens, locations, diagnostics ----

TEST_CASE("lexer: empty input yields only EOF") {
    Diagnostics diag("t.vp", "");
    const auto tokens = Lexer::tokenize("", diag);
    CHECK_EQ(tokens.size(), 1u);
    CHECK(tokens[0].type == TokenType::END_OF_FILE);
    CHECK(!diag.has_errors());
}

TEST_CASE("lexer: 1-based line and column tracking") {
    const std::string src = "x\n  y = 1;\n";
    Diagnostics diag("t.vp", src);
    const auto t = Lexer::tokenize(src, diag);
    CHECK_EQ(t[0].line, 1);
    CHECK_EQ(t[0].column, 1);
    CHECK_EQ(t[1].lexeme, std::string("y"));
    CHECK_EQ(t[1].line, 2);
    CHECK_EQ(t[1].column, 3);
    CHECK_EQ(t[2].column, 5); // '='
    CHECK_EQ(t[3].column, 7); // 1
}

TEST_CASE("diagnostics: caret points at the error column") {
    const std::string src = "int x = 1;\nx = $;\n";
    Diagnostics diag("prog.vp", src);
    Lexer::tokenize(src, diag);
    CHECK_EQ(diag.count(), 1u);
    const std::string text = diag.format();
    CHECK_CONTAINS(text, "prog.vp:2:5: lexer error: unexpected character '$'");
    CHECK_CONTAINS(text, "    x = $;\n        ^\n");
}

TEST_CASE("diagnostics: very long lines are clipped around the error") {
    const std::string src = "int x = " + std::string(500, '1') + " $ " + std::string(500, '2') + ";";
    Diagnostics diag("long.vp", src);
    Lexer::tokenize(src, diag);
    const std::string text = diag.format();
    CHECK_CONTAINS(text, "long.vp:1:510: lexer error: unexpected character '$'");
    CHECK(text.size() < 400);
    CHECK_CONTAINS(text, "    ...1111");
    CHECK_CONTAINS(text, "1111 $ 2222");
    CHECK_CONTAINS(text, "2222...\n");
    // the caret still sits under the '$'
    const size_t line_start = text.find("    ...");
    const size_t caret_line = text.find('\n', line_start) + 1;
    CHECK_EQ(text.find('$', line_start) - line_start, text.find('^', caret_line) - caret_line);
}

// ---- 1.2 operators & punctuation ----

TEST_CASE("lexer: single-character punctuation") {
    CHECK_EQ(lex("{ } [ ] ( ) ; , : ."), std::string("{ } [ ] ( ) ; , : ."));
}

TEST_CASE("lexer: arithmetic, comparison and logical operators") {
    CHECK_EQ(lex("+ - * / % == != < <= > >= && || !"), std::string("+ - * / % == != < <= > >= && || !"));
}

TEST_CASE("lexer: bitwise operators and shifts") {
    CHECK_EQ(lex("& | ^ ~ << >>"), std::string("& | ^ ~ << >>"));
}

TEST_CASE("lexer: compound assignment and increment operators") {
    CHECK_EQ(lex("= += -= *= /= %= &= |= ^= <<= >>= ++ --"),
             std::string("= += -= *= /= %= &= |= ^= <<= >>= ++ --"));
}

TEST_CASE("lexer: arrow and range dots") {
    CHECK_EQ(lex("-> .."), std::string("-> .."));
    CHECK_EQ(lex("a->b"), std::string("identifier:a -> identifier:b"));
}

TEST_CASE("lexer: maximal munch without spaces") {
    CHECK_EQ(lex("i++<=j"), std::string("identifier:i ++ <= identifier:j"));
    CHECK_EQ(lex("a<<=2"), std::string("identifier:a <<= int-literal:2"));
    CHECK_EQ(lex("x>>y"), std::string("identifier:x >> identifier:y"));
}

// ---- 1.3 literals & comments ----

TEST_CASE("lexer: integer literals with separators and hex") {
    CHECK_EQ(lex("0 42 1_000_000 0xFF 0x_dead_beef"),
             std::string("int-literal:0 int-literal:42 int-literal:1000000 int-literal:0xFF int-literal:0xdeadbeef"));
}

TEST_CASE("lexer: float literals") {
    CHECK_EQ(lex("3.14 1e9 2.5e-3 1E+2"),
             std::string("float-literal:3.14 float-literal:1e9 float-literal:2.5e-3 float-literal:1E+2"));
}

TEST_CASE("lexer: range literal is not a float") {
    CHECK_EQ(lex("[1..10]"), std::string("[ int-literal:1 .. int-literal:10 ]"));
    CHECK_EQ(lex("[a..b]"), std::string("[ identifier:a .. identifier:b ]"));
}

TEST_CASE("lexer: string literals and escapes") {
    CHECK_EQ(lex("\"hello world\""), std::string("string-literal:hello world"));
    Diagnostics diag("t.vp", "\"a\\nb\\t\\\"q\\\"\\\\\"");
    const auto t = Lexer::tokenize("\"a\\nb\\t\\\"q\\\"\\\\\"", diag);
    CHECK(!diag.has_errors());
    CHECK_EQ(t[0].lexeme, std::string("a\nb\t\"q\"\\"));
}

TEST_CASE("lexer: char literals") {
    Diagnostics diag("t.vp", "'a' '\\n' '\\''");
    const auto t = Lexer::tokenize("'a' '\\n' '\\''", diag);
    CHECK(!diag.has_errors());
    CHECK(t[0].type == TokenType::CHAR_LITERAL);
    CHECK_EQ(t[0].lexeme, std::string("a"));
    CHECK_EQ(t[1].lexeme, std::string("\n"));
    CHECK_EQ(t[2].lexeme, std::string("'"));
}

TEST_CASE("lexer: comments run to end of line") {
    CHECK_EQ(lex("x # comment ; ignored\ny"), std::string("identifier:x identifier:y"));
    CHECK_EQ(lex("# only a comment"), std::string(""));
}

TEST_CASE("lexer: literal errors are reported") {
    std::string errors;
    lex("\"never closed", &errors);
    CHECK_CONTAINS(errors, "unterminated string literal");
    lex("'ab'", &errors);
    CHECK_CONTAINS(errors, "exactly one character");
    lex("''", &errors);
    CHECK_CONTAINS(errors, "empty or unterminated char literal");
    lex("12abc", &errors);
    CHECK_CONTAINS(errors, "invalid character in number literal");
    lex("\"bad \\q escape\"", &errors);
    CHECK_CONTAINS(errors, "unknown escape sequence");
}

TEST_CASE("lexer: continues after an error and reports several") {
    Diagnostics diag("t.vp", "$ x @ y");
    const auto t = Lexer::tokenize("$ x @ y", diag);
    CHECK_EQ(diag.count(), 2u);
    CHECK_EQ(t.size(), 3u); // x y EOF
}

// ---- 1.4 identifiers & keywords ----

TEST_CASE("lexer: keywords") {
    CHECK_EQ(lex("fn struct if elif else while for in return break continue TC read print println true false"),
             std::string("fn struct if elif else while for in return break continue TC read print println true false"));
    CHECK_EQ(lex("dp"), std::string("identifier:dp")); // contextual: only special in `dp fn`
}

TEST_CASE("lexer: type keywords") {
    CHECK_EQ(lex("int char bool float string vec map"), std::string("int char bool float string vec map"));
}

TEST_CASE("lexer: iN / uN integer type names") {
    CHECK_EQ(lex("i8 i21 u3 i256 u64 i1"),
             std::string("int-n:i8 int-n:i21 int-n:u3 int-n:i256 int-n:u64 int-n:i1"));
    // out of range or not canonical -> plain identifiers
    CHECK_EQ(lex("i257 i0 u08 i x1 ix"),
             std::string("identifier:i257 identifier:i0 identifier:u08 identifier:i identifier:x1 identifier:ix"));
}

TEST_CASE("lexer: parse_int_type_name") {
    int bits = 0;
    bool is_signed = false;
    CHECK(Lexer::parse_int_type_name("i21", bits, is_signed));
    CHECK_EQ(bits, 21);
    CHECK(is_signed);
    CHECK(Lexer::parse_int_type_name("u256", bits, is_signed));
    CHECK_EQ(bits, 256);
    CHECK(!is_signed);
    CHECK(!Lexer::parse_int_type_name("int", bits, is_signed));
    CHECK(!Lexer::parse_int_type_name("i300", bits, is_signed));
}

TEST_CASE("lexer: TC is case sensitive") {
    CHECK_EQ(lex("TC tc Tc"), std::string("TC identifier:tc identifier:Tc"));
}

TEST_CASE("lexer: identifiers with underscores and digits") {
    CHECK_EQ(lex("_a b_2 dp_table"), std::string("identifier:_a identifier:b_2 identifier:dp_table"));
}

TEST_CASE("lexer: the fib example from the task") {
    const std::string src = R"(dp fn fib(i32 n) -> i32 { # cached
    if n <= 1:
        return n;
    else: {
        i32 a = fib(n - 1), b = fib(n - 2);
        return a + b;
    }
})";
    CHECK(lexes_cleanly(src));
    CHECK_EQ(lex("dp fn fib(i32 n) -> i32 {"),
             std::string("identifier:dp fn identifier:fib ( int-n:i32 identifier:n ) -> int-n:i32 {"));
}
