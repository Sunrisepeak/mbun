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
#include <zstd_errors.h>

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
//
// The first three ordinals are load-bearing: the JS layers (zlib_stream.cppm,
// node_zlib_iter.cppm) hard-code 0/1/2, so partial/full/block are APPENDED. They
// exist because node's zlib exposes five distinct deflate flush modes and
// `.flush()` defaults to Z_FULL_FLUSH, not Z_SYNC_FLUSH; collapsing them all
// onto Z_SYNC_FLUSH produced byte streams node's own tests do not accept
// (test-zlib-params compares the exact stored-block framing).
export enum class StreamFlush : std::uint8_t {
    none,     // _transform: keep buffering internally where possible
    sync,     // Z_SYNC_FLUSH: emit everything buffered so far
    finish,   // _flush(): finalize the stream
    partial,  // Z_PARTIAL_FLUSH
    full,     // Z_FULL_FLUSH (.flush() default for the zlib family)
    block,    // Z_BLOCK
};

// Result of one incremental process() call.
export struct StreamChunk {
    Bytes output;                 // bytes produced this call
    std::size_t consumed { 0 };   // input bytes consumed this call
    bool stream_end { false };    // codec reached end-of-stream
    bool ok { true };             // false → data error (message set)
    std::string message;
    // node's err.code for the failure, when the engine reports one the JS layer
    // could not have guessed from the message: Z_NEED_DICT for a zlib stream
    // that wants a dictionary, ERR_<BrotliDecoderErrorString> for brotli, and
    // ZSTD_error_<name> for zstd. Empty means "no engine code" and the JS layer
    // falls back to Z_DATA_ERROR.
    std::string code;
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
    // windowBits >= 32 asks zlib to sniff zlib-vs-gzip framing, so at open time
    // we do NOT yet know whether concatenated members apply: node's UNZIP mode
    // becomes GUNZIP (multi-member) or INFLATE (single member) once the first two
    // header bytes are in. Tracked across writes because createUnzip() is fed one
    // byte at a time by test-zlib-unzip-one-byte-chunks.
    bool inflateAutoDetect { false };
    int autoHeaderBytesSeen { 0 };
    int openWindowBits { 15 };

    // node's `dictionary` option for inflate. A zlib-framed stream cannot take
    // the dictionary until it asks (Z_NEED_DICT), so it is stored here and
    // applied from process(); raw deflate takes it immediately at open.
    Bytes dictionary;
    bool hasDictionary { false };

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

// node reports a zstd failure as err.code === "ZSTD_error_<enum name>"
// (test-zlib-zstd-pledged-src-size asserts ZSTD_error_srcSize_wrong exactly).
// zstd's API only offers human-readable strings — ZSTD_getErrorName() answers
// "Src size is incorrect" — so the public ZSTD_ErrorCode enum from
// zstd_errors.h is mapped by hand.
std::string zstd_error_code_(std::size_t rc) {
    switch (ZSTD_getErrorCode(rc)) {
        case ZSTD_error_no_error: return "ZSTD_error_no_error";
        case ZSTD_error_GENERIC: return "ZSTD_error_GENERIC";
        case ZSTD_error_prefix_unknown: return "ZSTD_error_prefix_unknown";
        case ZSTD_error_version_unsupported: return "ZSTD_error_version_unsupported";
        case ZSTD_error_frameParameter_unsupported: return "ZSTD_error_frameParameter_unsupported";
        case ZSTD_error_frameParameter_windowTooLarge: return "ZSTD_error_frameParameter_windowTooLarge";
        case ZSTD_error_corruption_detected: return "ZSTD_error_corruption_detected";
        case ZSTD_error_checksum_wrong: return "ZSTD_error_checksum_wrong";
        case ZSTD_error_literals_headerWrong: return "ZSTD_error_literals_headerWrong";
        case ZSTD_error_dictionary_corrupted: return "ZSTD_error_dictionary_corrupted";
        case ZSTD_error_dictionary_wrong: return "ZSTD_error_dictionary_wrong";
        case ZSTD_error_dictionaryCreation_failed: return "ZSTD_error_dictionaryCreation_failed";
        case ZSTD_error_parameter_unsupported: return "ZSTD_error_parameter_unsupported";
        case ZSTD_error_parameter_combination_unsupported: return "ZSTD_error_parameter_combination_unsupported";
        case ZSTD_error_parameter_outOfBound: return "ZSTD_error_parameter_outOfBound";
        case ZSTD_error_tableLog_tooLarge: return "ZSTD_error_tableLog_tooLarge";
        case ZSTD_error_maxSymbolValue_tooLarge: return "ZSTD_error_maxSymbolValue_tooLarge";
        case ZSTD_error_maxSymbolValue_tooSmall: return "ZSTD_error_maxSymbolValue_tooSmall";
        case ZSTD_error_stabilityCondition_notRespected: return "ZSTD_error_stabilityCondition_notRespected";
        case ZSTD_error_stage_wrong: return "ZSTD_error_stage_wrong";
        case ZSTD_error_init_missing: return "ZSTD_error_init_missing";
        case ZSTD_error_memory_allocation: return "ZSTD_error_memory_allocation";
        case ZSTD_error_workSpace_tooSmall: return "ZSTD_error_workSpace_tooSmall";
        case ZSTD_error_dstSize_tooSmall: return "ZSTD_error_dstSize_tooSmall";
        case ZSTD_error_srcSize_wrong: return "ZSTD_error_srcSize_wrong";
        case ZSTD_error_dstBuffer_null: return "ZSTD_error_dstBuffer_null";
        case ZSTD_error_noForwardProgress_destFull: return "ZSTD_error_noForwardProgress_destFull";
        case ZSTD_error_noForwardProgress_inputEmpty: return "ZSTD_error_noForwardProgress_inputEmpty";
        case ZSTD_error_frameIndex_tooLarge: return "ZSTD_error_frameIndex_tooLarge";
        case ZSTD_error_seekableIO: return "ZSTD_error_seekableIO";
        case ZSTD_error_dstBuffer_wrong: return "ZSTD_error_dstBuffer_wrong";
        case ZSTD_error_srcBuffer_wrong: return "ZSTD_error_srcBuffer_wrong";
        case ZSTD_error_sequenceProducer_failed: return "ZSTD_error_sequenceProducer_failed";
        case ZSTD_error_externalSequences_invalid: return "ZSTD_error_externalSequences_invalid";
        default: return "ZSTD_error_GENERIC";
    }
}

int to_zlib_op_(StreamFlush f) {
    switch (f) {
        case StreamFlush::sync: return Z_SYNC_FLUSH;
        case StreamFlush::finish: return Z_FINISH;
        case StreamFlush::partial: return Z_PARTIAL_FLUSH;
        case StreamFlush::full: return Z_FULL_FLUSH;
        case StreamFlush::block: return Z_BLOCK;
        default: return Z_NO_FLUSH;
    }
}

// brotli and zstd have no partial/full/block distinction: anything that is not
// "keep buffering" and not "finalize" is their single flush operation.
bool is_flush_op_(StreamFlush f) { return f != StreamFlush::none && f != StreamFlush::finish; }

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
            st->openWindowBits = windowBits;
            // gzip framing (mag + 16) carries RFC 1952 members and may be
            // concatenated. Auto-detect framing (mag + 32) clears the 16 bit, so
            // it lands in inflateAutoDetect instead and is decided from the
            // first two header bytes in process_inflate_.
            st->inflateAllowsConcatenatedMembers = windowBits > 0 && (windowBits & 16) != 0;
            st->inflateAutoDetect = windowBits >= 32;
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
    // Remember it either way: a zlib-framed stream rejects the dictionary now
    // (it is only accepted after Z_NEED_DICT) and node reports "Bad dictionary"
    // vs "Missing dictionary" based on whether one was supplied at all, so the
    // bytes have to outlive this call even when inflateSetDictionary fails here.
    st->dictionary.assign(dict.begin(), dict.end());
    st->hasDictionary = true;
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
    // node's UNZIP mode resolves to GUNZIP or INFLATE from the first two bytes
    // (src/node_zlib.cc: GZIP_HEADER_ID1/ID2, tracked in gzip_id_bytes_read_).
    // Only the gzip resolution gets concatenated-member handling — the test
    // corpus asserts that unzip()ing two concatenated *zlib* streams yields the
    // first one only.
    for (std::size_t i {0}; st->inflateAutoDetect && i < input.size(); ++i) {
        const std::uint8_t b { input[i] };
        if (st->autoHeaderBytesSeen == 0) {
            if (b != 0x1f) { st->inflateAutoDetect = false; break; }
            st->autoHeaderBytesSeen = 1;
        } else {
            st->inflateAllowsConcatenatedMembers = b == 0x8b;
            st->inflateAutoDetect = false;
        }
    }
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
        if (rc == Z_NEED_DICT) {
            // node src/node_zlib.cc: a zlib stream that asks for a dictionary is
            // "Missing dictionary" when none was supplied and "Bad dictionary"
            // when the supplied one does not match the stream's Adler-32.
            if (!st->hasDictionary) {
                r.ok = false; r.message = "Missing dictionary"; r.code = "Z_NEED_DICT"; break;
            }
            if (inflateSetDictionary(&zs, st->dictionary.data(),
                                     static_cast<uInt>(st->dictionary.size())) == Z_OK)
                continue;
            r.ok = false; r.message = "Bad dictionary"; r.code = "Z_DATA_ERROR"; break;
        }
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
        : is_flush_op_(flush)        ? BROTLI_OPERATION_FLUSH
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
        if (res == BROTLI_DECODER_RESULT_ERROR) {
            // The engine DOES distinguish a corrupt stream from a truncated one
            // (BROTLI_DECODER_ERROR_FORMAT_* vs the NEEDS_MORE_INPUT path above);
            // until now that detail died here and every brotli failure reached JS
            // as one generic message with no code. node reports
            // "ERR_" + BrotliDecoderErrorString(code) as err.code.
            const char* name { BrotliDecoderErrorString(BrotliDecoderGetErrorCode(st->bd)) };
            r.ok = false;
            r.message = name != nullptr ? name : "brotli decode failed";
            r.code = std::string{"ERR_"} + (name != nullptr ? name : "BROTLI_DECODE_FAILED");
            break;
        }
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
        : is_flush_op_(flush)        ? ZSTD_e_flush
                                     : ZSTD_e_continue };
    ZSTD_inBuffer in { input.data(), input.size(), 0 };

    std::uint8_t buf[16384];
    for (;;) {
        ZSTD_outBuffer out { buf, sizeof(buf), 0 };
        const std::size_t rc { ZSTD_compressStream2(st->zc, &out, &in, dir) };
        if (ZSTD_isError(rc)) {
            r.ok = false;
            r.message = ZSTD_getErrorName(rc);
            r.code = zstd_error_code_(rc);
            break;
        }
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
        if (ZSTD_isError(rc)) {
            r.ok = false;
            r.message = ZSTD_getErrorName(rc);
            r.code = zstd_error_code_(rc);
            break;
        }
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
    // A reset stream re-sniffs its framing from scratch.
    st->autoHeaderBytesSeen = 0;
    if (st->kind == StreamKind::inflate) {
        st->inflateAllowsConcatenatedMembers =
            st->openWindowBits > 0 && (st->openWindowBits & 16) != 0;
        st->inflateAutoDetect = st->openWindowBits >= 32;
    }
    switch (st->kind) {
        case StreamKind::deflate: return deflateReset(&st->zs) == Z_OK;
        case StreamKind::inflate: return inflateReset(&st->zs) == Z_OK;
        case StreamKind::zstd_dec: ZSTD_initDStream(st->zd); return true;
        case StreamKind::zstd_enc: ZSTD_CCtx_reset(st->zc, ZSTD_reset_session_only); return true;
        default: return false;
    }
}

// node zlib .params(level, strategy): change the deflate parameters mid-stream
// without restarting the stream (deflateParams). The caller must have flushed
// first (node's Zlib.prototype.params does a Z_SYNC_FLUSH), otherwise zlib has
// pending output and reports Z_BUF_ERROR. Only meaningful for compressors.
export bool stream_params(int handle, int level, int strategy) {
    StreamState* st { find_(handle) };
    if (!st || st->kind != StreamKind::deflate) return false;
    return deflateParams(&st->zs, level, strategy) == Z_OK;
}

// node zstd `pledgedSrcSize`: declare the uncompressed size up front so the
// frame header carries it and the encoder rejects a mismatched total
// (ZSTD_error_srcSize_wrong). Must be set before any input is compressed.
export bool stream_zstd_set_pledged_src_size(int handle, unsigned long long size) {
    StreamState* st { find_(handle) };
    if (!st || st->kind != StreamKind::zstd_enc) return false;
    return !ZSTD_isError(ZSTD_CCtx_setPledgedSrcSize(st->zc, size));
}

// Release the codec state. Idempotent.
export void stream_close(int handle) {
    registry_().erase(handle);
}

}  // namespace mbun::compress
