// Markdown option presets from bun-ref/src/md/root.rs and md/root.zig.
export module mbun.md.options;

import std;

export namespace mbun::md {

struct RenderOptions { bool tagFilter{false}; bool headingIds{false}; bool autolinkHeadings{false}; };

struct Options {
    bool tables{true}; bool strikethrough{true}; bool tasklists{true};
    bool permissiveAutolinks{false}; bool permissiveUrlAutolinks{false};
    bool permissiveWwwAutolinks{false}; bool permissiveEmailAutolinks{false};
    bool hardSoftBreaks{false}; bool wikiLinks{false}; bool underline{false};
    bool latexMath{false}; bool collapseWhitespace{false}; bool permissiveAtxHeaders{false};
    bool noIndentedCodeBlocks{false}; bool noHtmlBlocks{false}; bool noHtmlSpans{false};
    bool tagFilter{false}; bool headingIds{false}; bool autolinkHeadings{false};

    static constexpr Options commonmark() { return Options{false, false, false}; }
    static constexpr Options github() { Options value{}; value.permissiveAutolinks = true; value.permissiveWwwAutolinks = true; value.permissiveEmailAutolinks = true; value.tagFilter = true; return value; }
    static constexpr Options terminal() { Options value{}; value.permissiveUrlAutolinks = true; value.permissiveWwwAutolinks = true; value.permissiveEmailAutolinks = true; value.wikiLinks = true; value.underline = true; value.latexMath = true; return value; }
};

} // namespace mbun::md
