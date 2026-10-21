#ifndef VULP_LEXER_HPP
#define VULP_LEXER_HPP

#include <string>
#include <vector>
#include "lexer/Token.hpp"
#include "misc/Diagnostics.hpp"

namespace Lexer {
    // Tokenizes the whole source. Errors are reported to `diag`; lexing continues after an
    // error so that several problems can be reported at once. The result always ends with
    // an END_OF_FILE token.
    std::vector<Token> tokenize(const std::string& source, Diagnostics& diag);

    // "i21" -> (21, true), "u8" -> (8, false). Returns false if `word` is not an iN/uN type name.
    bool parse_int_type_name(const std::string& word, int& bits, bool& is_signed);
}

#endif // VULP_LEXER_HPP
