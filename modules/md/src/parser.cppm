// Parser state skeleton following bun-ref/src/md/parser.rs and md/parser.zig.
// Block bytes, verbatim lines, container stacks, reference definitions, and
// inline delimiter state remain separate implementation units in S1.
export module mbun.md.parser;

import std;
import mbun.md.options;
import mbun.md.types;

export namespace mbun::md {

struct VerbatimLine { Offset begin{0}; Offset end{0}; std::uint32_t indent{0}; };
struct Line { enum class Kind : std::uint8_t { Blank, Hr, AtxHeader, SetextHeader, IndentedCode, FencedCode, Html, Text, Table, TableUnderline }; Kind kind{Kind::Blank}; Offset begin{0}; Offset end{0}; std::uint32_t indent{0}; std::uint32_t data{0}; };
struct Container { char marker{0}; bool loose{false}; bool task{false}; Offset taskMarkOffset{0}; std::uint32_t contentsIndent{0}; };

class Parser {
public:
    Parser(std::string_view source, Options options, Renderer& renderer) : source_{source}, options_{options}, renderer_{renderer} {}
    [[nodiscard]] std::expected<void, RenderError> process_document() {
        // DEFERRED(S1): line analysis, block construction, reference map,
        // and inline delimiter resolution.
        return {};
    }

private:
    std::string_view source_;
    Options options_;
    Renderer& renderer_;
    std::vector<Container> containers_;
    std::vector<VerbatimLine> currentBlockLines_;
};

} // namespace mbun::md
