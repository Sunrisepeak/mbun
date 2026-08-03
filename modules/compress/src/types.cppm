// types.cppm — shared compression vocabulary.
//
// Mechanical port scaffold. The Rust source is the preferred blueprint for
// result/error ownership; the Zig implementation is the compatibility
// reference for option names and stream formats.
// ref: bun src/compress/{zlib,gzip,deflate,brotli,zstd}.rs
export module mbun.compress.types;

import std;

namespace mbun::compress {

export using Bytes = std::vector<std::uint8_t>;
export using ByteView = std::span<const std::uint8_t>;

export enum class ErrorCode : std::uint8_t {
    invalid_input,
    truncated_input,
    checksum_mismatch,
    unsupported_option,
    native_library_unavailable,
};

export struct Error {
    ErrorCode code {ErrorCode::invalid_input};
    std::string message {};
};

export template <typename T>
using Result = std::expected<T, Error>;

export struct Options {
    int level {-1};
    int windowBits {15};
    int memLevel {8};
    int strategy {0};
};

export inline Error deferred_native(std::string_view name) {
    return {ErrorCode::native_library_unavailable,
            std::string{name} + " native backend is DEFERRED(S1)"};
}

}  // namespace mbun::compress
