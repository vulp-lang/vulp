#ifndef VULP_DIAGNOSTICS_HPP
#define VULP_DIAGNOSTICS_HPP

#include <string>
#include <vector>

// 1-based line / column of a token or AST node.
struct SourceLoc {
    int line = 0;
    int col = 0;
};

struct Diagnostic {
    std::string stage;   // "lexer", "parser", "semantic"
    SourceLoc loc;
    std::string message;
};

// Collects errors from every compiler stage and renders them with a source snippet:
//
//   prog.vp:3:9: semantic error: unknown variable 'y'
//       x = y + 1;
//           ^
class Diagnostics {
private:
    std::string filename;
    std::vector<std::string> lines;
    std::vector<Diagnostic> diags;

public:
    Diagnostics(std::string p_filename, const std::string& source);

    void error(const std::string& stage, SourceLoc loc, const std::string& message);
    [[nodiscard]] bool has_errors() const { return !diags.empty(); }
    [[nodiscard]] size_t count() const { return diags.size(); }
    [[nodiscard]] const std::vector<Diagnostic>& all() const { return diags; }
    [[nodiscard]] std::string format() const;
    [[nodiscard]] std::string format_one(const Diagnostic& d) const;
};

#endif // VULP_DIAGNOSTICS_HPP
