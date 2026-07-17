// Conversion seam between the pure source-map representation and the shape
// consumed by Node's SourceMap class. No JSC headers or JSValue types belong
// here; the eventual binding can serialize these values at its edge.
// ref: bun src/sourcemap_jsc/JSSourceMap.rs constructor/find_entry/find_origin;
//      bun-v1.3.14 src/sourcemap_jsc/JSSourceMap.zig.
export module mbun.sourcemap_jsc.map_conversion;

import std;
import mbun.sourcemap;
import mbun.sourcemap_jsc.source_position;

export namespace mbun::sourcemap_jsc {

struct MapPayload {
    std::string mappings;
    std::vector<std::string> sources;
    std::vector<std::string> names;
};

struct ConvertedMap {
    std::vector<mbun::sourcemap::Mapping> mappings;
    std::vector<std::string> sources;
    std::vector<std::string> names;
};

[[nodiscard]] std::expected<ConvertedMap, std::string> convert_map(MapPayload payload) {
    auto parsed { mbun::sourcemap::parse_mappings(payload.mappings,
                                                   static_cast<std::int32_t>(payload.sources.size()),
                                                   true, true) };
    if (!parsed.ok) {
        return std::unexpected(std::format("{} at {}", parsed.error, parsed.errorOffset));
    }
    return ConvertedMap {
        .mappings = std::move(parsed.mappings),
        .sources = std::move(payload.sources),
        .names = std::move(payload.names),
    };
}

[[nodiscard]] std::optional<SourceMapEntry> to_entry(const ConvertedMap& map,
                                                     SourcePosition position) {
    auto* mapping { mbun::sourcemap::find_mapping(map.mappings, position.line, position.column) };
    if (mapping == nullptr) {
        return std::nullopt;
    }

    SourceMapEntry entry {
        .generated = { mapping->generated.line, mapping->generated.column },
        .original = { mapping->original.line, mapping->original.column },
    };
    if (mapping->sourceIndex >= 0
        && static_cast<std::size_t>(mapping->sourceIndex) < map.sources.size()) {
        entry.source = map.sources[static_cast<std::size_t>(mapping->sourceIndex)];
    }
    if (mapping->nameIndex >= 0
        && static_cast<std::size_t>(mapping->nameIndex) < map.names.size()) {
        entry.name = map.names[static_cast<std::size_t>(mapping->nameIndex)];
    }
    return entry;
}

[[nodiscard]] std::optional<SourceMapOrigin> to_origin(const ConvertedMap& map,
                                                        SourcePosition position) {
    auto entry { to_entry(map, position) };
    if (!entry) {
        return std::nullopt;
    }
    return SourceMapOrigin {
        .original = entry->original,
        .source = std::move(entry->source),
        .name = std::move(entry->name),
    };
}

}  // namespace mbun::sourcemap_jsc
