// ANSI renderer boundary from bun-ref/src/md/ansi_renderer.rs and
// bun-zig-src/src/md/ansi_renderer.zig. Terminal behavior is DEFERRED(S1/S2).
export module mbun.md.ansi_renderer;

import std;
import mbun.md.output;
import mbun.md.renderer;

export namespace mbun::md {

struct AnsiTheme { bool light{false}; std::uint16_t columns{80}; bool colors{true}; bool hyperlinks{false}; bool kittyGraphics{false}; };

class ImageUrlCollector final : public RendererAdapter {
public:
    [[nodiscard]] const std::vector<std::string>& urls() const { return urls_; }

private:
    std::vector<std::string> urls_;
};

class AnsiRenderer final : public RendererAdapter {
public:
    explicit AnsiRenderer(AnsiTheme theme = {}) : theme_{theme} {}
    [[nodiscard]] std::string take_output() { return output_.take(); }

private:
    AnsiTheme theme_;
    OutputBuffer output_;
};

} // namespace mbun::md
