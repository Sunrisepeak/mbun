// Public Markdown->HTML entry point bridging mbun::md::Options to the
// CommonMark/GFM parser (mbun.md.cm_block). Mirrors bun md/root.rs's
// render_to_html surface.
export module mbun.md.cm_api;

import std;
import mbun.md.options;
import mbun.md.cm_block;

export namespace mbun::md {

// Render Markdown source to an HTML fragment, honouring the option presets
// (commonmark / github / terminal).
inline std::string render_html(std::string_view source, Options options = {}) {
    cm::BlockFlags flags;
    flags.tables = options.tables;
    flags.strikethrough = options.strikethrough;
    flags.tasklists = options.tasklists;
    flags.noIndentedCode = options.noIndentedCodeBlocks;
    flags.hardSoftBreaks = options.hardSoftBreaks;
    return cm::render_to_html(source, flags);
}

}  // namespace mbun::md
