// HTML renderer boundary from bun-ref/src/md/html_renderer.rs and
// bun-zig-src/src/md/html_renderer.zig. Emission is DEFERRED(S1).
export module mbun.md.html_renderer;

import std;
import mbun.md.options;
import mbun.md.output;
import mbun.md.renderer;

export namespace mbun::md {

class HtmlRenderer final : public RendererAdapter {
public:
    explicit HtmlRenderer(RenderOptions options = {}) : options_{options} {}
    [[nodiscard]] std::string take_output() { return output_.take(); }

private:
    RenderOptions options_;
    OutputBuffer output_;
};

} // namespace mbun::md
