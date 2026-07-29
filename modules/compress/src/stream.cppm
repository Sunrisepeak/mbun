// stream.cppm — stateful, incremental (handle-based) streaming compression.
//
// The one-shot zlib/brotli/zstd entry points in this subsystem cover node:zlib's
// *Sync / async single-shot APIs. The streaming Transform classes (createGzip,
// createInflate, createBrotliCompress, …) need the *_handle lifecycle: open a
// codec state, feed data chunk by chunk, flush, and close — reporting how many
// input bytes were consumed and whether the codec reached end-of-stream so the
// JS Transform can auto-end its readable side (and expose bytesWritten).
//
// PORT-SOURCE: bun drives the same libz/brotli/zstd streaming loops from its
// node:zlib native handles (deflate/inflate with Z_NO_FLUSH/Z_FINISH,
// BrotliEncoder/DecoderCompressStream, ZSTD_compressStream2/decompressStream).
// ref: bun src/compress/*.rs; node lib/zlib.js (_processChunk).
//
// This module owns a private handle registry; the JS layer holds opaque int ids.
module;

#include <zlib.h>
#include <brotli/decode.h>
#include <brotli/encode.h>
#include <zstd.h>

export module mbun.compress.stream;

import std;
import mbun.compress.types;

namespace mbun::compress {

export enum class StreamKind : std::uint8_t {
    deflate,      // zlib/gzip/raw compress (windowBits picks the framing)
    inflate,      // zlib/gzip/raw/auto decompress
    brotli_enc,
    brotli_dec,
    zstd_enc,
    zstd_dec,
};

// Flush operation requested by the JS layer for one process() call.
export enum class StreamFlush : std::uint8_t {
    none,    // _transform: keep buffering internally where possible
    sync,    // .flush(): emit everything buffered so far
    finish,  // _flush(): finalize the stream
};

// Result of one incremental process() call.
export struct StreamChunk {
    Bytes output;                 // bytes produced this call
    std::size_t consumed { 0 };   // input bytes consumed this call
    bool stream_end { false };    // codec reached end-of-stream
    bool ok { true };             // false → data error (message set)
    std::string message;
};

namespace {

struct StreamState {
    StreamKind kind;
    bool ended { false };
    // gzip (and inflate auto-detect) may carry several RFC 1952 members. A
    // member boundary is not the stream boundary until the JS transform's final
    // flush: the next write may begin another member.
    bool inflateAllowsConcatenatedMembers { false };
    bool inflateAtMemberBoundary { false };

    // zlib (deflate + inflate)
    z_stream zs {};
    bool zsInit { false };

    // brotli
    BrotliEncoderState* be { nullptr };
    BrotliDecoderState* bd { nullptr };

    // zstd
    ZSTD_CStream* zc { nullptr };
    ZSTD_DStream* zd { nullptr };

    ~StreamState() {
        if (zsInit) {
            if (kind == StreamKind::deflate) deflateEnd(&zs);
            else inflateEnd(&zs);
        }
        if (be) BrotliEncoderDestroyInstance(be);
        if (bd) BrotliDecoderDestroyInstance(bd);
        if (zc) ZSTD_freeCStream(zc);
        if (zd) ZSTD_freeDStream(zd);
    }
};

std::unordered_map<int, std::unique_ptr<StreamState>>& registry_() {
    static std::unordered_map<int, std::unique_ptr<StreamState>> g;
    return g;
}

int next_id_() {
    static int g { 0 };
    return ++g;
}

StreamState* find_(int handle) {
    auto& reg { registry_() };
    auto it { reg.find(handle) };
    return it == reg.end() ? nullptr : it->second.get();
}

int to_zlib_op_(StreamFlush f) {
    switch (f) {
        case StreamFlush::sync: return Z_SYNC_FLUSH;
        case StreamFlush::finish: return Z_FINISH;
        default: return Z_NO_FLUSH;
    }
}

}  // namespace

// Open a streaming codec state. Returns an opaque handle id, or -1 on failure.
// windowBits follows the deflateInit2/inflateInit2 convention (raw = -mag,
// zlib = mag, gzip = mag + 16, auto-detect = mag + 32). level/memLevel/strategy
// apply to deflate; quality/lgwin/mode apply to brotli encode; level applies to
// zstd encode.
export int stream_open(StreamKind kind, int windowBits, int level, int memLevel,
                       int strategy, int quality, int lgwin, int mode) {
    auto st { std::make_unique<StreamState>() };
    st->kind = kind;

    switch (kind) {
        case StreamKind::deflate: {
            const int lvl { level < 0 ? Z_DEFAULT_COMPRESSION : level };
            if (deflateInit2(&st->zs, lvl, Z_DEFLATED, windowBits, memLevel, strategy) != Z_OK)
                return -1;
            st->zsInit = true;
            break;
        }
        case StreamKind::inflate: {
            if (inflateInit2(&st->zs, windowBits) != Z_OK) return -1;
            st->zsInit = true;
            st->inflateAllowsConcatenatedMembers = windowBits > 0 && (windowBits & 16) != 0;
            break;
        }
        case StreamKind::brotli_enc: {
            st->be = BrotliEncoderCreateInstance(nullptr, nullptr, nullptr);
            if (!st->be) return -1;
            if (quality >= 0)
                BrotliEncoderSetParameter(st->be, BROTLI_PARAM_QUALITY,
                                          static_cast<std::uint32_t>(quality));
            if (lgwin > 0)
                BrotliEncoderSetParameter(st->be, BROTLI_PARAM_LGWIN,
                                          static_cast<std::uint32_t>(lgwin));
            if (mode > 0)
                BrotliEncoderSetParameter(st->be, BROTLI_PARAM_MODE,
                                          static_cast<std::uint32_t>(mode));
            break;
        }
        case StreamKind::brotli_dec: {
            st->bd = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
            if (!st->bd) return -1;
            break;
        }
        case StreamKind::zstd_enc: {
            st->zc = ZSTD_createCStream();
            if (!st->zc) return -1;
            ZSTD_initCStream(st->zc, level);
            break;
        }
        case StreamKind::zstd_dec: {
            st->zd = ZSTD_createDStream();
            if (!st->zd) return -1;
            ZSTD_initDStream(st->zd);
            break;
        }
    }

    const int id { next_id_() };
    registry_().emplace(id, std::move(st));
    return id;
}

// Set the inflate dictionary (node zlib `dictionary` option). Safe to call after
// stream_open; a raw-deflate dictionary is applied immediately, a zlib one is
// applied on Z_NEED_DICT inside process(). Returns true on success.
export bool stream_inflate_set_dictionary(int handle, ByteView dict) {
    StreamState* st { find_(handle) };
    if (!st || st->kind != StreamKind::inflate) return false;
    return inflateSetDictionary(&st->zs,
                                reinterpret_cast<const Bytef*>(dict.data()),
                                static_cast<uInt>(dict.size())) == Z_OK;
}

namespace {

StreamChunk process_deflate_(StreamState* st, ByteView input, StreamFlush flush) {
    StreamChunk r;
    const int op { to_zlib_op_(flush) };
    z_stream& zs { st->zs };
    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input.data()));
    zs.avail_in = static_cast<uInt>(input.size());

    std::uint8_t buf[16384];
    int rc { Z_OK };
    do {
        zs.next_out = buf;
        zs.avail_out = sizeof(buf);
        rc = deflate(&zs, op);
        if (rc == Z_STREAM_ERROR) { r.ok = false; r.message = "zlib deflate stream error"; break; }
        const std::size_t produced { sizeof(buf) - zs.avail_out };
        r.output.insert(r.output.end(), buf, buf + produced);
    } while (zs.avail_out == 0);

    if (op == Z_FINISH && rc == Z_STREAM_END) { r.stream_end = true; st->ended = true; }
    r.consumed = input.size() - zs.avail_in;
    return r;
}

StreamChunk process_inflate_(StreamState* st, ByteView input, StreamFlush flush) {
    StreamChunk r;
    z_stream& zs { st->zs };
    // A previous write ended exactly at a gzip member. Keep the stream open for
    // a later member, but let the final empty flush close the readable side.
    if (st->inflateAllowsConcatenatedMembers && st->inflateAtMemberBoundary && input.empty()) {
        if (flush == StreamFlush::finish) { r.stream_end = true; st->ended = true; }
        return r;
    }
    if (!input.empty()) st->inflateAtMemberBoundary = false;
    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input.data()));
    zs.avail_in = static_cast<uInt>(input.size());

    std::uint8_t buf[16384];
    for (;;) {
        zs.next_out = buf;
        zs.avail_out = sizeof(buf);
        const int rc { inflate(&zs, Z_NO_FLUSH) };
        const std::size_t produced { sizeof(buf) - zs.avail_out };
        r.output.insert(r.output.end(), buf, buf + produced);
        if (rc == Z_STREAM_END) {
            if (!st->inflateAllowsConcatenatedMembers) {
                r.stream_end = true; st->ended = true; break;
            }
            // zlib leaves the next member (or trailer) in avail_in. All-zero
            // trailer padding is ignored by node; anything else is fed through
            // a reset decoder so a malformed gzip-looking member still reports
            // zlib's ordinary data error.
            bool zeroTrailer { true };
            for (uInt i {0}; i < zs.avail_in; ++i) {
                if (zs.next_in[i] != 0) { zeroTrailer = false; break; }
            }
            if (zs.avail_in != 0 && zeroTrailer) {
                zs.next_in += zs.avail_in;
                zs.avail_in = 0;
            }
            if (zs.avail_in == 0) {
                st->inflateAtMemberBoundary = true;
                if (flush == StreamFlush::finish) { r.stream_end = true; st->ended = true; break; }
            }
            const Bytef* const nextIn {zs.next_in};
            const uInt remaining {zs.avail_in};
            if (inflateReset(&zs) != Z_OK) { r.ok = false; r.message = "zlib inflate reset failed"; break; }
            zs.next_in = const_cast<Bytef*>(nextIn);
            zs.avail_in = remaining;
            // The fresh member appends to this call's output; it must not start
            // overwriting the first member's bytes after inflateReset().
            zs.next_out = buf;
            zs.avail_out = sizeof(buf);
            if (remaining == 0) break;
            continue;
        }
        if (rc == Z_NEED_DICT) { r.ok = false; r.message = "zlib need dictionary"; break; }
        if (rc == Z_DATA_ERROR || rc == Z_MEM_ERROR) {
            r.ok = false;
            r.message = zs.msg != nullptr ? zs.msg : "incorrect data check";
            break;
        }
        // Z_BUF_ERROR (or Z_OK) with output room and no more input → needs more.
        if (zs.avail_in == 0 && zs.avail_out != 0) break;
        if (rc == Z_BUF_ERROR && zs.avail_out != 0) break;
    }
    r.consumed = input.size() - zs.avail_in;
    return r;
}

StreamChunk process_brotli_enc_(StreamState* st, ByteView input, StreamFlush flush) {
    StreamChunk r;
    BrotliEncoderOperation op {
        flush == StreamFlush::finish ? BROTLI_OPERATION_FINISH
        : flush == StreamFlush::sync ? BROTLI_OPERATION_FLUSH
                                     : BROTLI_OPERATION_PROCESS };
    std::size_t availIn { input.size() };
    const std::uint8_t* nextIn { input.data() };

    std::uint8_t buf[16384];
    for (;;) {
        std::size_t availOut { sizeof(buf) };
        std::uint8_t* nextOut { buf };
        if (!BrotliEncoderCompressStream(st->be, op, &availIn, &nextIn, &availOut,
                                         &nextOut, nullptr)) {
            r.ok = false; r.message = "brotli encode failed"; break;
        }
        r.output.insert(r.output.end(), buf, buf + (sizeof(buf) - availOut));
        if (op == BROTLI_OPERATION_FINISH && BrotliEncoderIsFinished(st->be)) {
            r.stream_end = true; st->ended = true; break;
        }
        if (BrotliEncoderHasMoreOutput(st->be)) continue;
        if (availIn == 0) break;
    }
    r.consumed = input.size() - availIn;
    return r;
}

StreamChunk process_brotli_dec_(StreamState* st, ByteView input) {
    StreamChunk r;
    std::size_t availIn { input.size() };
    const std::uint8_t* nextIn { input.data() };

    std::uint8_t buf[16384];
    for (;;) {
        std::size_t availOut { sizeof(buf) };
        std::uint8_t* nextOut { buf };
        const BrotliDecoderResult res {
            BrotliDecoderDecompressStream(st->bd, &availIn, &nextIn, &availOut,
                                          &nextOut, nullptr) };
        r.output.insert(r.output.end(), buf, buf + (sizeof(buf) - availOut));
        if (res == BROTLI_DECODER_RESULT_SUCCESS) { r.stream_end = true; st->ended = true; break; }
        if (res == BROTLI_DECODER_RESULT_ERROR) { r.ok = false; r.message = "brotli decode failed"; break; }
        if (res == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT) break;
        // NEEDS_MORE_OUTPUT → loop with a fresh buffer.
    }
    r.consumed = input.size() - availIn;
    return r;
}

StreamChunk process_zstd_enc_(StreamState* st, ByteView input, StreamFlush flush) {
    StreamChunk r;
    const ZSTD_EndDirective dir {
        flush == StreamFlush::finish ? ZSTD_e_end
        : flush == StreamFlush::sync ? ZSTD_e_flush
                                     : ZSTD_e_continue };
    ZSTD_inBuffer in { input.data(), input.size(), 0 };

    std::uint8_t buf[16384];
    for (;;) {
        ZSTD_outBuffer out { buf, sizeof(buf), 0 };
        const std::size_t rc { ZSTD_compressStream2(st->zc, &out, &in, dir) };
        if (ZSTD_isError(rc)) { r.ok = false; r.message = "zstd compress failed"; break; }
        r.output.insert(r.output.end(), buf, buf + out.pos);
        if (dir == ZSTD_e_end) {
            if (rc == 0) { r.stream_end = true; st->ended = true; break; }
            continue;  // rc > 0 → more output pending
        }
        if (in.pos == in.size && out.pos < out.size) break;  // fully consumed & flushed
    }
    r.consumed = in.pos;
    return r;
}

StreamChunk process_zstd_dec_(StreamState* st, ByteView input) {
    StreamChunk r;
    ZSTD_inBuffer in { input.data(), input.size(), 0 };

    std::uint8_t buf[16384];
    for (;;) {
        ZSTD_outBuffer out { buf, sizeof(buf), 0 };
        const std::size_t rc { ZSTD_decompressStream(st->zd, &out, &in) };
        if (ZSTD_isError(rc)) { r.ok = false; r.message = "zstd decode failed"; break; }
        r.output.insert(r.output.end(), buf, buf + out.pos);
        if (rc == 0) { r.stream_end = true; st->ended = true; break; }  // frame complete
        if (in.pos == in.size && out.pos < out.size) break;  // needs more input
    }
    r.consumed = in.pos;
    return r;
}

}  // namespace

// Feed `input` into the codec identified by `handle`. `flush` selects the
// operation (none = process, sync = flush, finish = finalize). Decoders ignore
// `flush`. Returns produced output, bytes consumed and end-of-stream status.
export StreamChunk stream_process(int handle, ByteView input, StreamFlush flush) {
    StreamState* st { find_(handle) };
    if (!st) { StreamChunk r; r.ok = false; r.message = "invalid zlib stream handle"; return r; }
    if (st->ended) { StreamChunk r; r.stream_end = true; return r; }
    switch (st->kind) {
        case StreamKind::deflate: return process_deflate_(st, input, flush);
        case StreamKind::inflate: return process_inflate_(st, input, flush);
        case StreamKind::brotli_enc: return process_brotli_enc_(st, input, flush);
        case StreamKind::brotli_dec: return process_brotli_dec_(st, input);
        case StreamKind::zstd_enc: return process_zstd_enc_(st, input, flush);
        case StreamKind::zstd_dec: return process_zstd_dec_(st, input);
    }
    StreamChunk r; r.ok = false; r.message = "unknown stream kind"; return r;
}

// Reset a codec back to its initial state (node zlib .reset()). Compressors and
// zlib inflaters can reset in place; others are reopened by the caller.
export bool stream_reset(int handle) {
    StreamState* st { find_(handle) };
    if (!st) return false;
    st->ended = false;
    st->inflateAtMemberBoundary = false;
    switch (st->kind) {
        case StreamKind::deflate: return deflateReset(&st->zs) == Z_OK;
        case StreamKind::inflate: return inflateReset(&st->zs) == Z_OK;
        case StreamKind::zstd_dec: ZSTD_initDStream(st->zd); return true;
        case StreamKind::zstd_enc: ZSTD_CCtx_reset(st->zc, ZSTD_reset_session_only); return true;
        default: return false;
    }
}

// Release the codec state. Idempotent.
export void stream_close(int handle) {
    registry_().erase(handle);
}

}  // namespace mbun::compress
