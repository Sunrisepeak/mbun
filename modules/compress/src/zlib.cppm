// zlib.cppm — RFC 1950 wrapper around a DEFLATE stream.
//
// Header/trailer shape is kept here because it is independent of the native
// zlib/libdeflate implementation. Payload processing is DEFERRED(S1).
// ref: bun src/compress/zlib.rs; Zig src/bun.js/api/bun/zlib.zig
export module mbun.compress.zlib;

import std;
import mbun.compress.types;
import mbun.compress.zlib_native;

namespace mbun::compress::zlib {

export struct Header {
    std::uint8_t cmf {0};
    std::uint8_t flg {0};
    bool dictionary {false};
};

export inline Result<Header> parse_header(ByteView input) {
    if (input.size() < 2) {
        return std::unexpected(Error{ErrorCode::truncated_input, "zlib header is truncated"});
    }
    const auto cmf {input[0]};
    const auto flg {input[1]};
    if ((cmf & 0x0fU) != 8U || (static_cast<unsigned>(cmf) * 256U + flg) % 31U != 0U) {
        return std::unexpected(Error{ErrorCode::invalid_input, "invalid zlib header"});
    }
    return Header{cmf, flg, (flg & 0x20U) != 0U};
}

// RFC 1950 zlib stream: windowBits = 15.
export Result<Bytes> compress(ByteView input, Options opts = {}) {
    return zlib_deflate(input, 15, opts.level, opts.memLevel, opts.strategy);
}

export Result<Bytes> decompress(ByteView input, Options = {}) {
    return zlib_inflate(input, 15);
}

}  // namespace mbun::compress::zlib
