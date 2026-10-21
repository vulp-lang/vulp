#include "parser/Parser.hpp"

#include "lexer/Lexer.hpp"

using namespace AST;

Parser::NestingGuard::NestingGuard(Parser& p, const SourceLoc loc) : parser(p) {
    if (parser.nesting >= MAX_NESTING) {
        throw ParseException(loc, "program is nested too deeply (more than "
                                      + std::to_string(MAX_NESTING) + " levels)");
    }
    parser.nesting++;
}

Parser::Parser(std::vector<Token> p_tokens, Diagnostics& p_diag) : tokens(std::move(p_tokens)), diag(p_diag) {
    if (tokens.empty() || tokens.back().type != TokenType::END_OF_FILE) {
        const int line = tokens.empty() ? 1 : tokens.back().line;
        tokens.emplace_back(TokenType::END_OF_FILE, "", line, 1);
    }
    // Struct names may be used as types before their declaration, so collect them up front.
    for (size_t i = 0; i + 1 < tokens.size(); i++) {
        if (tokens[i].type == TokenType::STRUCT && tokens[i + 1].type == TokenType::IDENTIFIER) {
            struct_names.insert(tokens[i + 1].lexeme);
        }
    }
}

AST::Program Parser::take_program() {
    return std::move(program);
}

void Parser::parse() {
    while (!is_at_end()) {
        const size_t start = current;
        try {
            if (auto stmt = parse_statement()) program.items.push_back(std::move(stmt));
        } catch (const ParseException& e) {
            report(e);
            synchronize();
            if (current == start) advance();
        }
    }
}

/*** token helpers ***/

const Token& Parser::peek(const size_t ahead) const {
    const size_t idx = std::min(current + ahead, tokens.size() - 1);
    return tokens[idx];
}

const Token& Parser::previous_token() const {
    return tokens[current == 0 ? 0 : current - 1];
}

const Token& Parser::advance() {
    if (!is_at_end()) current++;
    return previous_token();
}

bool Parser::check(const TokenType type) const {
    return peek().type == type;
}

bool Parser::check_next(const TokenType type) const {
    return peek(1).type == type;
}

bool Parser::is_at_end() const {
    return check(TokenType::END_OF_FILE);
}

bool Parser::match(const std::initializer_list<TokenType> types) {
    for (const auto type : types) {
        if (check(type)) {
            advance();
            return true;
        }
    }
    return false;
}

bool Parser::match(const TokenType type) {
    return match({type});
}

void Parser::fail(const std::string& msg) const {
    throw ParseException(peek().loc(), msg);
}

// "expected X" errors: when the offending token is on a later line than the previous
// token (typically a missing ';' at the end of a line), point just past the previous token.
void Parser::fail_expected(const std::string& msg) const {
    if (current > 0 && peek().line > previous_token().line) {
        const Token& prev = previous_token();
        const int len = std::max<int>(1, static_cast<int>(prev.lexeme.size()));
        throw ParseException({prev.line, prev.column + len}, msg);
    }
    std::string found = peek().type == TokenType::END_OF_FILE ? "end of file" : "'" + peek().lexeme + "'";
    throw ParseException(peek().loc(), msg + ", found " + found);
}

const Token& Parser::consume(const TokenType type, const std::string& msg) {
    if (check(type)) return advance();
    fail_expected(msg);
}

// Consumes one '>' closing a generic argument list. A '>>' token (as in vec<vec<int>>) is split.
void Parser::consume_closing_angle() {
    if (match(TokenType::GREATER)) return;
    if (check(TokenType::SHR)) {
        Token& tok = tokens[current];
        tok.type = TokenType::GREATER;
        tok.lexeme = ">";
        tok.column += 1;
        return;
    }
    fail_expected("expected '>' to close the type argument list");
}

void Parser::report(const ParseException& e) {
    error_count++;
    diag.error("parser", e.loc, e.what());
}

// Skip to a plausible statement boundary: after a ';', or before a '}' or a statement keyword.
void Parser::synchronize() {
    while (!is_at_end()) {
        if (match(TokenType::SEMICOLON)) return;
        switch (peek().type) {
            case TokenType::RIGHT_CURLY_BRACKET:
            case TokenType::FN:
            case TokenType::STRUCT:
            case TokenType::IF:
            case TokenType::WHILE:
            case TokenType::FOR:
            case TokenType::TC:
            case TokenType::RETURN:
            case TokenType::PRINT:
            case TokenType::PRINTLN:
            case TokenType::READ:
                return;
            default:
                advance();
        }
    }
}

/*** expressions ***/

int Parser::prefix_bp(const TokenType type) {
    switch (type) {
        case TokenType::MINUS:
        case TokenType::NOT:
        case TokenType::TILDE:
            return 21;
        default:
            return -1;
    }
}

// Lowest to highest (Python-like: bitwise operators bind tighter than comparisons).
std::pair<int, int> Parser::infix_bp(const TokenType type) {
    switch (type) {
        case TokenType::EQUAL:
        case TokenType::PLUS_EQUAL:
        case TokenType::MINUS_EQUAL:
        case TokenType::STAR_EQUAL:
        case TokenType::SLASH_EQUAL:
        case TokenType::PERCENT_EQUAL:
        case TokenType::AMP_EQUAL:
        case TokenType::PIPE_EQUAL:
        case TokenType::CARET_EQUAL:
        case TokenType::SHL_EQUAL:
        case TokenType::SHR_EQUAL:
            return {2, 1}; // right associative
        case TokenType::OR_OR:
            return {3, 4};
        case TokenType::AND_AND:
            return {5, 6};
        case TokenType::EQUAL_EQUAL:
        case TokenType::NOT_EQUAL:
        case TokenType::LESS:
        case TokenType::LESS_EQUAL:
        case TokenType::GREATER:
        case TokenType::GREATER_EQUAL:
            return {7, 8};
        case TokenType::PIPE:
            return {9, 10};
        case TokenType::CARET:
            return {11, 12};
        case TokenType::AMP:
            return {13, 14};
        case TokenType::SHL:
        case TokenType::SHR:
            return {15, 16};
        case TokenType::PLUS:
        case TokenType::MINUS:
            return {17, 18};
        case TokenType::STAR:
        case TokenType::SLASH:
        case TokenType::PERCENT:
            return {19, 20};
        default:
            return {-1, -1};
    }
}

std::vector<ExprPtr> Parser::parse_call_args() {
    // '(' already consumed
    std::vector<ExprPtr> args;
    if (!check(TokenType::RIGHT_PAREN)) {
        while (true) {
            args.push_back(parse_expr(0));
            if (!match(TokenType::COMMA)) break;
        }
    }
    consume(TokenType::RIGHT_PAREN, "expected ')' to close the argument list");
    return args;
}

ExprPtr Parser::parse_primary() {
    const Token tok = peek();
    const SourceLoc loc = tok.loc();

    if (match(TokenType::INT_LITERAL)) return make_expr(IntLit{tok.lexeme, llvm::APInt()}, loc);
    if (match(TokenType::FLOAT_LITERAL)) return make_expr(FloatLit{tok.lexeme}, loc);
    if (match(TokenType::CHAR_LITERAL)) {
        return make_expr(CharLit{static_cast<unsigned char>(tok.lexeme[0])}, loc);
    }
    if (match(TokenType::STRING_LITERAL)) return make_expr(StringLit{tok.lexeme}, loc);
    if (match(TokenType::TRUE)) return make_expr(BoolLit{true}, loc);
    if (match(TokenType::FALSE)) return make_expr(BoolLit{false}, loc);

    if (match(TokenType::IDENTIFIER)) {
        if (match(TokenType::LEFT_PAREN)) {
            CallExpr call;
            call.callee = tok.lexeme;
            call.callee_loc = loc;
            call.args = parse_call_args();
            return make_expr(std::move(call), loc);
        }
        return make_expr(NameExpr{tok.lexeme, nullptr}, loc);
    }

    // casts: int(x), i32(x), char(x), float(x), bool(x), string(x)
    if ((check(TokenType::INT) || check(TokenType::INT_N) || check(TokenType::CHAR) || check(TokenType::FLOAT)
         || check(TokenType::BOOL) || check(TokenType::STRING))
        && check_next(TokenType::LEFT_PAREN)) {
        TypeSpec target = parse_base_type();
        consume(TokenType::LEFT_PAREN, "expected '(' after type name in cast");
        ExprPtr operand = parse_expr(0);
        consume(TokenType::RIGHT_PAREN, "expected ')' to close the cast");
        return make_expr(CastExpr{std::move(target), std::move(operand)}, loc);
    }

    if (match(TokenType::LEFT_PAREN)) {
        ExprPtr inner = parse_expr(0);
        consume(TokenType::RIGHT_PAREN, "expected ')' to close the parenthesized expression");
        return inner;
    }

    if (match(TokenType::LEFT_SQUARE_BRACKET)) {
        if (match(TokenType::RIGHT_SQUARE_BRACKET)) return make_expr(ArrayLit{}, loc);
        ExprPtr first = parse_expr(0);
        if (match(TokenType::DOUBLE_DOT)) {
            ExprPtr hi = parse_expr(0);
            consume(TokenType::RIGHT_SQUARE_BRACKET, "expected ']' to close the range literal");
            return make_expr(RangeLit{std::move(first), std::move(hi)}, loc);
        }
        ArrayLit arr;
        arr.elems.push_back(std::move(first));
        while (match(TokenType::COMMA)) {
            if (check(TokenType::RIGHT_SQUARE_BRACKET)) break; // trailing comma
            arr.elems.push_back(parse_expr(0));
        }
        consume(TokenType::RIGHT_SQUARE_BRACKET, "expected ']' to close the array literal");
        return make_expr(std::move(arr), loc);
    }

    if (const int bp = prefix_bp(tok.type); bp > 0) {
        advance();
        ExprPtr operand = parse_expr(bp);
        return make_expr(UnaryExpr{tok.type, std::move(operand)}, loc);
    }

    if (tok.type == TokenType::END_OF_FILE) fail("expected an expression, found end of file");
    fail("expected an expression, found '" + tok.lexeme + "'");
}

ExprPtr Parser::parse_expr(const int min_bp) {
    const NestingGuard guard(*this, peek().loc());
    ExprPtr lhs = parse_primary();
    int chain = 0; // every operator applied below makes the expression tree one level deeper

    while (true) {
        const Token& tok = peek();
        const SourceLoc loc = tok.loc();
        if (nesting + chain >= MAX_NESTING) {
            throw ParseException(loc, "expression is nested too deeply (more than "
                                          + std::to_string(MAX_NESTING) + " levels)");
        }
        chain++;

        // postfix operators bind tightest
        if (match(TokenType::LEFT_SQUARE_BRACKET)) {
            ExprPtr index = parse_expr(0);
            consume(TokenType::RIGHT_SQUARE_BRACKET, "expected ']' after index");
            const SourceLoc base_loc = lhs->loc;
            lhs = make_expr(IndexExpr{std::move(lhs), std::move(index)}, base_loc);
            continue;
        }
        if (match(TokenType::DOT)) {
            const Token& name = consume(TokenType::IDENTIFIER, "expected a field or method name after '.'");
            const std::string member = name.lexeme;
            const SourceLoc member_loc = name.loc();
            if (match(TokenType::LEFT_PAREN)) {
                MethodCallExpr call;
                call.base = std::move(lhs);
                call.method = member;
                call.args = parse_call_args();
                lhs = make_expr(std::move(call), member_loc);
            } else {
                lhs = make_expr(FieldExpr{std::move(lhs), member, -1}, member_loc);
            }
            continue;
        }
        if (check(TokenType::PLUS_PLUS) || check(TokenType::MINUS_MINUS)) {
            const TokenType op = advance().type;
            const SourceLoc target_loc = lhs->loc;
            lhs = make_expr(IncDecExpr{op, std::move(lhs)}, target_loc);
            continue;
        }
        if (check(TokenType::LEFT_PAREN)) fail("only named functions can be called");

        const auto [lbp, rbp] = infix_bp(tok.type);
        if (lbp < 0 || lbp < min_bp) break;
        const TokenType op = advance().type;
        ExprPtr rhs = parse_expr(rbp);

        if (op == TokenType::EQUAL || (lbp == 2 && rbp == 1)) {
            lhs = make_expr(AssignExpr{op, std::move(lhs), std::move(rhs), nullptr}, loc);
        } else {
            lhs = make_expr(BinaryExpr{op, std::move(lhs), std::move(rhs), nullptr}, loc);
        }
    }
    return lhs;
}

/*** types ***/

bool Parser::starts_declaration() const {
    const TokenType t = peek().type;
    if (is_type_keyword(t)) return !check_next(TokenType::LEFT_PAREN); // `int(x)` is a cast
    if (t == TokenType::IDENTIFIER) {
        if (check_next(TokenType::IDENTIFIER)) return true; // `Point p` (or an unknown type name)
        if (struct_names.contains(peek().lexeme) && check_next(TokenType::LEFT_SQUARE_BRACKET)) return true;
    }
    return false;
}

std::string Parser::parse_signed_int_literal() {
    std::string sign = match(TokenType::MINUS) ? "-" : "";
    const Token& num = consume(TokenType::INT_LITERAL, "expected an integer literal as range bound");
    return sign + num.lexeme;
}

TypeSpec Parser::parse_base_type() {
    TypeSpec t;
    const Token tok = peek();
    t.loc = tok.loc();
    switch (tok.type) {
        case TokenType::INT:
            advance();
            if (match(TokenType::LESS)) { // int<lo..hi>
                t.kind = TypeSpec::Kind::RangeInt;
                t.lo = parse_signed_int_literal();
                consume(TokenType::DOUBLE_DOT, "expected '..' in range type int<lo..hi>");
                t.hi = parse_signed_int_literal();
                consume_closing_angle();
            } else {
                t.kind = TypeSpec::Kind::Int;
            }
            return t;
        case TokenType::INT_N:
            advance();
            t.kind = TypeSpec::Kind::IntN;
            Lexer::parse_int_type_name(tok.lexeme, t.bits, t.is_signed);
            return t;
        case TokenType::CHAR: advance(); t.kind = TypeSpec::Kind::Char; return t;
        case TokenType::BOOL: advance(); t.kind = TypeSpec::Kind::Bool; return t;
        case TokenType::FLOAT: advance(); t.kind = TypeSpec::Kind::Float; return t;
        case TokenType::STRING: advance(); t.kind = TypeSpec::Kind::String; return t;
        case TokenType::VEC:
            advance();
            t.kind = TypeSpec::Kind::Vec;
            consume(TokenType::LESS, "expected '<' after vec");
            t.args.push_back(parse_type(false, false));
            consume_closing_angle();
            return t;
        case TokenType::MAP:
            advance();
            t.kind = TypeSpec::Kind::Map;
            consume(TokenType::LESS, "expected '<' after map");
            t.args.push_back(parse_type(false, false));
            consume(TokenType::COMMA, "expected ',' between map key and value types");
            t.args.push_back(parse_type(false, false));
            consume_closing_angle();
            return t;
        case TokenType::IDENTIFIER:
            advance();
            t.kind = TypeSpec::Kind::Named;
            t.name = tok.lexeme;
            return t;
        default:
            fail_expected("expected a type");
    }
}

TypeSpec Parser::parse_type(const bool allow_sizes, const bool allow_ref) {
    TypeSpec t = parse_base_type();
    while (check(TokenType::LEFT_SQUARE_BRACKET)) {
        const SourceLoc dim_loc = peek().loc();
        advance();
        if (match(TokenType::RIGHT_SQUARE_BRACKET)) {
            t.dims.push_back(nullptr);
            continue;
        }
        if (!allow_sizes) throw ParseException(dim_loc, "array sizes are not allowed here; use '[]'");
        t.dims.push_back(parse_expr(0));
        consume(TokenType::RIGHT_SQUARE_BRACKET, "expected ']' after array size");
    }
    if (check(TokenType::AMP)) {
        if (!allow_ref) fail("'&' reference types are only allowed for function parameters");
        advance();
        t.is_ref = true;
    }
    return t;
}

/*** statements ***/

StmtPtr Parser::parse_statement() {
    const Token tok = peek();
    const SourceLoc loc = tok.loc();
    const NestingGuard guard(*this, loc);

    switch (tok.type) {
        case TokenType::LEFT_CURLY_BRACKET:
            advance();
            return parse_block_after_brace(loc);
        case TokenType::FN:
            advance();
            return parse_fn(false, loc);
        case TokenType::IDENTIFIER:
            // `dp` is only a keyword in front of `fn`, so it stays usable as a variable name
            if (tok.lexeme == "dp" && check_next(TokenType::FN)) {
                advance();
                advance();
                return parse_fn(true, loc);
            }
            break;
        case TokenType::STRUCT:
            advance();
            return parse_struct();
        case TokenType::IF:
            advance();
            return parse_if();
        case TokenType::ELIF:
        case TokenType::ELSE:
            fail("'" + tok.lexeme + "' without a matching 'if'");
        case TokenType::WHILE:
            advance();
            return parse_while();
        case TokenType::FOR:
            advance();
            return parse_for();
        case TokenType::TC:
            advance();
            return parse_tc();
        case TokenType::SEMICOLON:
            advance();
            return nullptr; // empty statement
        default:
            break;
    }

    StmtPtr result;
    if (match(TokenType::READ)) {
        result = check(TokenType::LEFT_PAREN) ? parse_read() : parse_var_decl(true);
    } else if (match(TokenType::PRINT)) {
        result = parse_print(false);
    } else if (match(TokenType::PRINTLN)) {
        result = parse_print(true);
    } else if (match(TokenType::RETURN)) {
        result = parse_return();
    } else if (match(TokenType::BREAK)) {
        result = make_stmt(BreakStmt{}, loc);
    } else if (match(TokenType::CONTINUE)) {
        result = make_stmt(ContinueStmt{}, loc);
    } else if (starts_declaration()) {
        result = parse_var_decl(false);
    } else {
        ExprPtr expr = parse_expr(0);
        result = make_stmt(ExprStmt{std::move(expr)}, loc);
    }
    consume(TokenType::SEMICOLON, "expected ';' after statement");
    return result;
}

StmtPtr Parser::parse_block_after_brace(const SourceLoc loc) {
    BlockStmt block;
    while (!check(TokenType::RIGHT_CURLY_BRACKET) && !is_at_end()) {
        const size_t start = current;
        try {
            if (auto stmt = parse_statement()) block.stmts.push_back(std::move(stmt));
        } catch (const ParseException& e) {
            report(e);
            synchronize();
            if (current == start) advance();
        }
    }
    if (is_at_end()) throw ParseException(loc, "unclosed '{' (missing '}')");
    advance(); // '}'
    return make_stmt(std::move(block), loc);
}

StmtPtr Parser::parse_block_required(const std::string& what) {
    const SourceLoc loc = peek().loc();
    if (!match(TokenType::LEFT_CURLY_BRACKET)) fail_expected("expected '{' to start " + what);
    return parse_block_after_brace(loc);
}

// The body of if/elif/else/while/for/TC: `:` followed by a single statement (usually a block).
StmtPtr Parser::parse_body(const std::string& construct) {
    consume(TokenType::COLON, "expected ':' after " + construct);
    const SourceLoc loc = peek().loc();
    StmtPtr body = parse_statement();
    if (!body) body = make_stmt(BlockStmt{}, loc); // `while x: ;`
    if (std::holds_alternative<FnDecl>(body->node) || std::holds_alternative<StructDecl>(body->node)) {
        throw ParseException(loc, "functions and structs can only be declared at the top level");
    }
    return body;
}

StmtPtr Parser::parse_var_decl(const bool from_read) {
    const SourceLoc loc = peek().loc();
    VarDeclStmt decl;
    decl.from_read = from_read;
    decl.type = parse_type(true, false);
    while (true) {
        const Token& name = consume(TokenType::IDENTIFIER, "expected a variable name");
        Declarator d;
        d.name = name.lexeme;
        d.loc = name.loc();
        if (check(TokenType::EQUAL)) {
            if (from_read) fail("'read' declarations cannot have an initializer");
            advance();
            d.init = parse_expr(0);
        }
        decl.decls.push_back(std::move(d));
        if (!match(TokenType::COMMA)) break;
    }
    return make_stmt(std::move(decl), loc);
}

StmtPtr Parser::parse_read() {
    const SourceLoc loc = previous_token().loc();
    consume(TokenType::LEFT_PAREN, "expected '(' after read");
    ReadStmt read;
    if (!check(TokenType::RIGHT_PAREN)) {
        while (true) {
            read.targets.push_back(parse_expr(0));
            if (!match(TokenType::COMMA)) break;
        }
    }
    consume(TokenType::RIGHT_PAREN, "expected ')' to close read(...)");
    if (read.targets.empty()) throw ParseException(loc, "read() needs at least one target");
    return make_stmt(std::move(read), loc);
}

StmtPtr Parser::parse_print(const bool newline) {
    const SourceLoc loc = previous_token().loc();
    consume(TokenType::LEFT_PAREN, newline ? "expected '(' after println" : "expected '(' after print");
    PrintStmt print;
    print.newline = newline;
    if (!check(TokenType::RIGHT_PAREN)) {
        while (true) {
            print.args.push_back(parse_expr(0));
            if (!match(TokenType::COMMA)) break;
        }
    }
    consume(TokenType::RIGHT_PAREN, "expected ')' to close the print arguments");
    return make_stmt(std::move(print), loc);
}

StmtPtr Parser::parse_if() {
    const SourceLoc loc = previous_token().loc();
    ExprPtr cond = parse_expr(0);
    StmtPtr then_branch = parse_body("the if condition");
    StmtPtr else_branch;
    if (check(TokenType::ELIF)) {
        advance();
        else_branch = parse_if();
    } else if (match(TokenType::ELSE)) {
        else_branch = parse_body("else");
    }
    return make_stmt(IfStmt{std::move(cond), std::move(then_branch), std::move(else_branch)}, loc);
}

StmtPtr Parser::parse_while() {
    const SourceLoc loc = previous_token().loc();
    ExprPtr cond = parse_expr(0);
    StmtPtr body = parse_body("the while condition");
    return make_stmt(WhileStmt{std::move(cond), std::move(body)}, loc);
}

StmtPtr Parser::parse_for() {
    const SourceLoc loc = previous_token().loc();
    if (match(TokenType::LEFT_PAREN)) { // for (init; cond; step): body
        ForCStmt f;
        if (!check(TokenType::SEMICOLON)) {
            const SourceLoc init_loc = peek().loc();
            if (starts_declaration()) {
                f.init = parse_var_decl(false);
            } else {
                ExprPtr e = parse_expr(0);
                f.init = make_stmt(ExprStmt{std::move(e)}, init_loc);
            }
        }
        consume(TokenType::SEMICOLON, "expected ';' after the for-loop initializer");
        if (!check(TokenType::SEMICOLON)) f.cond = parse_expr(0);
        consume(TokenType::SEMICOLON, "expected ';' after the for-loop condition");
        if (!check(TokenType::RIGHT_PAREN)) f.step = parse_expr(0);
        consume(TokenType::RIGHT_PAREN, "expected ')' to close the for-loop header");
        f.body = parse_body("the for-loop header");
        return make_stmt(std::move(f), loc);
    }

    ForInStmt f;
    const Token& var = consume(TokenType::IDENTIFIER, "expected a loop variable name or '(' after 'for'");
    f.var_name = var.lexeme;
    f.var_loc = var.loc();
    consume(TokenType::IN, "expected 'in' after the loop variable");
    f.iterable = parse_expr(0);
    f.body = parse_body("the for-in header");
    return make_stmt(std::move(f), loc);
}

StmtPtr Parser::parse_tc() {
    const SourceLoc loc = previous_token().loc();
    StmtPtr body = parse_body("TC");
    return make_stmt(TCStmt{std::move(body)}, loc);
}

StmtPtr Parser::parse_return() {
    const SourceLoc loc = previous_token().loc();
    ExprPtr value;
    if (!check(TokenType::SEMICOLON)) value = parse_expr(0);
    return make_stmt(ReturnStmt{std::move(value)}, loc);
}

StmtPtr Parser::parse_fn(const bool is_dp, const SourceLoc loc) {
    FnDecl fn;
    fn.is_dp = is_dp;
    fn.name = consume(TokenType::IDENTIFIER, "expected a function name").lexeme;
    consume(TokenType::LEFT_PAREN, "expected '(' after the function name");
    if (!check(TokenType::RIGHT_PAREN)) {
        while (true) {
            Param p;
            p.type = parse_type(false, true);
            const Token& name = consume(TokenType::IDENTIFIER, "expected a parameter name");
            p.name = name.lexeme;
            p.loc = name.loc();
            fn.params.push_back(std::move(p));
            if (!match(TokenType::COMMA)) break;
        }
    }
    consume(TokenType::RIGHT_PAREN, "expected ')' to close the parameter list");
    if (match(TokenType::ARROW)) fn.ret = parse_type(false, false);
    fn.body = parse_block_required("the function body");
    return make_stmt(std::move(fn), loc);
}

StmtPtr Parser::parse_struct() {
    const SourceLoc loc = previous_token().loc();
    StructDecl st;
    st.name = consume(TokenType::IDENTIFIER, "expected a struct name").lexeme;
    consume(TokenType::LEFT_CURLY_BRACKET, "expected '{' after the struct name");
    while (!check(TokenType::RIGHT_CURLY_BRACKET) && !is_at_end()) {
        // `int x, y;` declares two fields of the same type
        const TypeSpec type = parse_type(false, false);
        while (true) {
            const Token& name = consume(TokenType::IDENTIFIER, "expected a field name");
            st.fields.push_back(StructField{clone_type_spec(type), name.lexeme, name.loc()});
            if (!match(TokenType::COMMA)) break;
        }
        consume(TokenType::SEMICOLON, "expected ';' after the struct field");
    }
    consume(TokenType::RIGHT_CURLY_BRACKET, "expected '}' to close the struct");
    return make_stmt(std::move(st), loc);
}
