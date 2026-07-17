// deflate.cppm — RFC 1951 stream model and native seam.
//
// The actual bit reader, Huffman tables, sliding window, and level-dependent
// encoder are intentionally left as the next translation stage. This file
// records the Bun-facing contract without inventing a replacement algorithm.
// ref: bun src/compress/deflate.rs; Zig src/deps/zlib/deflate.c
export module mbun.compress.deflate;

import std;
import mbun.compress.types;
import mbun.compress.zlib_native;

namespace mbun::compress::deflate {

export enum class BlockKind : std::uint8_t { stored, fixed, dynamic };

export struct StreamInfo {
    BlockKind block {BlockKind::stored};
    std::size_t consumed {0};
    bool finalBlock {false};
};

// Raw RFC 1951 DEFLATE (no zlib/gzip framing): zlib windowBits = -15.
export Result<Bytes> encode(ByteView input, Options opts = {}) {
    return zlib_deflate(input, -15, opts.level, opts.memLevel, opts.strategy);
}

export Result<Bytes> decode(ByteView input, std::size_t* consumed = nullptr) {
    auto r = zlib_inflate(input, -15);
    if (consumed != nullptr) *consumed = r.has_value() ? input.size() : 0;
    return r;
}

}  // namespace mbun::compress::deflate
