// JSC-facing source positions. The runtime binding converts JS values to this
// value type before asking the pure sourcemap layer for an entry.
// ref: bun src/sourcemap_jsc/JSSourceMap.rs get_line_column/find_entry;
//      bun-v1.3.14 src/sourcemap_jsc/JSSourceMap.zig getLineColumn.
export module mbun.sourcemap_jsc.source_position;

import std;

export namespace mbun::sourcemap_jsc {

struct SourcePosition {
    std::int32_t line { 0 };
    std::int32_t column { 0 };

    [[nodiscard]] constexpr auto operator<=>(const SourcePosition&) const = default;
};

struct SourceMapEntry {
    SourcePosition generated {};
    SourcePosition original {};
    std::optional<std::string> source {};
    std::optional<std::string> name {};
};

struct SourceMapOrigin {
    SourcePosition original {};
    std::optional<std::string> source {};
    std::optional<std::string> name {};
};

}  // namespace mbun::sourcemap_jsc
