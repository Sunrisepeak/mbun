// Low-level content-encoding types.
// ref: bun src/http_types/Encoding.rs and src/http_types/Encoding.zig.
export module mbun.http_types.encoding;

import std;

namespace mbun::http_types {

export enum class Encoding : std::uint8_t {
    identity,
    gzip,
    deflate,
    brotli,
    zstd,
    chunked,
};

export constexpr bool can_use_lib_deflate(Encoding encoding) noexcept {
    return encoding == Encoding::gzip || encoding == Encoding::deflate;
}

export constexpr bool is_compressed(Encoding encoding) noexcept {
    return encoding == Encoding::brotli || encoding == Encoding::gzip ||
           encoding == Encoding::deflate || encoding == Encoding::zstd;
}

} // namespace mbun::http_types
