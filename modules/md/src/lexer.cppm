// Preliminary lexical boundary model. Bun's md parser is line-oriented and
// performs inline delimiter scanning; this token layer records source slices.
export module mbun.md.lexer;

import std;
import mbun.md.types;

export namespace mbun::md {

enum class TokenKind : std::uint8_t { Text, Newline, BlankLine, Fence, Heading, Quote, ListMarker, Html, End };
struct Token { TokenKind kind{TokenKind::Text}; Offset begin{0}; Offset end{0}; std::uint32_t data{0}; };

class Lexer {
public:
    explicit Lexer(std::string_view source) : source_{source} {}
    [[nodiscard]] std::string_view source() const { return source_; }
    // DEFERRED(S1): complete md4c-compatible line analysis and inline marks.
    [[nodiscard]] std::vector<Token> scan() const { return {}; }

private:
    std::string_view source_;
};

} // namespace mbun::md
