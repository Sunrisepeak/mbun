// parser.cppm — the syntax-facing seam for the React compiler port.
//
// The Bun Rust/Zig react_compiler sources are not present in this checkout;
// the reproducible references are recorded in the audit document. This first
// seam intentionally models syntax metadata only. React transform semantics,
// JSX lowering, and JSC integration remain DEFERRED(S-react-compiler).
export module mbun.react_compiler.parser;

import std;

export namespace mbun::react_compiler {

enum class DiagnosticSeverity : std::uint8_t {
    warning,
    error,
};

struct SourceSpan {
    std::size_t start { 0 };
    std::size_t end { 0 };

    [[nodiscard]] constexpr std::size_t size() const { return end - start; }
};

struct Diagnostic {
    DiagnosticSeverity severity { DiagnosticSeverity::error };
    SourceSpan span {};
    std::string message;
};

struct ParsedFunction {
    std::string name;
    SourceSpan span {};
    std::size_t parameterCount { 0 };
};

struct ParsedModule {
    std::vector<ParsedFunction> functions;
};

struct ParseResult {
    ParsedModule module;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool has_errors() const {
        return std::ranges::any_of(diagnostics, [](const Diagnostic& diagnostic) {
            return diagnostic.severity == DiagnosticSeverity::error;
        });
    }
};

namespace detail {

[[nodiscard]] constexpr bool is_identifier_start(char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_'
        || value == '$';
}

[[nodiscard]] constexpr bool is_identifier_continue(char value) {
    return is_identifier_start(value) || (value >= '0' && value <= '9');
}

[[nodiscard]] std::size_t skip_space(std::string_view source, std::size_t offset) {
    while (offset < source.size() && std::isspace(static_cast<unsigned char>(source[offset])))
        ++offset;
    return offset;
}

[[nodiscard]] std::size_t count_parameters(std::string_view source, std::size_t offset,
                                           std::vector<Diagnostic>& diagnostics) {
    std::size_t count { 0 };
    bool has_token { false };
    int depth { 0 };
    for (; offset < source.size(); ++offset) {
        const char value = source[offset];
        if (value == '(') {
            ++depth;
            continue;
        }
        if (value == ')') {
            if (depth == 0)
                break;
            --depth;
            if (depth == 0) {
                if (has_token)
                    ++count;
                return count;
            }
            continue;
        }
        if (depth != 1)
            continue;
        if (value == ',') {
            if (has_token)
                ++count;
            has_token = false;
        } else if (!std::isspace(static_cast<unsigned char>(value))) {
            has_token = true;
        }
    }
    diagnostics.push_back(Diagnostic { DiagnosticSeverity::error,
                                       SourceSpan { offset, source.size() },
                                       "unterminated function parameter list" });
    return count;
}

}  // namespace detail

// Minimal syntax seam: discover `function name(...)` declarations and retain
// source offsets. It is deliberately not a JSX/JavaScript parser.
[[nodiscard]] ParseResult parse(std::string_view source) {
    ParseResult result;
    constexpr std::string_view KEYWORD { "function" };
    std::size_t offset { 0 };
    while ((offset = source.find(KEYWORD, offset)) != std::string_view::npos) {
        const bool boundary_before = offset == 0 || !detail::is_identifier_continue(source[offset - 1]);
        const std::size_t after_keyword = offset + KEYWORD.size();
        const bool boundary_after = after_keyword == source.size()
            || !detail::is_identifier_continue(source[after_keyword]);
        if (!boundary_before || !boundary_after) {
            offset = after_keyword;
            continue;
        }
        auto name_offset = detail::skip_space(source, after_keyword);
        if (name_offset == source.size() || !detail::is_identifier_start(source[name_offset])) {
            result.diagnostics.push_back(Diagnostic { DiagnosticSeverity::error,
                                                       SourceSpan { offset, after_keyword },
                                                       "function declaration is missing a name" });
            offset = after_keyword;
            continue;
        }
        auto name_end = name_offset + 1;
        while (name_end < source.size() && detail::is_identifier_continue(source[name_end]))
            ++name_end;
        auto paren = detail::skip_space(source, name_end);
        if (paren == source.size() || source[paren] != '(') {
            result.diagnostics.push_back(Diagnostic { DiagnosticSeverity::error,
                                                       SourceSpan { name_end, paren },
                                                       "function declaration is missing `(`" });
            offset = name_end;
            continue;
        }
        auto parameters = detail::count_parameters(source, paren, result.diagnostics);
        result.module.functions.push_back(ParsedFunction {
            std::string { source.substr(name_offset, name_end - name_offset) },
            SourceSpan { offset, name_end },
            parameters,
        });
        offset = name_end;
    }
    return result;
}

}  // namespace mbun::react_compiler
