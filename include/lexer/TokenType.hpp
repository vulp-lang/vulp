#ifndef VULP_TOKENTYPE_HPP
#define VULP_TOKENTYPE_HPP

#include <string>

enum class TokenType {
    // Literals
    IDENTIFIER,
    INT_LITERAL,
    FLOAT_LITERAL,
    CHAR_LITERAL,
    STRING_LITERAL,

    // Punctuation
    LEFT_CURLY_BRACKET, RIGHT_CURLY_BRACKET,
    LEFT_SQUARE_BRACKET, RIGHT_SQUARE_BRACKET,
    LEFT_PAREN, RIGHT_PAREN,
    SEMICOLON, COMMA, COLON, DOT, DOUBLE_DOT, ARROW,

    // Arithmetic
    PLUS, MINUS, STAR, SLASH, PERCENT,
    PLUS_PLUS, MINUS_MINUS,

    // Assignment
    EQUAL,
    PLUS_EQUAL, MINUS_EQUAL, STAR_EQUAL, SLASH_EQUAL, PERCENT_EQUAL,
    AMP_EQUAL, PIPE_EQUAL, CARET_EQUAL, SHL_EQUAL, SHR_EQUAL,

    // Comparison
    EQUAL_EQUAL, NOT_EQUAL,
    LESS, LESS_EQUAL, GREATER, GREATER_EQUAL,

    // Logical & bitwise
    AND_AND, OR_OR, NOT,
    AMP, PIPE, CARET, TILDE, SHL, SHR,

    // Keywords
    FN, STRUCT, // `dp` is contextual: an identifier, special only in `dp fn`
    IF, ELIF, ELSE, WHILE, FOR, IN,
    RETURN, BREAK, CONTINUE,
    TC, READ, PRINT, PRINTLN,
    TRUE, FALSE,

    // Type keywords
    INT,     // int  (= i64)
    INT_N,   // iN / uN with 1 <= N <= 256, lexeme holds the spelling ("i21", "u8")
    CHAR, BOOL, FLOAT, STRING, VEC, MAP,

    END_OF_FILE,
};

constexpr const char* token_type_to_string(const TokenType type) {
    switch (type) {
        case TokenType::IDENTIFIER: return "identifier";
        case TokenType::INT_LITERAL: return "int-literal";
        case TokenType::FLOAT_LITERAL: return "float-literal";
        case TokenType::CHAR_LITERAL: return "char-literal";
        case TokenType::STRING_LITERAL: return "string-literal";

        case TokenType::LEFT_CURLY_BRACKET: return "{";
        case TokenType::RIGHT_CURLY_BRACKET: return "}";
        case TokenType::LEFT_SQUARE_BRACKET: return "[";
        case TokenType::RIGHT_SQUARE_BRACKET: return "]";
        case TokenType::LEFT_PAREN: return "(";
        case TokenType::RIGHT_PAREN: return ")";
        case TokenType::SEMICOLON: return ";";
        case TokenType::COMMA: return ",";
        case TokenType::COLON: return ":";
        case TokenType::DOT: return ".";
        case TokenType::DOUBLE_DOT: return "..";
        case TokenType::ARROW: return "->";

        case TokenType::PLUS: return "+";
        case TokenType::MINUS: return "-";
        case TokenType::STAR: return "*";
        case TokenType::SLASH: return "/";
        case TokenType::PERCENT: return "%";
        case TokenType::PLUS_PLUS: return "++";
        case TokenType::MINUS_MINUS: return "--";

        case TokenType::EQUAL: return "=";
        case TokenType::PLUS_EQUAL: return "+=";
        case TokenType::MINUS_EQUAL: return "-=";
        case TokenType::STAR_EQUAL: return "*=";
        case TokenType::SLASH_EQUAL: return "/=";
        case TokenType::PERCENT_EQUAL: return "%=";
        case TokenType::AMP_EQUAL: return "&=";
        case TokenType::PIPE_EQUAL: return "|=";
        case TokenType::CARET_EQUAL: return "^=";
        case TokenType::SHL_EQUAL: return "<<=";
        case TokenType::SHR_EQUAL: return ">>=";

        case TokenType::EQUAL_EQUAL: return "==";
        case TokenType::NOT_EQUAL: return "!=";
        case TokenType::LESS: return "<";
        case TokenType::LESS_EQUAL: return "<=";
        case TokenType::GREATER: return ">";
        case TokenType::GREATER_EQUAL: return ">=";

        case TokenType::AND_AND: return "&&";
        case TokenType::OR_OR: return "||";
        case TokenType::NOT: return "!";
        case TokenType::AMP: return "&";
        case TokenType::PIPE: return "|";
        case TokenType::CARET: return "^";
        case TokenType::TILDE: return "~";
        case TokenType::SHL: return "<<";
        case TokenType::SHR: return ">>";

        case TokenType::FN: return "fn";
        case TokenType::STRUCT: return "struct";
        case TokenType::IF: return "if";
        case TokenType::ELIF: return "elif";
        case TokenType::ELSE: return "else";
        case TokenType::WHILE: return "while";
        case TokenType::FOR: return "for";
        case TokenType::IN: return "in";
        case TokenType::RETURN: return "return";
        case TokenType::BREAK: return "break";
        case TokenType::CONTINUE: return "continue";
        case TokenType::TC: return "TC";
        case TokenType::READ: return "read";
        case TokenType::PRINT: return "print";
        case TokenType::PRINTLN: return "println";
        case TokenType::TRUE: return "true";
        case TokenType::FALSE: return "false";

        case TokenType::INT: return "int";
        case TokenType::INT_N: return "int-n";
        case TokenType::CHAR: return "char";
        case TokenType::BOOL: return "bool";
        case TokenType::FLOAT: return "float";
        case TokenType::STRING: return "string";
        case TokenType::VEC: return "vec";
        case TokenType::MAP: return "map";

        case TokenType::END_OF_FILE: return "end-of-file";
    }
    return "INVALID";
}

// Type keywords that may start a declaration.
constexpr bool is_type_keyword(const TokenType type) {
    switch (type) {
        case TokenType::INT:
        case TokenType::INT_N:
        case TokenType::CHAR:
        case TokenType::BOOL:
        case TokenType::FLOAT:
        case TokenType::STRING:
        case TokenType::VEC:
        case TokenType::MAP:
            return true;
        default:
            return false;
    }
}

#endif // VULP_TOKENTYPE_HPP
