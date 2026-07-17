// gzip.cppm — RFC 1952 member framing.
// ref: bun src/compress/gzip.rs; Zig src/deps/zlib/gzread.c
export module mbun.compress.gzip;

import std;
import mbun.compress.types;
import mbun.compress.zlib_native;

namespace mbun::compress::gzip {

export struct Header {
    std::uint8_t method {8};
    std::uint8_t flags {0};
    std::uint32_t modificationTime {0};
    std::uint8_t extraFlags {0};
    std::uint8_t operatingSystem {255};
};

export inline Result<Header> parse_header(ByteView input) {
    if (input.size() < 10) {
        return std::unexpected(Error{ErrorCode::truncated_input, "gzip header is truncated"});
    }
    if (input[0] != 0x1fU || input[1] != 0x8bU || input[2] != 8U) {
        return std::unexpected(Error{ErrorCode::invalid_input, "invalid gzip header"});
    }
    return Header{input[2], input[3],
                  static_cast<std::uint32_t>(input[4]) |
                      (static_cast<std::uint32_t>(input[5]) << 8U) |
                      (static_cast<std::uint32_t>(input[6]) << 16U) |
                      (static_cast<std::uint32_t>(input[7]) << 24U),
                  input[8], input[9]};
}

// RFC 1952 gzip member: windowBits = 15 + 16.
export Result<Bytes> compress(ByteView input, Options opts = {}) {
    return zlib_deflate(input, 15 + 16, opts.level, opts.memLevel, opts.strategy);
}

export Result<Bytes> decompress(ByteView input, Options = {}) {
    return zlib_inflate(input, 15 + 16);
}

}  // namespace mbun::compress::gzip
