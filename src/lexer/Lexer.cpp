#include "lexer/Lexer.hpp"

#include <cctype>
#include <unordered_map>

namespace Lexer {
    namespace {
        const std::unordered_map<std::string, TokenType>& keywords() {
            static const std::unordered_map<std::string, TokenType> table = {
                {"fn", TokenType::FN},
                {"struct", TokenType::STRUCT},
                {"if", TokenType::IF},
                {"elif", TokenType::ELIF},
                {"else", TokenType::ELSE},
                {"while", TokenType::WHILE},
                {"for", TokenType::FOR},
                {"in", TokenType::IN},
                {"return", TokenType::RETURN},
                {"break", TokenType::BREAK},
                {"continue", TokenType::CONTINUE},
                {"TC", TokenType::TC},
                {"read", TokenType::READ},
                {"print", TokenType::PRINT},
                {"println", TokenType::PRINTLN},
                {"true", TokenType::TRUE},
                {"false", TokenType::FALSE},
                {"int", TokenType::INT},
                {"char", TokenType::CHAR},
                {"bool", TokenType::BOOL},
                {"float", TokenType::FLOAT},
                {"string", TokenType::STRING},
                {"vec", TokenType::VEC},
                {"map", TokenType::MAP},
            };
            return table;
        }

        bool is_ident_start(const char c) {
            return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
        }

        bool is_ident_char(const char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
        }

        bool is_digit(const char c) {
            return std::isdigit(static_cast<unsigned char>(c)) != 0;
        }

        bool is_hex_digit(const char c) {
            return std::isxdigit(static_cast<unsigned char>(c)) != 0;
        }

        // Cursor over the source that keeps 1-based line/column up to date.
        class Scanner {
        public:
            const std::string& src;
            Diagnostics& diag;
            std::vector<Token> tokens;
            size_t pos = 0;
            int line = 1, col = 1;

            Scanner(const std::string& s, Diagnostics& d) : src(s), diag(d) {}

            [[nodiscard]] bool at_end() const { return pos >= src.size(); }
            [[nodiscard]] char peek(const size_t ahead = 0) const {
                return pos + ahead < src.size() ? src[pos + ahead] : '\0';
            }
            char advance() {
                const char c = src[pos++];
                if (c == '\n') {
                    line++;
                    col = 1;
                } else {
                    col++;
                }
                return c;
            }
            void error(const int l, const int c, const std::string& msg) const {
                diag.error("lexer", {l, c}, msg);
            }
            void add(const TokenType type, std::string lexeme, const int l, const int c) {
                tokens.emplace_back(type, std::move(lexeme), l, c);
            }

            // Decodes one escape sequence after a backslash. Returns false on an unknown escape.
            bool escape(char& out) {
                if (at_end()) return false;
                switch (const char c = advance()) {
                    case 'n': out = '\n'; return true;
                    case 't': out = '\t'; return true;
                    case 'r': out = '\r'; return true;
                    case '0': out = '\0'; return true;
                    case '\\': out = '\\'; return true;
                    case '"': out = '"'; return true;
                    case '\'': out = '\''; return true;
                    default: out = c; return false;
                }
            }

            void scan_string(const int l, const int c) {
                std::string body;
                while (true) {
                    if (at_end() || peek() == '\n') {
                        error(l, c, "unterminated string literal");
                        return;
                    }
                    const char ch = advance();
                    if (ch == '"') break;
                    if (ch == '\\') {
                        char decoded = 0;
                        if (!escape(decoded)) {
                            error(line, col - 2, std::string("unknown escape sequence '\\") + decoded + "'");
                        }
                        body += decoded;
                        continue;
                    }
                    body += ch;
                }
                add(TokenType::STRING_LITERAL, body, l, c);
            }

            void scan_char(const int l, const int c) {
                if (at_end() || peek() == '\n' || peek() == '\'') {
                    error(l, c, "empty or unterminated char literal");
                    if (peek() == '\'') advance();
                    return;
                }
                char value = advance();
                if (value == '\\') {
                    char decoded = 0;
                    if (!escape(decoded)) {
                        error(line, col - 2, std::string("unknown escape sequence '\\") + decoded + "'");
                    }
                    value = decoded;
                }
                if (peek() != '\'') {
                    error(l, c, "char literal must contain exactly one character");
                    while (!at_end() && peek() != '\'' && peek() != '\n') advance();
                    if (peek() == '\'') advance();
                    return;
                }
                advance(); // closing quote
                add(TokenType::CHAR_LITERAL, std::string(1, value), l, c);
            }

            void scan_number(const int l, const int c) {
                std::string digits;
                // hexadecimal
                if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
                    advance();
                    advance();
                    while (is_hex_digit(peek()) || peek() == '_') {
                        const char ch = advance();
                        if (ch != '_') digits += ch;
                    }
                    if (digits.empty()) {
                        error(l, c, "hex literal needs at least one digit");
                        digits = "0";
                    }
                    if (is_ident_char(peek())) {
                        error(line, col, "invalid character in number literal");
                        while (is_ident_char(peek())) advance();
                    }
                    add(TokenType::INT_LITERAL, "0x" + digits, l, c);
                    return;
                }

                bool is_float = false;
                while (is_digit(peek()) || peek() == '_') {
                    const char ch = advance();
                    if (ch != '_') digits += ch;
                }
                // fraction: only when a digit follows the dot, so `1..10` stays a range
                if (peek() == '.' && is_digit(peek(1))) {
                    is_float = true;
                    digits += advance();
                    while (is_digit(peek()) || peek() == '_') {
                        const char ch = advance();
                        if (ch != '_') digits += ch;
                    }
                }
                // exponent
                if ((peek() == 'e' || peek() == 'E')
                    && (is_digit(peek(1)) || ((peek(1) == '+' || peek(1) == '-') && is_digit(peek(2))))) {
                    is_float = true;
                    digits += advance();
                    if (peek() == '+' || peek() == '-') digits += advance();
                    while (is_digit(peek())) digits += advance();
                }
                if (is_ident_char(peek())) {
                    error(line, col, "invalid character in number literal");
                    while (is_ident_char(peek())) advance();
                }
                add(is_float ? TokenType::FLOAT_LITERAL : TokenType::INT_LITERAL, digits, l, c);
            }

            void scan_word(const int l, const int c) {
                std::string word;
                while (is_ident_char(peek())) word += advance();
                if (const auto it = keywords().find(word); it != keywords().end()) {
                    add(it->second, word, l, c);
                    return;
                }
                int bits;
                bool is_signed;
                if (parse_int_type_name(word, bits, is_signed)) {
                    add(TokenType::INT_N, word, l, c);
                    return;
                }
                add(TokenType::IDENTIFIER, word, l, c);
            }

            // Operators: try the longest spelling first.
            bool scan_operator(const int l, const int c) {
                struct Op {
                    const char* text;
                    TokenType type;
                };
                static const Op ops[] = {
                    {"<<=", TokenType::SHL_EQUAL}, {">>=", TokenType::SHR_EQUAL},
                    {"..", TokenType::DOUBLE_DOT}, {"->", TokenType::ARROW},
                    {"++", TokenType::PLUS_PLUS}, {"--", TokenType::MINUS_MINUS},
                    {"+=", TokenType::PLUS_EQUAL}, {"-=", TokenType::MINUS_EQUAL},
                    {"*=", TokenType::STAR_EQUAL}, {"/=", TokenType::SLASH_EQUAL},
                    {"%=", TokenType::PERCENT_EQUAL}, {"&=", TokenType::AMP_EQUAL},
                    {"|=", TokenType::PIPE_EQUAL}, {"^=", TokenType::CARET_EQUAL},
                    {"==", TokenType::EQUAL_EQUAL}, {"!=", TokenType::NOT_EQUAL},
                    {"<=", TokenType::LESS_EQUAL}, {">=", TokenType::GREATER_EQUAL},
                    {"&&", TokenType::AND_AND}, {"||", TokenType::OR_OR},
                    {"<<", TokenType::SHL}, {">>", TokenType::SHR},
                    {"{", TokenType::LEFT_CURLY_BRACKET}, {"}", TokenType::RIGHT_CURLY_BRACKET},
                    {"[", TokenType::LEFT_SQUARE_BRACKET}, {"]", TokenType::RIGHT_SQUARE_BRACKET},
                    {"(", TokenType::LEFT_PAREN}, {")", TokenType::RIGHT_PAREN},
                    {";", TokenType::SEMICOLON}, {",", TokenType::COMMA},
                    {":", TokenType::COLON}, {".", TokenType::DOT},
                    {"+", TokenType::PLUS}, {"-", TokenType::MINUS},
                    {"*", TokenType::STAR}, {"/", TokenType::SLASH},
                    {"%", TokenType::PERCENT}, {"=", TokenType::EQUAL},
                    {"<", TokenType::LESS}, {">", TokenType::GREATER},
                    {"!", TokenType::NOT}, {"&", TokenType::AMP},
                    {"|", TokenType::PIPE}, {"^", TokenType::CARET},
                    {"~", TokenType::TILDE},
                };
                for (const auto& [text, type] : ops) {
                    const std::string_view sv(text);
                    if (src.compare(pos, sv.size(), sv) == 0) {
                        for (size_t k = 0; k < sv.size(); k++) advance();
                        add(type, std::string(sv), l, c);
                        return true;
                    }
                }
                return false;
            }

            void run() {
                while (!at_end()) {
                    const char ch = peek();
                    const int l = line, c = col;
                    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f' || ch == '\v') {
                        advance();
                        continue;
                    }
                    if (ch == '#') { // comment until end of line
                        while (!at_end() && peek() != '\n') advance();
                        continue;
                    }
                    if (ch == '"') {
                        advance();
                        scan_string(l, c);
                        continue;
                    }
                    if (ch == '\'') {
                        advance();
                        scan_char(l, c);
                        continue;
                    }
                    if (is_digit(ch)) {
                        scan_number(l, c);
                        continue;
                    }
                    if (is_ident_start(ch)) {
                        scan_word(l, c);
                        continue;
                    }
                    if (scan_operator(l, c)) continue;
                    advance();
                    error(l, c, std::string("unexpected character '") + ch + "'");
                }
                add(TokenType::END_OF_FILE, "", line, col);
            }
        };
    } // namespace

    bool parse_int_type_name(const std::string& word, int& bits, bool& is_signed) {
        if (word.size() < 2 || word.size() > 4) return false;
        if (word[0] != 'i' && word[0] != 'u') return false;
        if (word[1] == '0') return false; // no leading zeros: "i08" is an identifier
        int value = 0;
        for (size_t i = 1; i < word.size(); i++) {
            if (!is_digit(word[i])) return false;
            value = value * 10 + (word[i] - '0');
        }
        if (value < 1 || value > 256) return false;
        bits = value;
        is_signed = word[0] == 'i';
        return true;
    }

    std::vector<Token> tokenize(const std::string& source, Diagnostics& diag) {
        Scanner scanner(source, diag);
        scanner.run();
        return std::move(scanner.tokens);
    }
} // namespace Lexer
