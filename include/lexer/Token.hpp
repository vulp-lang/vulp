#ifndef VULP_TOKEN_HPP
#define VULP_TOKEN_HPP

#include "lexer/TokenType.hpp"
#include "misc/Diagnostics.hpp"
#include <string>
#include <utility>

class Token {
public:
    TokenType type;
    // For literals this is the *value*: digits without '_' separators, the decoded
    // string body, or the decoded char. For everything else it is the source spelling.
    std::string lexeme;
    int line, column;

    Token(const TokenType type, std::string lexeme, const int line, const int column)
        : type(type), lexeme(std::move(lexeme)), line(line), column(column) {}

    [[nodiscard]] SourceLoc loc() const { return {line, column}; }

    [[nodiscard]] std::string to_string() const {
        return std::string("<") + token_type_to_string(type) + ", \"" + lexeme + "\", " + std::to_string(line)
               + ":" + std::to_string(column) + ">";
    }
};

#endif // VULP_TOKEN_HPP
