// Renderer protocol shared by HTML, ANSI, and callback renderers.
export module mbun.md.renderer;

import std;
import mbun.md.types;

export namespace mbun::md {

class RendererAdapter : public Renderer {
public:
    std::expected<void, RenderError> enter_block(BlockType, std::uint32_t, std::uint32_t) override { return {}; }
    std::expected<void, RenderError> leave_block(BlockType, std::uint32_t) override { return {}; }
    std::expected<void, RenderError> enter_span(SpanType, SpanDetail) override { return {}; }
    std::expected<void, RenderError> leave_span(SpanType) override { return {}; }
    std::expected<void, RenderError> text(TextType, std::string_view) override { return {}; }
};

} // namespace mbun::md
