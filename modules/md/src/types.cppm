// Markdown event model, based on bun-ref/src/md/types.rs and
// bun-zig-src/src/md/types.zig. Source offsets stay explicit in this first
// port; parser ownership and renderer lifetime rules are deferred to S1.
export module mbun.md.types;

import std;

export namespace mbun::md {

using Offset = std::uint32_t;
using Size = std::uint32_t;

enum class BlockType : std::uint8_t { Doc, Quote, Ul, Ol, Li, Hr, H, Code, Html, P, Table, Thead, Tbody, Tr, Th, Td };
enum class SpanType : std::uint8_t { Em, Strong, A, Img, Code, Del, LatexMath, LatexMathDisplay, WikiLink, Underline };
enum class TextType : std::uint8_t { Normal, NullChar, Br, SoftBr, Entity, Code, Html, LatexMath };
enum class Align : std::uint8_t { Default, Left, Center, Right };

struct SpanDetail {
    std::string_view href{};
    std::string_view title{};
    bool autolink{false};
    bool autolinkEmail{false};
    bool permissiveAutolink{false};
    bool autolinkWww{false};
};

struct RenderError {
    enum class Code : std::uint8_t { OutOfMemory, JavaScriptError, JavaScriptTerminated, StackOverflow, InputTooLarge, TooManyBlocks };
    Code code{Code::JavaScriptError};
};

class Renderer {
public:
    virtual ~Renderer() = default;
    virtual std::expected<void, RenderError> enter_block(BlockType, std::uint32_t data, std::uint32_t flags) = 0;
    virtual std::expected<void, RenderError> leave_block(BlockType, std::uint32_t data) = 0;
    virtual std::expected<void, RenderError> enter_span(SpanType, SpanDetail) = 0;
    virtual std::expected<void, RenderError> leave_span(SpanType) = 0;
    virtual std::expected<void, RenderError> text(TextType, std::string_view) = 0;
};

} // namespace mbun::md
