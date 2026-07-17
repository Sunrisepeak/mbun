// test_compress.cpp — mbun.compress pure-logic smoke checks.
//
// Exercises the framing/model logic that does NOT depend on the DEFERRED(S1)
// native backends: gzip RFC 1952 header parsing, the shared Result/Error
// vocabulary, and the deferred_native() seam contract. The zlib/libdeflate/
// brotli/zstd encoders themselves return native_library_unavailable until the
// native libraries are wired in.
// ref: bun src/compress/{gzip,zlib,deflate,brotli,zstd}.rs
import std;
import mbun.compress;

namespace {

using namespace mbun::compress;

int gChecks{0};
int gFailures{0};

void check(bool ok, std::string_view what) {
    ++gChecks;
    if (!ok) {
        ++gFailures;
        std::println("FAIL: {}", what);
    }
}

Bytes bytes(std::initializer_list<int> vs) {
    Bytes out;
    out.reserve(vs.size());
    for (int v : vs) {
        out.push_back(static_cast<std::uint8_t>(v));
    }
    return out;
}

}  // namespace

int main() {
    // Valid gzip header: magic 1f 8b, method 8 (deflate), mtime little-endian.
    {
        Bytes in = bytes({0x1f, 0x8b, 0x08, 0x00, 0x78, 0x56, 0x34, 0x12, 0x00, 0xff});
        auto hdr = gzip::parse_header(in);
        check(hdr.has_value(), "valid gzip header parses");
        if (hdr) {
            check(hdr->method == 8, "gzip method is deflate");
            check(hdr->modificationTime == 0x12345678U, "gzip mtime little-endian");
            check(hdr->operatingSystem == 0xff, "gzip OS byte");
        }
    }

    // Truncated header (< 10 bytes) is rejected as truncated_input.
    {
        Bytes in = bytes({0x1f, 0x8b, 0x08});
        auto hdr = gzip::parse_header(in);
        check(!hdr.has_value(), "truncated gzip header rejected");
        check(!hdr.has_value() && hdr.error().code == ErrorCode::truncated_input,
              "truncated header yields truncated_input");
    }

    // Bad magic is rejected as invalid_input.
    {
        Bytes in = bytes({0x00, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0});
        auto hdr = gzip::parse_header(in);
        check(!hdr.has_value() && hdr.error().code == ErrorCode::invalid_input,
              "bad gzip magic yields invalid_input");
    }

    // ---- Real native backends: compress -> decompress round-trips. ----

    // A payload with structure (repetition) so the codecs actually shrink it.
    Bytes payload;
    {
        const char* seed = "The quick brown fox jumps over the lazy dog. ";
        for (int i = 0; i < 200; ++i) {
            for (const char* p = seed; *p != '\0'; ++p) {
                payload.push_back(static_cast<std::uint8_t>(*p));
            }
        }
    }

    auto roundtrip_zlib = [&](std::string_view name, auto&& comp, auto&& decomp) {
        auto c = comp(ByteView{payload});
        check(c.has_value(), std::string{name} + " compress ok");
        if (!c) return;
        check(c->size() < payload.size(), std::string{name} + " actually shrinks payload");
        auto d = decomp(ByteView{*c});
        check(d.has_value(), std::string{name} + " decompress ok");
        check(d.has_value() && *d == payload, std::string{name} + " round-trips exactly");
    };

    roundtrip_zlib("deflate",
        [](ByteView v) { return deflate::encode(v); },
        [](ByteView v) { return deflate::decode(v); });
    roundtrip_zlib("zlib",
        [](ByteView v) { return zlib::compress(v); },
        [](ByteView v) { return zlib::decompress(v); });
    roundtrip_zlib("gzip",
        [](ByteView v) { return gzip::compress(v); },
        [](ByteView v) { return gzip::decompress(v); });
    roundtrip_zlib("libdeflate_sys/gzip",
        [](ByteView v) { return libdeflate_sys::compress(libdeflate_sys::Format::gzip, v); },
        [](ByteView v) { return libdeflate_sys::decompress(libdeflate_sys::Format::gzip, v); });

    // gzip output must carry the RFC 1952 magic (1f 8b 08) so node:zlib and
    // external `gunzip` accept it.
    {
        auto c = gzip::compress(ByteView{payload});
        check(c.has_value() && c->size() >= 3 && (*c)[0] == 0x1fU &&
                  (*c)[1] == 0x8bU && (*c)[2] == 0x08U,
              "gzip output has RFC 1952 magic");
    }

    // zstd round-trip (default level).
    {
        auto c = zstd::compress(ByteView{payload});
        check(c.has_value(), "zstd compress ok");
        if (c) {
            check(c->size() < payload.size(), "zstd shrinks payload");
            auto d = zstd::decompress(ByteView{*c});
            check(d.has_value() && *d == payload, "zstd round-trips exactly");
        }
    }

    // zstd multi-frame: two independent frames concatenated must all decode.
    {
        auto f1 = zstd::compress(ByteView{payload});
        Bytes half{payload.begin(), payload.begin() + payload.size() / 2};
        auto f2 = zstd::compress(ByteView{half});
        check(f1.has_value() && f2.has_value(), "zstd two frames compress");
        if (f1 && f2) {
            Bytes both;
            both.insert(both.end(), f1->begin(), f1->end());
            both.insert(both.end(), f2->begin(), f2->end());
            auto d = zstd::decompress(ByteView{both});
            Bytes expect = payload;
            expect.insert(expect.end(), half.begin(), half.end());
            check(d.has_value() && *d == expect, "zstd decodes concatenated frames");
        }
    }

    // brotli round-trip.
    {
        auto c = brotli::compress(ByteView{payload});
        check(c.has_value(), "brotli compress ok");
        if (c) {
            check(c->size() < payload.size(), "brotli shrinks payload");
            auto d = brotli::decompress(ByteView{*c});
            check(d.has_value() && *d == payload, "brotli round-trips exactly");
        }
    }

    // Empty-input round-trips (edge case bun's APIs must honor).
    {
        Bytes empty;
        auto gz = gzip::compress(ByteView{empty});
        check(gz.has_value(), "gzip compresses empty input");
        if (gz) {
            auto d = gzip::decompress(ByteView{*gz});
            check(d.has_value() && d->empty(), "gzip empty round-trips to empty");
        }
        auto zc = zstd::compress(ByteView{empty});
        auto bc = brotli::compress(ByteView{empty});
        check(zc.has_value() && zstd::decompress(ByteView{*zc}).value_or(Bytes{1}).empty(),
              "zstd empty round-trips to empty");
        check(bc.has_value() && brotli::decompress(ByteView{*bc}).value_or(Bytes{1}).empty(),
              "brotli empty round-trips to empty");
    }

    // Corrupt input must fail cleanly (no crash), not silently succeed.
    {
        Bytes garbage = bytes({0xde, 0xad, 0xbe, 0xef, 0x00, 0x11, 0x22});
        check(!zlib::decompress(ByteView{garbage}).has_value(), "zlib rejects garbage");
        check(!zstd::decompress(ByteView{garbage}).has_value(), "zstd rejects garbage");
    }

    // Truncated (half-length) valid streams must fail with a bounded return,
    // not spin forever. Regression for the zlib_inflate infinite loop where
    // inflate() returns Z_BUF_ERROR with avail_in==0 (no more input) yet the
    // loop only grew the output buffer and never broke. gunzipSync(truncated)
    // used to hang here; it must now return an error promptly.
    {
        auto truncate_rejects = [&](std::string_view name, auto&& comp, auto&& decomp) {
            auto c = comp(ByteView{payload});
            check(c.has_value(), std::string{name} + " truncation: compress ok");
            if (!c || c->size() < 2) return;
            Bytes half{c->begin(), c->begin() + c->size() / 2};
            auto d = decomp(ByteView{half});  // must return (bounded), not hang
            check(!d.has_value(),
                  std::string{name} + " truncated input rejected (bounded)");
        };
        truncate_rejects("deflate",
            [](ByteView v) { return deflate::encode(v); },
            [](ByteView v) { return deflate::decode(v); });
        truncate_rejects("zlib",
            [](ByteView v) { return zlib::compress(v); },
            [](ByteView v) { return zlib::decompress(v); });
        truncate_rejects("gzip",
            [](ByteView v) { return gzip::compress(v); },
            [](ByteView v) { return gzip::decompress(v); });
    }

    // deferred_native() carries the seam name in its message.
    {
        Error e = deferred_native("zstd");
        check(e.code == ErrorCode::native_library_unavailable, "deferred_native code");
        check(e.message.find("zstd") != std::string::npos, "deferred_native names the seam");
    }

    std::println("compress: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
