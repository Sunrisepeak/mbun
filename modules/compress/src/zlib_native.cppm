// zlib_native.cppm — real zlib backend shared by the raw-DEFLATE / zlib / gzip
// framing modules. The three formats are the same codec with a different
// windowBits: raw = -15, zlib = 15, gzip = 15 + 16 (see zlib deflateInit2 /
// inflateInit2). This module owns the only <zlib.h> include (global module
// fragment); the framing modules stay header-free and just pass windowBits.
//
// PORT-SOURCE: bun's node:zlib / Bun.deflate* native path drives libz through
// deflateInit2 + deflate(Z_FINISH) and inflateInit2 + inflate loops with the
// same windowBits convention; we mirror that streaming loop here.
// ref: bun src/compress/zlib.rs; Zig src/bun.js/api/bun/zlib.zig
module;

#include <zlib.h>

export module mbun.compress.zlib_native;

import std;
import mbun.compress.types;

namespace mbun::compress {

// Deflate `input` with the given windowBits (raw/-15, zlib/15, gzip/31).
// level<0 maps to Z_DEFAULT_COMPRESSION. memLevel/strategy default per zlib.
export Result<Bytes> zlib_deflate(ByteView input, int windowBits, int level,
                                  int memLevel = 8, int strategy = Z_DEFAULT_STRATEGY) {
    z_stream strm{};
    const int lvl = level < 0 ? Z_DEFAULT_COMPRESSION : level;
    int rc = deflateInit2(&strm, lvl, Z_DEFLATED, windowBits, memLevel, strategy);
    if (rc != Z_OK) {
        return std::unexpected(Error{ErrorCode::unsupported_option,
                                     "zlib deflateInit2 failed"});
    }

    Bytes out;
    out.resize(input.empty() ? 64 : deflateBound(&strm, static_cast<uLong>(input.size())));

    strm.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input.data()));
    strm.avail_in = static_cast<uInt>(input.size());
    strm.next_out = reinterpret_cast<Bytef*>(out.data());
    strm.avail_out = static_cast<uInt>(out.size());

    do {
        if (strm.avail_out == 0) {
            const std::size_t used = out.size();
            out.resize(out.size() * 2);
            strm.next_out = reinterpret_cast<Bytef*>(out.data() + used);
            strm.avail_out = static_cast<uInt>(out.size() - used);
        }
        rc = deflate(&strm, Z_FINISH);
        // Same guard as inflate: a Z_BUF_ERROR with output space still free means
        // deflate cannot make progress and is not waiting on a full buffer, so
        // stop rather than loop. (With Z_FINISH + growing output this should not
        // happen, but it keeps the loop provably bounded.)
        if (rc == Z_BUF_ERROR && strm.avail_out != 0) {
            break;
        }
    } while (rc == Z_OK || rc == Z_BUF_ERROR);

    if (rc != Z_STREAM_END) {
        deflateEnd(&strm);
        return std::unexpected(Error{ErrorCode::invalid_input, "zlib deflate failed"});
    }
    out.resize(strm.total_out);
    deflateEnd(&strm);
    return out;
}

// Inflate `input` with the given windowBits. Handles output growth; for gzip
// windowBits (31) zlib will also transparently accept a zlib stream when the
// caller adds 32, but we keep formats explicit to match bun's per-API framing.
export Result<Bytes> zlib_inflate(ByteView input, int windowBits) {
    z_stream strm{};
    int rc = inflateInit2(&strm, windowBits);
    if (rc != Z_OK) {
        return std::unexpected(Error{ErrorCode::unsupported_option,
                                     "zlib inflateInit2 failed"});
    }

    Bytes out;
    out.resize(input.empty() ? 64 : input.size() * 4);

    strm.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input.data()));
    strm.avail_in = static_cast<uInt>(input.size());
    strm.next_out = reinterpret_cast<Bytef*>(out.data());
    strm.avail_out = static_cast<uInt>(out.size());

    do {
        if (strm.avail_out == 0) {
            const std::size_t used = out.size();
            out.resize(out.size() * 2);
            strm.next_out = reinterpret_cast<Bytef*>(out.data() + used);
            strm.avail_out = static_cast<uInt>(out.size() - used);
        }
        rc = inflate(&strm, Z_NO_FLUSH);
        // zlib returns Z_BUF_ERROR when no forward progress is possible. That is
        // only recoverable when it was caused by a full output buffer
        // (avail_out == 0) — we grow and retry above. If output space is still
        // available it means the input ran out mid-stream (truncated/corrupt),
        // so break instead of spinning forever re-calling inflate with no input.
        if (rc == Z_BUF_ERROR && strm.avail_out != 0) {
            break;
        }
    } while (rc == Z_OK || rc == Z_BUF_ERROR);

    if (rc != Z_STREAM_END) {
        // Surface zlib's own diagnostic (e.g. "invalid stored block lengths",
        // "incorrect header check") so node:zlib error messages match bun/node.
        std::string msg{strm.msg != nullptr ? strm.msg : "zlib inflate failed"};
        inflateEnd(&strm);
        const auto code = (rc == Z_DATA_ERROR) ? ErrorCode::invalid_input
                        : (rc == Z_NEED_DICT)  ? ErrorCode::unsupported_option
                                               : ErrorCode::truncated_input;
        return std::unexpected(Error{code, std::move(msg)});
    }
    out.resize(strm.total_out);
    inflateEnd(&strm);
    return out;
}

}  // namespace mbun::compress
