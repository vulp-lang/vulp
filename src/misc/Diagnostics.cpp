#include "misc/Diagnostics.hpp"

#include <algorithm>

Diagnostics::Diagnostics(std::string p_filename, const std::string& source) : filename(std::move(p_filename)) {
    std::string cur;
    for (const char c : source) {
        if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    lines.push_back(cur);
}

void Diagnostics::error(const std::string& stage, const SourceLoc loc, const std::string& message) {
    diags.push_back({stage, loc, message});
}

std::string Diagnostics::format_one(const Diagnostic& d) const {
    std::string out = filename + ":" + std::to_string(d.loc.line) + ":" + std::to_string(d.loc.col) + ": "
                      + d.stage + " error: " + d.message + "\n";
    if (d.loc.line >= 1 && d.loc.line <= static_cast<int>(lines.size())) {
        // very long lines are clipped to a window around the error column
        constexpr int WINDOW = 70;
        const std::string& full = lines[d.loc.line - 1];
        int start = 0, col = d.loc.col;
        std::string text = full, prefix, suffix;
        if (static_cast<int>(full.size()) > 2 * WINDOW + 20) {
            start = std::max(0, col - 1 - WINDOW);
            const int end = std::min(static_cast<int>(full.size()), col - 1 + WINDOW);
            text = full.substr(static_cast<size_t>(start), static_cast<size_t>(end - start));
            if (start > 0) prefix = "...";
            if (end < static_cast<int>(full.size())) suffix = "...";
            col -= start;
        }
        out += "    " + prefix + text + suffix + "\n    " + std::string(prefix.size(), ' ');
        for (int i = 1; i < col && i <= static_cast<int>(text.size()); i++) {
            out += (text[i - 1] == '\t') ? '\t' : ' ';
        }
        out += "^\n";
    }
    return out;
}

std::string Diagnostics::format() const {
    std::string out;
    for (const auto& d : diags) out += format_one(d);
    return out;
}
