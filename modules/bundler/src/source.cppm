// source.cppm — mbun.bundler.source: input source file record.
//
// Minimal port of `bun_ast::Source` (path text + contents + source index) as
// the bundler graph consumes it. The full `bun_ast::Source` carries a
// `fs::Path` handle with pretty/namespace fields; here we hold owned byte
// strings. Real fs `Path`/`PathName` wiring is DEFERRED(S-bundler).
// ref: .mbun/bun-ref/src/ast/*.rs (Source), bundler/Graph.rs (InputFile.source).
export module mbun.bundler.source;

import std;
import mbun.bundler.index;

export namespace mbun::bundler {

struct Source {
    // Absolute path text (bun: `path.text`).
    std::string path;
    // Pretty/display path (bun: `path.pretty`) — relative for diagnostics.
    std::string prettyPath;
    // File contents.
    std::string contents;
    // Assigned source index in the module graph.
    Index index { Index::invalid() };

    Source() = default;

    static Source init_path_string(std::string_view p, std::string_view c) {
        Source s;
        s.path = std::string { p };
        s.prettyPath = std::string { p };
        s.contents = std::string { c };
        return s;
    }

    [[nodiscard]] bool is_empty() const { return path.empty() && contents.empty(); }
};

}  // namespace mbun::bundler
