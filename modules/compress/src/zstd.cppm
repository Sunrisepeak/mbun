// zstd.cppm — real Zstandard backend (compress one-shot, decompress streaming
// so concatenated multi-frame input round-trips like bun's zstd decoder).
// Owns the only <zstd.h> include (global module fragment).
//
// PORT-SOURCE: bun's zstd path calls ZSTD_compress at the requested level and
// decodes with a streaming loop (ZSTD_decompressStream) to span multiple
// frames; we mirror that.
// ref: bun src/compress/zstd.rs; Zig src/deps/zstd
module;

#include <zstd.h>

export module mbun.compress.zstd;

import std;
import mbun.compress.types;

namespace mbun::compress::zstd {

export struct Options {
    int level {3};
    bool checksum {false};
    bool contentSize {true};
};

export inline Result<Bytes> compress(ByteView input, Options opts = {}) {
    const std::size_t bound = ZSTD_compressBound(input.size());
    Bytes out;
    out.resize(bound);
    const std::size_t n = ZSTD_compress(out.data(), out.size(),
                                        input.data(), input.size(), opts.level);
    if (ZSTD_isError(n) != 0U) {
        return std::unexpected(Error{ErrorCode::invalid_input,
                                     std::string{"zstd compress: "} + ZSTD_getErrorName(n)});
    }
    out.resize(n);
    return out;
}

// Streaming decompress: loops ZSTD_decompressStream over the whole input so
// multiple concatenated frames are all decoded (a fresh frame resets the DCtx
// automatically between frames).
export inline Result<Bytes> decompress(ByteView input) {
    ZSTD_DStream* ds = ZSTD_createDStream();
    if (ds == nullptr) {
        return std::unexpected(Error{ErrorCode::native_library_unavailable,
                                     "zstd createDStream failed"});
    }
    ZSTD_initDStream(ds);

    Bytes out;
    const std::size_t chunk = ZSTD_DStreamOutSize();
    ZSTD_inBuffer in{input.data(), input.size(), 0};

    std::size_t lastRc = 0;
    bool progressed = false;
    while (in.pos < in.size) {
        const std::size_t base = out.size();
        out.resize(base + chunk);
        ZSTD_outBuffer ob{out.data() + base, chunk, 0};
        const std::size_t rc = ZSTD_decompressStream(ds, &ob, &in);
        out.resize(base + ob.pos);
        if (ZSTD_isError(rc) != 0U) {
            ZSTD_freeDStream(ds);
            return std::unexpected(Error{ErrorCode::invalid_input,
                                         std::string{"zstd decompress: "} + ZSTD_getErrorName(rc)});
        }
        lastRc = rc;
        progressed = true;
        if (rc == 0 && in.pos == in.size) {
            break;  // frame boundary and input drained
        }
    }
    ZSTD_freeDStream(ds);
    // A truncated frame drains the input while ZSTD still expects more (last rc
    // != 0 means we stopped mid-frame). bun errors instead of returning the
    // partial data. ref: bun src/zstd/lib.rs (ZstdDecompressionError).
    if (progressed && lastRc != 0) {
        return std::unexpected(Error{ErrorCode::invalid_input,
                                     "zstd decompress: truncated or incomplete frame"});
    }
    return out;
}

}  // namespace mbun::compress::zstd
