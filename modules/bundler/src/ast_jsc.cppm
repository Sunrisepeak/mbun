// ast_jsc.cppm — validated, JSC-neutral view of serialized module metadata.
//
// ref: bun-ref/src/bundler/analyze_transpiled_module.rs and
// bun-ref/src/bundler_jsc/analyze_jsc.rs. The real JSModuleRecord creation is
// a native JSC operation and remains DEFERRED. Validation here prevents a
// malformed cache from reaching that boundary and mirrors Bun's discriminant
// and bounds checks.
export module mbun.bundler.ast_jsc;

import std;

export namespace mbun::bundler::ast_jsc {

// Bun serializes record kinds as arbitrary bytes. Keep the deserialization
// boundary byte-typed so an unknown tag is rejected rather than being treated
// as an invalid enum value before validation can report it.
using RecordTag = std::uint8_t;

inline constexpr RecordTag DECLARED_VARIABLE {0};
inline constexpr RecordTag LEXICAL_VARIABLE {1};
inline constexpr RecordTag IMPORT_INFO_SINGLE {2};
inline constexpr RecordTag IMPORT_INFO_SINGLE_TYPESCRIPT {3};
inline constexpr RecordTag IMPORT_INFO_NAMESPACE {4};
inline constexpr RecordTag EXPORT_INFO_INDIRECT {5};
inline constexpr RecordTag EXPORT_INFO_LOCAL {6};
inline constexpr RecordTag EXPORT_INFO_NAMESPACE {7};
inline constexpr RecordTag EXPORT_INFO_STAR {8};
inline constexpr RecordTag IMPORT_INFO_NAMESPACE_DEFER {9};

enum class RequestedModuleValue : std::uint8_t { None, Javascript, Webassembly, Json, HostDefined };

struct ModuleInfo {
    std::span<const std::uint32_t> string_lengths;
    std::span<const std::uint32_t> requested_keys;
    std::span<const RequestedModuleValue> requested_values;
    std::span<const std::uint8_t> requested_phases;
    std::span<const std::uint32_t> buffer;
    std::span<const RecordTag> record_kinds;
    std::size_t strings_byte_count { 0 };
};

constexpr std::size_t record_width(RecordTag tag) {
    switch (tag) {
        case DECLARED_VARIABLE:
        case LEXICAL_VARIABLE: return 1;
        case IMPORT_INFO_SINGLE:
        case IMPORT_INFO_SINGLE_TYPESCRIPT:
        case IMPORT_INFO_NAMESPACE:
        case IMPORT_INFO_NAMESPACE_DEFER: return 3;
        case EXPORT_INFO_INDIRECT: return 3;
        case EXPORT_INFO_LOCAL: return 3;
        case EXPORT_INFO_NAMESPACE: return 2;
        case EXPORT_INFO_STAR: return 1;
    }
    return 0;
}

constexpr bool valid_id(std::uint32_t id, std::size_t string_count) {
    // Bun reserves the high sentinel range for STAR_NAMESPACE and other
    // host-defined values; ordinary identifiers index the string table.
    return id < string_count || id >= 0xFFFF'FF00u;
}

bool validate(const ModuleInfo& info) {
    if (info.requested_keys.size() != info.requested_values.size()
        || info.requested_keys.size() != info.requested_phases.size()) {
        return false;
    }
    std::size_t string_bytes { 0 };
    for (const auto length : info.string_lengths) {
        if (string_bytes > info.strings_byte_count
            || length > info.strings_byte_count - string_bytes) {
            return false;
        }
        string_bytes += length;
    }
    for (const auto id : info.requested_keys) {
        if (!valid_id(id, info.string_lengths.size())) return false;
    }
    std::size_t cursor { 0 };
    for (const auto kind : info.record_kinds) {
        const auto width { record_width(kind) };
        if (width == 0 || width > info.buffer.size() - std::min(cursor, info.buffer.size())) {
            return false;
        }
        for (std::size_t i { 0 }; i < width; ++i) {
            if (!valid_id(info.buffer[cursor + i], info.string_lengths.size())) return false;
        }
        cursor += width;
    }
    for (const auto phase : info.requested_phases) {
        if (phase > 1) return false;
    }
    return cursor == info.buffer.size();
}

} // namespace mbun::bundler::ast_jsc
