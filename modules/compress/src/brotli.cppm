// brotli.cppm — real Brotli backend (encode + streaming decode). Owns the only
// <brotli/{encode,decode}.h> includes (global module fragment).
//
// PORT-SOURCE: bun's brotli path encodes with BrotliEncoderCompress at the
// requested quality/lgwin/mode and decodes with a streaming loop
// (BrotliDecoderDecompressStream) growing the output buffer; we mirror that.
// ref: bun src/compress/brotli.rs; Zig src/deps/brotli
module;

#include <brotli/decode.h>
#include <brotli/encode.h>

export module mbun.compress.brotli;

import std;
import mbun.compress.types;

namespace mbun::compress::brotli {

export struct Options {
    int quality {-1};
    int mode {0};
    int lgwin {0};
};

export inline Result<Bytes> compress(ByteView input, Options opts = {}) {
    const int quality = opts.quality < 0 ? BROTLI_DEFAULT_QUALITY : opts.quality;
    const int lgwin = opts.lgwin <= 0 ? BROTLI_DEFAULT_WINDOW : opts.lgwin;
    const auto mode = static_cast<BrotliEncoderMode>(opts.mode);

    std::size_t out_size = BrotliEncoderMaxCompressedSize(input.size());
    if (out_size == 0) out_size = input.size() + 1024;
    Bytes out;
    out.resize(out_size);

    if (BrotliEncoderCompress(quality, lgwin, mode, input.size(), input.data(),
                              &out_size, out.data()) == BROTLI_FALSE) {
        return std::unexpected(Error{ErrorCode::invalid_input, "brotli compress failed"});
    }
    out.resize(out_size);
    return out;
}

export inline Result<Bytes> decompress(ByteView input) {
    BrotliDecoderState* st = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
    if (st == nullptr) {
        return std::unexpected(Error{ErrorCode::native_library_unavailable,
                                     "brotli createInstance failed"});
    }

    Bytes out;
    const std::size_t chunk = input.empty() ? 64 : input.size() * 4;
    std::size_t avail_in = input.size();
    const std::uint8_t* next_in = input.data();
    BrotliDecoderResult r = BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT;

    while (true) {
        const std::size_t base = out.size();
        out.resize(base + chunk);
        std::size_t avail_out = chunk;
        std::uint8_t* next_out = out.data() + base;
        r = BrotliDecoderDecompressStream(st, &avail_in, &next_in,
                                          &avail_out, &next_out, nullptr);
        out.resize(out.size() - avail_out);
        if (r == BROTLI_DECODER_RESULT_SUCCESS) break;
        if (r == BROTLI_DECODER_RESULT_ERROR ||
            r == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT) {
            BrotliDecoderDestroyInstance(st);
            const auto code = (r == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT)
                                  ? ErrorCode::truncated_input
                                  : ErrorCode::invalid_input;
            return std::unexpected(Error{code, "brotli decompress failed"});
        }
        // NEEDS_MORE_OUTPUT: loop grows the buffer and continues.
    }
    BrotliDecoderDestroyInstance(st);
    return out;
}

}  // namespace mbun::compress::brotli
