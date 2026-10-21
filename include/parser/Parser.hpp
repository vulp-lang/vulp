#ifndef VULP_PARSER_HPP
#define VULP_PARSER_HPP

#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "ast/Ast.hpp"
#include "lexer/Token.hpp"
#include "misc/Diagnostics.hpp"

class ParseException : public std::runtime_error {
public:
    SourceLoc loc;
    ParseException(const SourceLoc l, const std::string& msg) : std::runtime_error(msg), loc(l) {}
};

class Parser {
private:
    std::vector<Token> tokens;
    size_t current = 0;
    Diagnostics& diag;
    std::set<std::string> struct_names; // pre-scanned so `Point p;` parses as a declaration
    AST::Program program;
    int error_count = 0;

    // Bound on expression / statement nesting, so absurd inputs get an error message instead of
    // exhausting the compiler's stack.
    static constexpr int MAX_NESTING = 100000;
    int nesting = 0;
    struct NestingGuard {
        Parser& parser;
        NestingGuard(Parser& p, SourceLoc loc);
        ~NestingGuard() { parser.nesting--; }
    };

    // token helpers
    [[nodiscard]] const Token& peek(size_t ahead = 0) const;
    [[nodiscard]] const Token& previous_token() const;
    const Token& advance();
    [[nodiscard]] bool check(TokenType type) const;
    [[nodiscard]] bool check_next(TokenType type) const;
    [[nodiscard]] bool is_at_end() const;
    bool match(std::initializer_list<TokenType> types);
    bool match(TokenType type);
    const Token& consume(TokenType type, const std::string& msg);
    void consume_closing_angle();
    [[noreturn]] void fail(const std::string& msg) const;
    [[noreturn]] void fail_expected(const std::string& msg) const;
    void synchronize();
    void report(const ParseException& e);

    // expressions (Pratt parsing)
    static int prefix_bp(TokenType type);
    static std::pair<int, int> infix_bp(TokenType type);
    AST::ExprPtr parse_expr(int min_bp = 0);
    AST::ExprPtr parse_primary();
    std::vector<AST::ExprPtr> parse_call_args();

    // types
    [[nodiscard]] bool starts_declaration() const;
    AST::TypeSpec parse_type(bool allow_sizes, bool allow_ref);
    AST::TypeSpec parse_base_type();
    std::string parse_signed_int_literal();

    // statements
    AST::StmtPtr parse_statement();
    AST::StmtPtr parse_block_after_brace(SourceLoc loc);
    AST::StmtPtr parse_block_required(const std::string& what);
    AST::StmtPtr parse_var_decl(bool from_read);
    AST::StmtPtr parse_read();
    AST::StmtPtr parse_print(bool newline);
    AST::StmtPtr parse_if();
    AST::StmtPtr parse_while();
    AST::StmtPtr parse_for();
    AST::StmtPtr parse_tc();
    AST::StmtPtr parse_return();
    AST::StmtPtr parse_fn(bool is_dp, SourceLoc loc);
    AST::StmtPtr parse_struct();
    AST::StmtPtr parse_body(const std::string& construct);

public:
    Parser(std::vector<Token> p_tokens, Diagnostics& p_diag);
    void parse();
    [[nodiscard]] bool has_errors() const { return error_count > 0; }
    AST::Program take_program();
};

#endif // VULP_PARSER_HPP
