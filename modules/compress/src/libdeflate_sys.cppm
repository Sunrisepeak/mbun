// libdeflate_sys.cppm — narrow raw/zlib/gzip FFI seam. The three Format values
// map exactly to zlib windowBits (raw = -15, zlib = 15, gzip = 31), so this
// seam is backed by the shared zlib backend. libdeflate proper (the faster
// one-shot library bun also uses) is DEFERRED; the format-level behavior and
// round-trips are equivalent through zlib.
// ref: bun src/compress/libdeflate_sys.rs; Zig src/deps/libdeflate
export module mbun.compress.libdeflate_sys;

import std;
import mbun.compress.types;
import mbun.compress.zlib_native;

namespace mbun::compress::libdeflate_sys {

export enum class Format : std::uint8_t { raw, zlib, gzip };

export struct Allocator {
    void* opaque {nullptr};
};

inline int window_bits(Format f) {
    switch (f) {
    case Format::raw:  return -15;
    case Format::zlib: return 15;
    case Format::gzip: return 15 + 16;
    }
    return 15;
}

export inline Result<Bytes> compress(Format f, ByteView input, Options opts = {},
                                     Allocator = {}) {
    return zlib_deflate(input, window_bits(f), opts.level, opts.memLevel, opts.strategy);
}

export inline Result<Bytes> decompress(Format f, ByteView input, Allocator = {}) {
    return zlib_inflate(input, window_bits(f));
}

}  // namespace mbun::compress::libdeflate_sys
