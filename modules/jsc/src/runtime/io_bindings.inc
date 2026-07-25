// ── native compression (real; node:zlib + Bun.deflateSync build on these) ────
// Exposed as globalThis.__mbunZlibNative. mbun.core.compress does the actual
// DEFLATE/zlib/gzip work; binary payloads cross the JS boundary as base64
// strings (first-cut bridge — no typed-array marshalling in the C API layer
// yet), and the JS layer converts to/from Buffer.

std::string zlib_b64_encode(std::span<const std::uint8_t> data) {
    static constexpr char TABLE[]{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};
    std::string out{};
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t i{0};
    for (; i + 3 <= data.size(); i += 3) {
        std::uint32_t v{static_cast<std::uint32_t>(data[i]) << 16
                        | static_cast<std::uint32_t>(data[i + 1]) << 8 | data[i + 2]};
        out.push_back(TABLE[v >> 18]);
        out.push_back(TABLE[(v >> 12) & 63]);
        out.push_back(TABLE[(v >> 6) & 63]);
        out.push_back(TABLE[v & 63]);
    }
    if (i + 1 == data.size()) {
        std::uint32_t v{static_cast<std::uint32_t>(data[i]) << 16};
        out.push_back(TABLE[v >> 18]);
        out.push_back(TABLE[(v >> 12) & 63]);
        out.push_back('=');
        out.push_back('=');
    } else if (i + 2 == data.size()) {
        std::uint32_t v{static_cast<std::uint32_t>(data[i]) << 16
                        | static_cast<std::uint32_t>(data[i + 1]) << 8};
        out.push_back(TABLE[v >> 18]);
        out.push_back(TABLE[(v >> 12) & 63]);
        out.push_back(TABLE[(v >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

std::vector<std::uint8_t> zlib_b64_decode(std::string_view s) {
    auto val{[](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    }};
    std::vector<std::uint8_t> out{};
    out.reserve(s.size() / 4 * 3);
    std::uint32_t acc{0};
    int bits{0};
    for (char c : s) {
        int v{val(c)};
        if (v < 0) continue;  // '=' padding / whitespace
        acc = acc << 6 | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>(acc >> bits));
        }
    }
    return out;
}

// Small numeric-arg reader with a default (node zlib options are all numbers by
// the time the JS layer forwards them here).
int zlibn_arg_int(JSContextRef ctx, std::size_t argc, const JSValueRef args[],
                  std::size_t i, int dflt) {
    if (i < argc && JSValueIsNumber(ctx, args[i])) {
        return static_cast<int>(JSValueToNumber(ctx, args[i], nullptr));
    }
    return dflt;
}

// ── byte bridge: typed array instead of base64 ─────────────────────────────
// Base64 costs ~6x the payload in transient JS strings on each crossing (encode
// → UTF-16 copy → UTF-8 copy → decode), so a one-shot deflate/inflate of a
// 150 MB buffer was OOM-killed. When the JS layer hands us a Uint8Array /
// ArrayBuffer we read it in place and answer with a Uint8Array; the base64
// signature stays for string callers.
bool arg_bytes_view(JSContextRef ctx, std::size_t argc, const JSValueRef args[], std::size_t i,
                   const std::uint8_t*& p, std::size_t& n) {
    p = nullptr;
    n = 0;
    if (i >= argc) return false;
    JSTypedArrayType tt{JSValueGetTypedArrayType(ctx, args[i], nullptr)};
    if (tt == kJSTypedArrayTypeNone) return false;
    JSObjectRef obj{JSValueToObject(ctx, args[i], nullptr)};
    if (obj == nullptr) return false;
    if (tt == kJSTypedArrayTypeArrayBuffer) {
        p = static_cast<const std::uint8_t*>(JSObjectGetArrayBufferBytesPtr(ctx, obj, nullptr));
        n = JSObjectGetArrayBufferByteLength(ctx, obj, nullptr);
    } else {
        p = static_cast<const std::uint8_t*>(JSObjectGetTypedArrayBytesPtr(ctx, obj, nullptr));
        n = JSObjectGetTypedArrayByteLength(ctx, obj, nullptr);
    }
    if (p == nullptr) n = 0;
    return true;
}
JSValueRef make_bytes_view(JSContextRef ctx, const std::vector<std::uint8_t>& bytes) {
    JSObjectRef ta{JSObjectMakeTypedArray(ctx, kJSTypedArrayTypeUint8Array, bytes.size(), nullptr)};
    if (ta != nullptr && !bytes.empty()) {
        if (void* d{JSObjectGetTypedArrayBytesPtr(ctx, ta, nullptr)}) {
            std::memcpy(d, bytes.data(), bytes.size());
        }
    }
    return ta;
}

// Map (format, windowBits magnitude) → zlib windowBits. raw = -mag, zlib = mag,
// gzip = mag + 16 (see zlib deflateInit2 / inflateInit2). "auto" (node unzip)
// uses mag + 32 so inflate auto-detects a zlib vs gzip wrapper.
int zlibn_window_bits(std::string_view fmt, int mag, bool decompress) {
    if (mag < 9 || mag > 15) mag = 15;
    if (fmt == "raw") return -mag;
    if (fmt == "gzip") return mag + 16;
    if (fmt == "auto") return decompress ? mag + 32 : mag;
    return mag;  // zlib
}

// compress(dataB64, fmt, level, windowBits, memLevel, strategy) → b64.
// fmt: "raw" | "zlib" | "gzip". Backed by the native zlib deflate loop
// (mbun.compress.zlib_native); honours level/windowBits/memLevel/strategy.
JSValueRef zlibn_compress_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                             const JSValueRef args[], JSValueRef* exc) {
    const std::uint8_t* vp{nullptr};
    std::size_t vn{0};
    const bool view{arg_bytes_view(ctx, argc, args, 0, vp, vn)};
    std::vector<std::uint8_t> data;
    if (!view) data = zlib_b64_decode(arg_string(ctx, argc, args, 0));
    std::string fmt{arg_string(ctx, argc, args, 1)};
    const int level{zlibn_arg_int(ctx, argc, args, 2, -1)};
    const int mag{zlibn_arg_int(ctx, argc, args, 3, 15)};
    const int memLevel{zlibn_arg_int(ctx, argc, args, 4, 8)};
    const int strategy{zlibn_arg_int(ctx, argc, args, 5, 0)};
    const int wbits{zlibn_window_bits(fmt, mag, /*decompress=*/false)};
    auto r{mbun::compress::zlib_deflate(
        view ? mbun::compress::ByteView{vp, vn} : mbun::compress::ByteView{data}, wbits, level,
        memLevel, strategy)};
    if (!r) {
        if (exc != nullptr) *exc = make_string(ctx, r.error().message);
        return JSValueMakeUndefined(ctx);
    }
    if (view) return make_bytes_view(ctx, *r);
    return make_string(ctx, zlib_b64_encode(*r));
}

// decompress(dataB64, format, windowBits) → b64. format: "raw" | "zlib" |
// "gzip" | "auto" ("auto" = node's unzip, auto-detecting zlib vs gzip framing).
JSValueRef zlibn_decompress_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                               const JSValueRef args[], JSValueRef* exc) {
    const std::uint8_t* vp{nullptr};
    std::size_t vn{0};
    const bool view{arg_bytes_view(ctx, argc, args, 0, vp, vn)};
    std::vector<std::uint8_t> data;
    if (!view) data = zlib_b64_decode(arg_string(ctx, argc, args, 0));
    std::string fmt{arg_string(ctx, argc, args, 1)};
    const int mag{zlibn_arg_int(ctx, argc, args, 2, 15)};
    const int wbits{zlibn_window_bits(fmt, mag, /*decompress=*/true)};
    auto r{mbun::compress::zlib_inflate(
        view ? mbun::compress::ByteView{vp, vn} : mbun::compress::ByteView{data}, wbits)};
    if (!r) {
        if (exc != nullptr) *exc = make_string(ctx, r.error().message);
        return JSValueMakeUndefined(ctx);
    }
    if (view) return make_bytes_view(ctx, *r);
    return make_string(ctx, zlib_b64_encode(*r));
}

// brotliCompress(dataB64, quality, lgwin, mode) → b64.
JSValueRef brotlin_compress_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                               const JSValueRef args[], JSValueRef* exc) {
    std::vector<std::uint8_t> data{zlib_b64_decode(arg_string(ctx, argc, args, 0))};
    mbun::compress::brotli::Options o{};
    o.quality = zlibn_arg_int(ctx, argc, args, 1, -1);
    o.lgwin = zlibn_arg_int(ctx, argc, args, 2, 0);
    o.mode = zlibn_arg_int(ctx, argc, args, 3, 0);
    auto r{mbun::compress::brotli::compress(mbun::compress::ByteView{data}, o)};
    if (!r) {
        if (exc != nullptr) *exc = make_string(ctx, r.error().message);
        return JSValueMakeUndefined(ctx);
    }
    return make_string(ctx, zlib_b64_encode(*r));
}

// brotliDecompress(dataB64) → b64.
JSValueRef brotlin_decompress_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                 const JSValueRef args[], JSValueRef* exc) {
    std::vector<std::uint8_t> data{zlib_b64_decode(arg_string(ctx, argc, args, 0))};
    auto r{mbun::compress::brotli::decompress(mbun::compress::ByteView{data})};
    if (!r) {
        if (exc != nullptr) *exc = make_string(ctx, r.error().message);
        return JSValueMakeUndefined(ctx);
    }
    return make_string(ctx, zlib_b64_encode(*r));
}

// zstdCompress(dataB64, level) → b64.
JSValueRef zstdn_compress_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                             const JSValueRef args[], JSValueRef* exc) {
    std::vector<std::uint8_t> data{zlib_b64_decode(arg_string(ctx, argc, args, 0))};
    mbun::compress::zstd::Options o{};
    o.level = zlibn_arg_int(ctx, argc, args, 1, 3);
    auto r{mbun::compress::zstd::compress(mbun::compress::ByteView{data}, o)};
    if (!r) {
        if (exc != nullptr) *exc = make_string(ctx, r.error().message);
        return JSValueMakeUndefined(ctx);
    }
    return make_string(ctx, zlib_b64_encode(*r));
}

// zstdDecompress(dataB64) → b64.
JSValueRef zstdn_decompress_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                               const JSValueRef args[], JSValueRef* exc) {
    std::vector<std::uint8_t> data{zlib_b64_decode(arg_string(ctx, argc, args, 0))};
    auto r{mbun::compress::zstd::decompress(mbun::compress::ByteView{data})};
    if (!r) {
        if (exc != nullptr) *exc = make_string(ctx, r.error().message);
        return JSValueMakeUndefined(ctx);
    }
    return make_string(ctx, zlib_b64_encode(*r));
}

// crc32(dataB64, initial) → number (node:zlib crc32 export).
JSValueRef zlibn_crc32_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                          const JSValueRef args[], JSValueRef*) {
    std::vector<std::uint8_t> data{zlib_b64_decode(arg_string(ctx, argc, args, 0))};
    std::uint32_t initial{0};
    if (argc > 1 && JSValueIsNumber(ctx, args[1])) {
        initial = static_cast<std::uint32_t>(JSValueToNumber(ctx, args[1], nullptr));
    }
    return JSValueMakeNumber(ctx, mbun::core::compress::crc32(data, initial));
}

// ── native Bun.Image (mbun.image: resize/rotate/quantize kernels + PNG codec) ──
// Exposed as globalThis.__mbunImageNative. Binary payloads cross as base64
// (same first-cut bridge as __mbunZlibNative); the JS layer shapes the
// Bun.Image builder class (Image.rs port) around `metadata` and `pipeline`.

double img_prop_num(JSContextRef ctx, JSObjectRef o, const char* name, double dflt) {
    if (o == nullptr) return dflt;
    JSValueRef v{get_prop(ctx, o, name)};
    if (v == nullptr || !JSValueIsNumber(ctx, v)) return dflt;
    return JSValueToNumber(ctx, v, nullptr);
}
bool img_prop_bool(JSContextRef ctx, JSObjectRef o, const char* name, bool dflt) {
    if (o == nullptr) return dflt;
    JSValueRef v{get_prop(ctx, o, name)};
    if (v == nullptr || JSValueIsUndefined(ctx, v)) return dflt;
    return JSValueToBoolean(ctx, v);
}
std::string img_prop_str(JSContextRef ctx, JSObjectRef o, const char* name,
                         std::string_view dflt) {
    if (o == nullptr) return std::string{dflt};
    JSValueRef v{get_prop(ctx, o, name)};
    if (v == nullptr || !JSValueIsString(ctx, v)) return std::string{dflt};
    return val_to_string(ctx, v);
}
JSValueRef img_throw(JSContextRef ctx, JSValueRef* exc, std::string_view msg) {
    if (exc != nullptr) *exc = make_string(ctx, msg);
    return JSValueMakeUndefined(ctx);
}

// metadata(srcB64, maxPixels) → { width, height, format }. Header-only probe
// (PNG IHDR / BMP header / GIF LSD); the decompression-bomb guard fires here,
// before any pixel allocation (Image.rs doMetadata + codecs guard).
JSValueRef imgn_metadata_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                            const JSValueRef args[], JSValueRef* exc) {
    namespace img = mbun::image;
    const std::vector<std::uint8_t> data{zlib_b64_decode(arg_string(ctx, argc, args, 0))};
    const double mpArg{argc > 1 && JSValueIsNumber(ctx, args[1])
                           ? JSValueToNumber(ctx, args[1], nullptr)
                           : static_cast<double>(img::DEFAULT_MAX_PIXELS)};
    const auto maxPixels{static_cast<std::uint64_t>(std::max(0.0, mpArg))};
    const std::string_view fmt{img::sniff_format(data)};
    if (fmt.empty()) return img_throw(ctx, exc, img::ERR_UNKNOWN);
    std::uint64_t w{0};
    std::uint64_t h{0};
    if (fmt == "png") {
        const auto dims{img::png_probe(data)};
        if (!dims) return img_throw(ctx, exc, img::ERR_DECODE);
        w = dims->first;
        h = dims->second;
    } else if (fmt == "bmp" && data.size() >= 26) {
        auto le32{[&](std::size_t i) {
            return static_cast<std::int32_t>(static_cast<std::uint32_t>(data[i])
                                             | static_cast<std::uint32_t>(data[i + 1]) << 8
                                             | static_cast<std::uint32_t>(data[i + 2]) << 16
                                             | static_cast<std::uint32_t>(data[i + 3]) << 24);
        }};
        w = static_cast<std::uint64_t>(std::abs(le32(18)));
        h = static_cast<std::uint64_t>(std::abs(le32(22)));
    } else if (fmt == "gif" && data.size() >= 10) {
        w = static_cast<std::uint64_t>(data[6]) | static_cast<std::uint64_t>(data[7]) << 8;
        h = static_cast<std::uint64_t>(data[8]) | static_cast<std::uint64_t>(data[9]) << 8;
    } else if (fmt == "jpeg") {
        const auto dims{img::jpg::jpeg_probe(data)};
        if (!dims) return img_throw(ctx, exc, img::ERR_DECODE);
        w = dims->first;
        h = dims->second;
    } else if (fmt == "webp") {
        // Header-only VP8/VP8L/VP8X probe (Image.rs doMetadata via WebPGetInfo).
        const auto p{img::webp::probe(data)};
        if (!p) return img_throw(ctx, exc, img::ERR_DECODE);
        w = p->width;
        h = p->height;
    } else {
        return img_throw(ctx, exc,
                         std::string{"Image: decode failed ("} + std::string{fmt}
                             + " metadata is not supported by mbun yet)");
    }
    if (w * h > maxPixels) return img_throw(ctx, exc, img::ERR_TOO_MANY_PIXELS);
    JSObjectRef out{JSObjectMake(ctx, nullptr, nullptr)};
    set_prop(ctx, out, "width", JSValueMakeNumber(ctx, static_cast<double>(w)));
    set_prop(ctx, out, "height", JSValueMakeNumber(ctx, static_cast<double>(h)));
    set_prop(ctx, out, "format", make_string(ctx, fmt));
    return out;
}

// placeholder(srcB64, maxPixels) → { b64 (PNG), width, height }. ThumbHash
// LQIP render (Image.rs make_placeholder): box ≤100 → hash → ≤32px PNG.
JSValueRef imgn_placeholder_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                               const JSValueRef args[], JSValueRef* exc) {
    namespace img = mbun::image;
    const std::vector<std::uint8_t> data{zlib_b64_decode(arg_string(ctx, argc, args, 0))};
    const double mpArg{argc > 1 && JSValueIsNumber(ctx, args[1])
                           ? JSValueToNumber(ctx, args[1], nullptr)
                           : static_cast<double>(img::DEFAULT_MAX_PIXELS)};
    const auto maxPixels{static_cast<std::uint64_t>(std::max(0.0, mpArg))};
    const std::string_view fmt{img::sniff_format(data)};
    std::expected<img::Decoded, std::string> decoded{};
    if (fmt == "png") {
        decoded = img::png_decode(data, maxPixels);
    } else if (fmt == "bmp") {
        decoded = img::bmp_decode(data, maxPixels);
    } else if (fmt == "gif") {
        decoded = img::gif_decode(data, maxPixels);
    } else if (fmt == "jpeg") {
        auto jd{img::jpg::jpeg_decode(data, maxPixels)};
        if (!jd) return img_throw(ctx, exc, jd.error());
        img::Decoded d{};
        d.rgba = std::move(jd->rgba);
        d.width = jd->width;
        d.height = jd->height;
        decoded = std::move(d);
    } else {
        return img_throw(ctx, exc, fmt.empty() ? std::string{img::ERR_UNKNOWN}
                                               : std::string{"Image: decode failed ("}
                                                     + std::string{fmt}
                                                     + " is not supported by mbun yet)");
    }
    if (!decoded) return img_throw(ctx, exc, decoded.error());
    const auto render{
        img::make_placeholder_render(decoded->rgba, decoded->width, decoded->height)};
    if (!render) return img_throw(ctx, exc, render.error());
    // Indexed at 64 colours: the render is a ≤32px blur, so quantisation error
    // is a few LSB while the palette keeps the file a few hundred bytes even
    // with mbun's fixed-Huffman deflate (dynamic Huffman is a DEFERRED
    // core.compress ratio optimization; bun emits truecolour via zlib).
    const std::vector<std::uint8_t> png{img::png_encode_indexed(
        render->rgba, render->width, render->height, -1, 64, false)};
    JSObjectRef res{JSObjectMake(ctx, nullptr, nullptr)};
    set_prop(ctx, res, "b64", make_string(ctx, zlib_b64_encode(png)));
    set_prop(ctx, res, "width", JSValueMakeNumber(ctx, render->width));
    set_prop(ctx, res, "height", JSValueMakeNumber(ctx, render->height));
    return res;
}

// pipeline(srcB64, opts) → { b64, width, height, format }. One native call
// covering decode → rotate → flip/flop → resize → modulate → encode (the fixed
// Sharp order Image.rs::apply_pipeline uses).
JSValueRef imgn_pipeline_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                            const JSValueRef args[], JSValueRef* exc) {
    namespace img = mbun::image;
    const std::vector<std::uint8_t> data{zlib_b64_decode(arg_string(ctx, argc, args, 0))};
    JSObjectRef opts{argc > 1 && JSValueIsObject(ctx, args[1])
                         ? JSValueToObject(ctx, args[1], nullptr)
                         : nullptr};
    const auto maxPixels{static_cast<std::uint64_t>(std::max(
        0.0, img_prop_num(ctx, opts, "maxPixels",
                          static_cast<double>(img::DEFAULT_MAX_PIXELS))))};

    const std::string_view fmt{img::sniff_format(data)};
    if (fmt.empty()) return img_throw(ctx, exc, img::ERR_UNKNOWN);
    std::expected<img::Decoded, std::string> decoded{};
    if (fmt == "png") {
        decoded = img::png_decode(data, maxPixels);
    } else if (fmt == "bmp") {
        decoded = img::bmp_decode(data, maxPixels);
    } else if (fmt == "gif") {
        decoded = img::gif_decode(data, maxPixels);
    } else if (fmt == "jpeg") {
        auto jd{img::jpg::jpeg_decode(data, maxPixels)};
        if (!jd) return img_throw(ctx, exc, jd.error());
        img::Decoded d{};
        d.rgba = std::move(jd->rgba);
        d.width = jd->width;
        d.height = jd->height;
        d.iccProfile = std::move(jd->iccProfile);
        decoded = std::move(d);
    } else if (fmt == "webp") {
        // libwebp bitstream decode; probe already bomb-guards on the header
        // dimensions before any canvas is allocated (codec_webp.rs order).
        auto wd{img::webp::decode(data, maxPixels)};
        if (!wd)
            return img_throw(ctx, exc,
                             wd.error() == img::webp::Error::TooManyPixels
                                 ? img::ERR_TOO_MANY_PIXELS
                                 : img::ERR_DECODE);
        img::Decoded d{};
        d.rgba = std::move(wd->rgba);
        d.width = wd->width;
        d.height = wd->height;
        d.iccProfile = std::move(wd->iccProfile);
        decoded = std::move(d);
    } else if (fmt == "tiff" || fmt == "heic" || fmt == "avif") {
        // The JS layer lifts the CODE: prefix onto error.code (Image.rs
        // UnsupportedOnPlatform contract on Linux).
        return img_throw(ctx, exc,
                         std::string{"ERR_IMAGE_FORMAT_UNSUPPORTED: Image: "} + std::string{fmt}
                             + " decode requires macOS or Windows");
    } else {
        return img_throw(ctx, exc,
                         std::string{"Image: decode failed ("} + std::string{fmt}
                             + " is not supported by mbun yet)");
    }
    if (!decoded) return img_throw(ctx, exc, decoded.error());

    // rotate → flip → flop (pure permutations; dims swap on 90/270).
    const auto rotate{static_cast<std::uint32_t>(img_prop_num(ctx, opts, "rotate", 0))};
    if (rotate == 90 || rotate == 180 || rotate == 270) {
        decoded->rgba = img::rotate_rgba8(decoded->rgba, decoded->width, decoded->height, rotate);
        if (rotate != 180) std::swap(decoded->width, decoded->height);
    }
    if (img_prop_bool(ctx, opts, "flip", false))
        decoded->rgba = img::flip_rgba8(decoded->rgba, decoded->width, decoded->height, false);
    if (img_prop_bool(ctx, opts, "flop", false))
        decoded->rgba = img::flip_rgba8(decoded->rgba, decoded->width, decoded->height, true);

    if (img_prop_bool(ctx, opts, "hasResize", false)) {
        const auto sw{decoded->width};
        const auto sh{decoded->height};
        // resolve_resize (Image.rs): h==0 preserves aspect; fit:'inside'
        // shrinks the box to the source aspect; withoutEnlargement bails.
        auto w{static_cast<std::uint64_t>(img_prop_num(ctx, opts, "rw", 0))};
        auto h{static_cast<std::uint64_t>(img_prop_num(ctx, opts, "rh", 0))};
        if (h == 0) h = std::min<std::uint64_t>(0x3FFFF, std::max<std::uint64_t>(1, w * sh / sw));
        if (img_prop_bool(ctx, opts, "fitInside", false)) {
            const double s{std::min(static_cast<double>(w) / sw, static_cast<double>(h) / sh)};
            w = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::llround(sw * s)));
            h = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::llround(sh * s)));
        }
        if (img_prop_bool(ctx, opts, "withoutEnlargement", false) && (w > sw || h > sh)) {
            w = sw;
            h = sh;
        }
        // Guard the output canvas AND the H-then-V intermediate (dst_w×src_h) —
        // a 1×N source resized to W×1 has tiny in/out canvases yet a W×N
        // intermediate (Image.rs apply_pipeline comment).
        if (w * h > maxPixels || w * sh > maxPixels)
            return img_throw(ctx, exc, img::ERR_TOO_MANY_PIXELS);
        const auto filter{img::filter_from_name(img_prop_str(ctx, opts, "filter", "lanczos3"))};
        if (!filter) return img_throw(ctx, exc, "Image: unknown resize filter");
        if (w != sw || h != sh) {
            decoded->rgba = img::resize_rgba8(decoded->rgba, sw, sh,
                                              static_cast<std::uint32_t>(w),
                                              static_cast<std::uint32_t>(h), *filter);
            decoded->width = static_cast<std::uint32_t>(w);
            decoded->height = static_cast<std::uint32_t>(h);
        }
    }

    if (img_prop_bool(ctx, opts, "hasModulate", false)) {
        img::modulate_rgba8(decoded->rgba.data(), decoded->rgba.size(),
                            static_cast<float>(img_prop_num(ctx, opts, "brightness", 1.0)),
                            static_cast<float>(img_prop_num(ctx, opts, "saturation", 1.0)));
    }

    // "" ⇒ re-encode in the source format (png here; bmp/tiff/gif would fall
    // back to png the way Image.rs does).
    std::string outFmt{img_prop_str(ctx, opts, "output", "")};
    if (outFmt.empty()) outFmt = fmt == "jpeg" ? "jpeg" : "png";
    const int level{static_cast<int>(img_prop_num(ctx, opts, "compressionLevel", -1))};
    const std::vector<std::uint8_t>* icc{decoded->iccProfile ? &*decoded->iccProfile : nullptr};
    std::vector<std::uint8_t> out{};
    if (outFmt == "jpeg") {
        const int quality{static_cast<int>(img_prop_num(ctx, opts, "quality", 80))};
        const bool progressive{img_prop_bool(ctx, opts, "progressive", false)};
        out = img::jpg::jpeg_encode_rgba(decoded->rgba, decoded->width, decoded->height, quality,
                                         progressive, icc);
        JSObjectRef res{JSObjectMake(ctx, nullptr, nullptr)};
        set_prop(ctx, res, "b64", make_string(ctx, zlib_b64_encode(out)));
        set_prop(ctx, res, "width", JSValueMakeNumber(ctx, decoded->width));
        set_prop(ctx, res, "height", JSValueMakeNumber(ctx, decoded->height));
        set_prop(ctx, res, "format", make_string(ctx, "jpeg"));
        return res;
    }
    if (outFmt == "webp") {
        // Flat RGBA → VP8/VP8L via libwebp (WebPEncodeRGBA / lossless variant).
        // ICCP mux (VP8X wrapper) is a backend-side DEFERRED; a bare bitstream
        // is emitted here, so profile carry-through does not apply yet.
        img::webp::EncodeOptions wopts{};
        wopts.quality = static_cast<std::uint8_t>(
            std::clamp(img_prop_num(ctx, opts, "quality", 80), 0.0, 100.0));
        wopts.lossless = img_prop_bool(ctx, opts, "lossless", false);
        auto enc{img::webp::encode(decoded->rgba, decoded->width, decoded->height, wopts)};
        if (!enc) return img_throw(ctx, exc, img::ERR_DECODE);
        JSObjectRef res{JSObjectMake(ctx, nullptr, nullptr)};
        set_prop(ctx, res, "b64", make_string(ctx, zlib_b64_encode(enc->bytes)));
        set_prop(ctx, res, "width", JSValueMakeNumber(ctx, decoded->width));
        set_prop(ctx, res, "height", JSValueMakeNumber(ctx, decoded->height));
        set_prop(ctx, res, "format", make_string(ctx, "webp"));
        return res;
    }
    if (outFmt != "png") {
        return img_throw(ctx, exc,
                         "Image: encoding " + outFmt + " is not supported by mbun yet");
    }
    if (img_prop_bool(ctx, opts, "palette", false)) {
        const auto colors{static_cast<std::uint32_t>(img_prop_num(ctx, opts, "colors", 256))};
        out = img::png_encode_indexed(decoded->rgba, decoded->width, decoded->height, level,
                                      colors, img_prop_bool(ctx, opts, "dither", false), icc);
    } else {
        out = img::png_encode_rgba(decoded->rgba, decoded->width, decoded->height, level, icc);
    }
    JSObjectRef res{JSObjectMake(ctx, nullptr, nullptr)};
    set_prop(ctx, res, "b64", make_string(ctx, zlib_b64_encode(out)));
    set_prop(ctx, res, "width", JSValueMakeNumber(ctx, decoded->width));
    set_prop(ctx, res, "height", JSValueMakeNumber(ctx, decoded->height));
    set_prop(ctx, res, "format", make_string(ctx, outFmt));
    return res;
}

// ── native filesystem operations (real; node:fs + fs/promises build on these) ──
// Exposed as globalThis.__mbunFsNative; the JS layer shapes node:fs (sync) and
// fs/promises (async) around them. Errors throw a JS Error (node-ish messages).

std::string fs_arg(JSContextRef ctx, std::size_t argc, const JSValueRef args[], std::size_t i) {
    return arg_string(ctx, argc, args, i);
}
// libuv-style negative errno for the common POSIX fs error codes (node exposes
// these on error.errno; ref: deps/uv/include/uv.h UV_E*). Used when a native fs
// message carries an "E...:" prefix so the thrown Error mirrors node's shape.
int fs_errno_for(std::string_view code) {
    if (code == "ENOENT") return -2;
    if (code == "EEXIST") return -17;
    if (code == "ENOTDIR") return -20;
    if (code == "EISDIR") return -21;
    if (code == "EACCES") return -13;
    if (code == "EPERM") return -1;
    if (code == "ENAMETOOLONG") return -36;
    if (code == "EBADF") return -9;
    if (code == "ELOOP") return -40;
    if (code == "ENOTEMPTY") return -39;
    if (code == "EINVAL") return -22;
    // libuv on Linux defines UV_E* as -E*, so the remaining codes fd_errno_code
    // can produce map straight through. Without them a real errno (EXDEV from a
    // cross-device rename, EMLINK from a link(2) at the link limit, EROFS from a
    // chmod on a read-only mount) reached JS with `errno === 0`, which node never
    // reports — uvException always carries the negative uv code.
    if (code == "ENXIO") return -6;
    if (code == "EAGAIN") return -11;
    if (code == "EBUSY") return -16;
    if (code == "EXDEV") return -18;
    if (code == "ENODEV") return -19;
    if (code == "ENFILE") return -23;
    if (code == "EMFILE") return -24;
    if (code == "EFBIG") return -27;
    if (code == "ENOSPC") return -28;
    if (code == "EROFS") return -30;
    if (code == "EMLINK") return -31;
    if (code == "EOVERFLOW") return -75;
    return 0;
}
// errno → node's "EINVAL" / "invalid argument" pair. Defined further down this TU
// (with the fd-level bindings); forward-declared here so the path-level bindings
// above can render node's exact "<CODE>: <text>, <syscall> '<path>'" instead of
// collapsing every unexpected errno onto one hand-picked code.
std::string fd_errno_code(int e);
std::string fd_errno_message(int e);
// node fs errors are Error instances carrying .code/.errno/.syscall/.path — not
// bare strings. Callers pass a node-ish "ECODE: message, syscall 'path'" text;
// we build the Error and populate .code + .errno from the leading token so
// `.toThrow(Error)` / `err.code` assertions pass (ref: lib/internal/fs/utils.js
// uvException). syscall/path are best-effort parsed from the message tail.
JSValueRef fs_make_error(JSContextRef ctx, const std::string& msg) {
    JSValueRef m{make_string(ctx, msg)};
    JSObjectRef e{JSObjectMakeError(ctx, 1, &m, nullptr)};
    // node builds this error with plain JS property SETS (lib/internal/errors.js
    // uvException does `err.errno = …`), so a poisoned Object.prototype accessor
    // for errno/code/syscall/path throws and THAT error is what the caller must
    // see. set_prop discards the setter's exception, which turned
    // test-fs-writesync-crash's expected error into an EBADF report.
    JSValueRef setExc{nullptr};
    const auto put{[&](const char* name, JSValueRef v) {
        if (setExc != nullptr) return;
        JSStringRef n{JSStringCreateWithUTF8CString(name)};
        JSObjectSetProperty(ctx, e, n, v, kJSPropertyAttributeNone, &setExc);
        JSStringRelease(n);
    }};
    auto colon{msg.find(':')};
    if (colon != std::string::npos && colon > 0 && msg[0] == 'E') {
        std::string code{msg.substr(0, colon)};
        bool ok{true};
        for (char c : code)
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) { ok = false; break; }
        if (ok) {
            put("code", make_string(ctx, code));
            put("errno", JSValueMakeNumber(ctx, fs_errno_for(code)));
        }
    }
    // ", <syscall> '<path>'" tail → syscall + path (node uvException layout).
    auto comma{msg.rfind(", ")};
    auto quote{msg.find(" '", colon == std::string::npos ? 0 : colon)};
    if (comma != std::string::npos && quote != std::string::npos && quote > comma + 2) {
        std::string syscall{msg.substr(comma + 2, quote - (comma + 2))};
        if (!syscall.empty() && syscall.find(' ') == std::string::npos)
            put("syscall", make_string(ctx, syscall));
        auto endq{msg.rfind('\'')};
        if (endq != std::string::npos && endq > quote + 2) {
            const std::string tail{msg.substr(quote + 2, endq - (quote + 2))};
            // Two-path syscalls (link/rename/symlink/copyfile) render as
            // "…, <syscall> 'src' -> 'dest'". node's uvException splits that into
            // .path and .dest; treating the whole tail as one path produced the
            // mangled `"src' -> 'dest"` and left .dest undefined.
            const auto arrow{tail.find("' -> '")};
            if (arrow != std::string::npos) {
                put("path", make_string(ctx, tail.substr(0, arrow)));
                put("dest", make_string(ctx, tail.substr(arrow + 6)));
            } else {
                put("path", make_string(ctx, tail));
            }
        }
    } else if (comma != std::string::npos && quote == std::string::npos) {
        // A pathless message ("EBADF: bad file descriptor, fstat") still names a
        // syscall — node's uvException fills .syscall whether or not there is a
        // path, and test-fs-error-messages asserts err.syscall === 'fstat'.
        std::string syscall{msg.substr(comma + 2)};
        if (!syscall.empty() && syscall.find(' ') == std::string::npos
            && syscall.find('\'') == std::string::npos)
            put("syscall", make_string(ctx, syscall));
    }
    if (setExc != nullptr) return setExc;
    return e;
}
JSValueRef fs_throw(JSContextRef ctx, JSValueRef* exc, const std::string& msg) {
    if (exc != nullptr) *exc = fs_make_error(ctx, msg);
    return JSValueMakeUndefined(ctx);
}

// bun rejects a path whose UTF-8 byte length exceeds MAX_PATH_BYTES (4096 on
// Linux) with ENAMETOOLONG BEFORE the syscall (which would otherwise surface
// ENOENT or a non-node message). ref: bun types.rs Valid::path_slice +
// util.rs MAX_PATH_BYTES. Returns true (and sets exc) when the path is too long.
bool fs_path_too_long(JSContextRef ctx, JSValueRef* exc, const std::string& path, const char* op) {
    if (path.size() > 4096) {
        fs_throw(ctx, exc, std::string{"ENAMETOOLONG: name too long, "} + op + " '" + path + "'");
        return true;
    }
    return false;
}


// ── the Permission Model gate ────────────────────────────────────────────────
// node enforces --permission/--allow-fs-* inside src/node_file.cc, at the point
// where the path reaches the syscall. THIS block is mbun's equivalent boundary:
// `__mbunFsNative` / `__mbunFdNative` are the only ways any JS in this runtime
// reaches the filesystem (node:fs, fs/promises, Bun.file, the module loader all
// funnel here), so a check placed in the JS `fs` shim could be walked around by
// calling these directly while a check placed HERE cannot.
//
// The read/write classification is taken function-for-function from node
// (node_file.cc's THROW_IF_INSUFFICIENT_PERMISSIONS call sites); where node
// gates two scopes for one call (rename, copyFile, symlink) both are gated here,
// in node's order, because the ORDER decides which error a script sees.
//
// Every gate is a no-op unless the model is enabled — see permission_deny_fs.
JSValueRef fsn_readFile_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                           const JSValueRef args[], JSValueRef* exc) {
    std::string path{fs_arg(ctx, argc, args, 0)};
    if (permission_deny_fs(ctx, exc, path, false)) return JSValueMakeUndefined(ctx);
    if (fs_path_too_long(ctx, exc, path, "open")) return JSValueMakeUndefined(ctx);
    if (auto c{read_file(path)}) return make_string(ctx, *c);
    return fs_throw(ctx, exc, "ENOENT: no such file or directory, open '" + path + "'");
}
JSValueRef fsn_writeFile_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                            const JSValueRef args[], JSValueRef* exc) {
    std::string path{fs_arg(ctx, argc, args, 0)};
    if (permission_deny_fs(ctx, exc, path, true)) return JSValueMakeUndefined(ctx);
    std::string data{fs_arg(ctx, argc, args, 1)};
    std::ofstream f{std::filesystem::path{path}, std::ios::binary | std::ios::trunc};
    if (!f) return fs_throw(ctx, exc, "EACCES: cannot write '" + path + "'");
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    return JSValueMakeUndefined(ctx);
}
JSValueRef fsn_appendFile_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                             const JSValueRef args[], JSValueRef* exc) {
    std::string path{fs_arg(ctx, argc, args, 0)};
    if (permission_deny_fs(ctx, exc, path, true)) return JSValueMakeUndefined(ctx);
    std::string data{fs_arg(ctx, argc, args, 1)};
    std::ofstream f{std::filesystem::path{path}, std::ios::binary | std::ios::app};
    if (!f) return fs_throw(ctx, exc, "EACCES: cannot append '" + path + "'");
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    return JSValueMakeUndefined(ctx);
}
JSValueRef fsn_exists_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                         const JSValueRef args[], JSValueRef* exc) {
    const std::string path{fs_arg(ctx, argc, args, 0)};
    // node ExistsSync gates fs.read. fs.existsSync swallows the throw and
    // reports false; fs.accessSync (which is built on this probe in mbun) lets
    // the ERR_ACCESS_DENIED out, which is what the corpus asserts.
    if (permission_deny_fs(ctx, exc, path, false)) return JSValueMakeUndefined(ctx);
    std::error_code ec;
    return JSValueMakeBoolean(ctx, std::filesystem::exists(std::filesystem::path{path}, ec));
}
// node fs.mkdir(path, recursive, mode). `mode` is honored (default 0o777):
// POSIX mkdir(2) applies the process umask and keeps the sticky/setgid bits, so
// mode 0o1777 yields a sticky dir masked to ~umask (fs-mkdir "honoring mode
// bits above 0o777"). A path component that is a file surfaces ENOTDIR; an
// existing target surfaces EEXIST. ref: bun src/sys.rs mkdirRecursive.
JSValueRef fsn_mkdir_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                        const JSValueRef args[], JSValueRef* exc) {
    std::string path{fs_arg(ctx, argc, args, 0)};
    if (permission_deny_fs(ctx, exc, path, true)) return JSValueMakeUndefined(ctx);
    bool recursive{argc > 1 && JSValueToBoolean(ctx, args[1])};
    const unsigned mode{(argc > 2 && JSValueIsNumber(ctx, args[2]))
                            ? (static_cast<unsigned>(JSValueToNumber(ctx, args[2], nullptr)) & 07777u)
                            : 0777u};
#if defined(_WIN32)
    (void)mode;
    std::error_code ec;
    std::filesystem::path p{path};
    if (recursive) {
        if (std::filesystem::exists(p, ec)) {
            if (std::filesystem::is_directory(p, ec)) return JSValueMakeUndefined(ctx);
            return fs_throw(ctx, exc, "EEXIST: file already exists, mkdir '" + path + "'");
        }
        std::filesystem::path first{p}, parent{p.parent_path()};
        while (!parent.empty() && !std::filesystem::exists(parent, ec)) { first = parent; parent = parent.parent_path(); }
        std::filesystem::create_directories(p, ec);
        if (ec) return fs_throw(ctx, exc, "mkdir '" + path + "': " + ec.message());
        return make_string(ctx, first.string());
    }
    if (std::filesystem::exists(p, ec))
        return fs_throw(ctx, exc, "EEXIST: file already exists, mkdir '" + path + "'");
    std::filesystem::create_directory(p, ec);
    if (ec) return fs_throw(ctx, exc, "mkdir '" + path + "': " + ec.message());
    return JSValueMakeUndefined(ctx);
#else
    struct stat st{};
    if (!recursive) {
        if (::mkdir(path.c_str(), static_cast<mode_t>(mode)) != 0) {
            if (errno == EEXIST)
                return fs_throw(ctx, exc, "EEXIST: file already exists, mkdir '" + path + "'");
            if (errno == ENOTDIR)
                return fs_throw(ctx, exc, "ENOTDIR: not a directory, mkdir '" + path + "'");
            if (errno == ENAMETOOLONG)
                return fs_throw(ctx, exc, "ENAMETOOLONG: name too long, mkdir '" + path + "'");
            if (errno == ENOENT)
                return fs_throw(ctx, exc, "ENOENT: no such file or directory, mkdir '" + path + "'");
            return fs_throw(ctx, exc, "EACCES: permission denied, mkdir '" + path + "'");
        }
        return JSValueMakeUndefined(ctx);
    }
    // recursive: an existing directory is a no-op; an existing non-directory
    // (or a file component mid-path) is an error.
    if (::stat(path.c_str(), &st) == 0) {
        if ((st.st_mode & S_IFMT) == S_IFDIR) return JSValueMakeUndefined(ctx);
        return fs_throw(ctx, exc, "EEXIST: file already exists, mkdir '" + path + "'");
    }
    // Find the first non-existent ancestor (node returns it), then create the
    // chain top-down. A file component surfaces ENOTDIR from mkdir.
    std::filesystem::path p{path};
    std::vector<std::filesystem::path> chain;
    for (std::filesystem::path cur{p}; !cur.empty() && cur != cur.root_path(); cur = cur.parent_path()) {
        struct stat cs{};
        if (::stat(cur.c_str(), &cs) == 0) break;
        chain.push_back(cur);
    }
    std::string first{chain.empty() ? path : chain.back().string()};
    for (auto it{chain.rbegin()}; it != chain.rend(); ++it) {
        if (::mkdir(it->c_str(), static_cast<mode_t>(mode)) != 0 && errno != EEXIST) {
            if (errno == ENOTDIR)
                return fs_throw(ctx, exc, "ENOTDIR: not a directory, mkdir '" + it->string() + "'");
            if (errno == ENOENT)
                return fs_throw(ctx, exc, "ENOENT: no such file or directory, mkdir '" + it->string() + "'");
            if (errno == ENAMETOOLONG)
                return fs_throw(ctx, exc, "ENAMETOOLONG: name too long, mkdir '" + it->string() + "'");
            return fs_throw(ctx, exc, "EACCES: permission denied, mkdir '" + it->string() + "'");
        }
    }
    return make_string(ctx, first);
#endif
}
JSValueRef fsn_rm_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                     const JSValueRef args[], JSValueRef* exc) {
    std::string path{fs_arg(ctx, argc, args, 0)};
    if (permission_deny_fs(ctx, exc, path, true)) return JSValueMakeUndefined(ctx);
    bool recursive{argc > 1 && JSValueToBoolean(ctx, args[1])};
    bool force{argc > 2 && JSValueToBoolean(ctx, args[2])};
    std::error_code ec;
    if (recursive) {
        std::filesystem::remove_all(std::filesystem::path{path}, ec);
    } else {
        std::filesystem::remove(std::filesystem::path{path}, ec);
    }
    // node's { force: true } suppresses ONLY "the path does not exist" — every
    // other failure still throws (lib/internal/fs/rimraf.js rethrows anything
    // that is not ENOENT). Swallowing them all made rmSync(<file in a read-only
    // directory>, { force: true }) a silent no-op where node reports EACCES
    // (test-fs-rm, nodejs/node#38683).
    if (ec && !(force && ec.value() == ENOENT)) {
        // node-shaped errno so `err.code` is usable (a bare message left
        // fs.rm/rmdir callers with `code: undefined`).
        switch (ec.value()) {
            case ENOENT:
                return fs_throw(ctx, exc, "ENOENT: no such file or directory, unlink '" + path + "'");
            case ENOTEMPTY:
                return fs_throw(ctx, exc, "ENOTEMPTY: directory not empty, rmdir '" + path + "'");
            case ENOTDIR:
                return fs_throw(ctx, exc, "ENOTDIR: not a directory, rmdir '" + path + "'");
            case EPERM:
                return fs_throw(ctx, exc, "EPERM: operation not permitted, unlink '" + path + "'");
            case EBUSY:
                return fs_throw(ctx, exc, "EBUSY: resource busy or locked, unlink '" + path + "'");
            default:
                return fs_throw(ctx, exc, "EACCES: permission denied, unlink '" + path + "'");
        }
    }
    return JSValueMakeUndefined(ctx);
}
// rmdir(2). std::filesystem::remove cannot express node's error surface —
// it reports "directory not empty" as a generic error_code and a missing path
// as `false` with no error at all — so fs.rmdirSync had no ENOENT / ENOTDIR /
// ENOTEMPTY to report. Go straight to the syscall and format node's message.
JSValueRef fsn_rmdir_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                        const JSValueRef args[], JSValueRef* exc) {
    std::string path{fs_arg(ctx, argc, args, 0)};
    if (permission_deny_fs(ctx, exc, path, true)) return JSValueMakeUndefined(ctx);
#if defined(_WIN32)
    std::error_code ec;
    if (!std::filesystem::is_directory(std::filesystem::path{path}, ec))
        return fs_throw(ctx, exc, "ENOENT: no such file or directory, rmdir '" + path + "'");
    std::filesystem::remove(std::filesystem::path{path}, ec);
    if (ec) return fs_throw(ctx, exc, "ENOTEMPTY: directory not empty, rmdir '" + path + "'");
#else
    if (::rmdir(path.c_str()) != 0) {
        switch (errno) {
            case ENOENT:
                return fs_throw(ctx, exc, "ENOENT: no such file or directory, rmdir '" + path + "'");
            case ENOTDIR:
                return fs_throw(ctx, exc, "ENOTDIR: not a directory, rmdir '" + path + "'");
            case ENOTEMPTY:
            case EEXIST:
                return fs_throw(ctx, exc, "ENOTEMPTY: directory not empty, rmdir '" + path + "'");
            case EINVAL:
                return fs_throw(ctx, exc, "EINVAL: invalid argument, rmdir '" + path + "'");
            case EBUSY:
                return fs_throw(ctx, exc, "EBUSY: resource busy or locked, rmdir '" + path + "'");
            default:
                return fs_throw(ctx, exc, "EACCES: permission denied, rmdir '" + path + "'");
        }
    }
#endif
    return JSValueMakeUndefined(ctx);
}
// truncate(2) — fs.truncate/truncateSync were no-ops, so a corpus file that
// truncated and then re-read the file saw the original bytes.
JSValueRef fsn_truncate_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                           const JSValueRef args[], JSValueRef* exc) {
    std::string path{fs_arg(ctx, argc, args, 0)};
    if (permission_deny_fs(ctx, exc, path, true)) return JSValueMakeUndefined(ctx);
    const auto len{(argc > 1 && JSValueIsNumber(ctx, args[1]))
                       ? JSValueToNumber(ctx, args[1], nullptr)
                       : 0.0};
#if defined(_WIN32)
    std::error_code ec;
    std::filesystem::resize_file(std::filesystem::path{path},
                                 static_cast<std::uintmax_t>(len), ec);
    if (ec) return fs_throw(ctx, exc, "ENOENT: no such file or directory, open '" + path + "'");
#else
    if (::truncate(path.c_str(), static_cast<off_t>(len)) != 0) {
        // node reports the REAL errno (its syscall slot for truncate(2) is
        // "open", matching libuv's open-then-ftruncate implementation). Mapping
        // everything that was not ENOENT/EISDIR onto EACCES hid EINVAL/EFBIG.
        const int e{errno};
        const std::string code{fd_errno_code(e)};
        return fs_throw(ctx, exc, (code.empty() ? std::string{"EUNKNOWN"} : code) + ": "
                                      + fd_errno_message(e) + ", open '" + path + "'");
    }
#endif
    return JSValueMakeUndefined(ctx);
}
// statfs(2)/statvfs(3) — the JS layer previously returned a frozen fake row.
JSValueRef fsn_statfs_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                         const JSValueRef args[], JSValueRef* exc) {
    std::string path{fs_arg(ctx, argc, args, 0)};
    if (permission_deny_fs(ctx, exc, path, false)) return JSValueMakeUndefined(ctx);
    double type{0}, bsize{4096}, frsize{4096}, blocks{0}, bfree{0}, bavail{0}, files{0}, ffree{0};
#if !defined(_WIN32)
    struct statvfs sv{};
    if (::statvfs(path.c_str(), &sv) != 0)
        return fs_throw(ctx, exc, "ENOENT: no such file or directory, statfs '" + path + "'");
    bsize = static_cast<double>(sv.f_bsize);
    frsize = static_cast<double>(sv.f_frsize);
    blocks = static_cast<double>(sv.f_blocks);
    bfree = static_cast<double>(sv.f_bfree);
    bavail = static_cast<double>(sv.f_bavail);
    files = static_cast<double>(sv.f_files);
    ffree = static_cast<double>(sv.f_ffree);
    type = static_cast<double>(sv.f_fsid);
#else
    std::error_code ec;
    if (!std::filesystem::exists(std::filesystem::path{path}, ec))
        return fs_throw(ctx, exc, "ENOENT: no such file or directory, statfs '" + path + "'");
    const auto space{std::filesystem::space(std::filesystem::path{path}, ec)};
    blocks = static_cast<double>(space.capacity / 4096);
    bfree = static_cast<double>(space.free / 4096);
    bavail = static_cast<double>(space.available / 4096);
#endif
    JSObjectRef out{JSObjectMake(ctx, nullptr, nullptr)};
    set_prop(ctx, out, "type", JSValueMakeNumber(ctx, type));
    set_prop(ctx, out, "bsize", JSValueMakeNumber(ctx, bsize));
    // node's StatFs carries f_frsize as well (lib/internal/fs/utils.js
    // getStatFsFromBinding); omitting it failed test-fs-statfs's Object.hasOwn.
    set_prop(ctx, out, "frsize", JSValueMakeNumber(ctx, frsize));
    set_prop(ctx, out, "blocks", JSValueMakeNumber(ctx, blocks));
    set_prop(ctx, out, "bfree", JSValueMakeNumber(ctx, bfree));
    set_prop(ctx, out, "bavail", JSValueMakeNumber(ctx, bavail));
    set_prop(ctx, out, "files", JSValueMakeNumber(ctx, files));
    set_prop(ctx, out, "ffree", JSValueMakeNumber(ctx, ffree));
    (void)exc;
    return out;
}
JSValueRef fsn_readdir_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                          const JSValueRef args[], JSValueRef* exc) {
    std::string path{fs_arg(ctx, argc, args, 0)};
    if (permission_deny_fs(ctx, exc, path, false)) return JSValueMakeUndefined(ctx);
    std::error_code ec;
    // libuv's uv_fs_scandir sorts with alphasort (deps/uv/src/unix/fs.c), so
    // node's readdir order is deterministic and every corpus test that indexes
    // the result positionally relies on it (test-fs-readdir-types compares
    // dirents[i].name against a sorted literal). directory_iterator yields raw
    // getdents order, so the sort has to happen here — this is the one place
    // both node:fs and internalBinding('fs').readdir read the directory from.
    std::vector<std::string> raw;
    for (const auto& e : std::filesystem::directory_iterator{std::filesystem::path{path}, ec}) {
        raw.push_back(e.path().filename().string());
    }
    std::ranges::sort(raw);
    std::vector<JSValueRef> names;
    names.reserve(raw.size());
    for (const auto& n : raw) names.push_back(make_string(ctx, n));
    // node surfaces the real errno here (readdirSync on a file is ENOTDIR, not
    // ENOENT) — ref node test-fs-readdir-types.js:68.
    if (ec) {
        switch (ec.value()) {
            case ENOTDIR: return fs_throw(ctx, exc, "ENOTDIR: not a directory, scandir '" + path + "'");
            case EACCES:  return fs_throw(ctx, exc, "EACCES: permission denied, scandir '" + path + "'");
            case ELOOP:   return fs_throw(ctx, exc, "ELOOP: too many symbolic links encountered, scandir '" + path + "'");
            default: return fs_throw(ctx, exc, "ENOENT: no such file or directory, scandir '" + path + "'");
        }
    }
    return JSObjectMakeArray(ctx, names.size(), names.empty() ? nullptr : names.data(), nullptr);
}
// Numeric fields for a stat result, decoupled from the syscall source so stat /
// lstat / fstat share one JS-object builder. Times are node's fractional ms
// (sec*1000 + nsec/1e6). *Ns are integer nanoseconds for the bigint path.
struct FsStatFields {
    long long dev{0}, mode{0}, nlink{0}, uid{0}, gid{0}, rdev{0}, ino{0}, size{0}, blksize{0}, blocks{0};
    double atimeMs{0}, mtimeMs{0}, ctimeMs{0}, birthtimeMs{0};
    long long atimeNs{0}, mtimeNs{0}, ctimeNs{0}, birthtimeNs{0};
};
// Build the plain stat object node's JS wrapper reparents onto Stats.prototype.
// Field order + isX() bit-tests per node lib/internal/fs/utils.js Stats. The
// *Ns fields feed the JS bigint-stats builder (birthtimeNs precision).
JSValueRef fs_build_stat_object(JSContextRef ctx, const FsStatFields& f, JSValueRef* exc) {
    const auto num{[](double v) {
        std::ostringstream ss;
        ss.precision(17);
        ss << v;
        return ss.str();
    }};
    const std::string js{
        "(function(){var m=" + std::to_string(f.mode) + ";return{"
        "dev:" + std::to_string(f.dev) + ",mode:m,nlink:" + std::to_string(f.nlink) +
        ",uid:" + std::to_string(f.uid) + ",gid:" + std::to_string(f.gid) +
        ",rdev:" + std::to_string(f.rdev) + ",ino:" + std::to_string(f.ino) +
        ",size:" + std::to_string(f.size) +
        ",blksize:" + std::to_string(f.blksize) + ",blocks:" + std::to_string(f.blocks) + "," +
        "atimeMs:" + num(f.atimeMs) + ",mtimeMs:" + num(f.mtimeMs) + ",ctimeMs:" + num(f.ctimeMs) + ",birthtimeMs:" + num(f.birthtimeMs) +
        ",atimeNs:" + std::to_string(f.atimeNs) + ",mtimeNs:" + std::to_string(f.mtimeNs) +
        ",ctimeNs:" + std::to_string(f.ctimeNs) + ",birthtimeNs:" + std::to_string(f.birthtimeNs) +
        ",atime:new Date(" + num(f.atimeMs) + "),mtime:new Date(" + num(f.mtimeMs) + "),ctime:new Date(" + num(f.ctimeMs) + "),birthtime:new Date(" + num(f.birthtimeMs) + "),"
        "_isFile:(m&61440)===32768,_isDir:(m&61440)===16384,"  // legacy flags (readdir Dirent)
        "isFile(){return (m&61440)===32768;},isDirectory(){return (m&61440)===16384;},"
        "isSymbolicLink(){return (m&61440)===40960;},isBlockDevice(){return (m&61440)===24576;},"
        "isCharacterDevice(){return (m&61440)===8192;},isFIFO(){return (m&61440)===4096;},isSocket(){return (m&61440)===49152;}};})()"};
    JSStringRef src{JSStringCreateWithUTF8CString(js.c_str())};
    JSValueRef r{JSEvaluateScript(ctx, src, nullptr, nullptr, 1, exc)};
    JSStringRelease(src);
    return r;
}
#if !defined(_WIN32)
// Fill FsStatFields from a POSIX struct stat (birthtime falls back to ctime;
// real btime is filled by statx on Linux). Shared by lstat/fstat paths.
void fs_fields_from_stat(const struct stat& st, FsStatFields& f) {
    f.dev = static_cast<long long>(st.st_dev);
    f.mode = static_cast<long long>(st.st_mode);
    f.nlink = static_cast<long long>(st.st_nlink);
    f.uid = static_cast<long long>(st.st_uid);
    f.gid = static_cast<long long>(st.st_gid);
    f.rdev = static_cast<long long>(st.st_rdev);
    f.ino = static_cast<long long>(st.st_ino);
    f.size = static_cast<long long>(st.st_size);
    f.blksize = static_cast<long long>(st.st_blksize);
    f.blocks = static_cast<long long>(st.st_blocks);
    f.atimeNs = static_cast<long long>(st.st_atim.tv_sec) * 1000000000LL + st.st_atim.tv_nsec;
    f.mtimeNs = static_cast<long long>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
    f.ctimeNs = static_cast<long long>(st.st_ctim.tv_sec) * 1000000000LL + st.st_ctim.tv_nsec;
    f.atimeMs = static_cast<double>(st.st_atim.tv_sec) * 1000.0 + static_cast<double>(st.st_atim.tv_nsec) / 1e6;
    f.mtimeMs = static_cast<double>(st.st_mtim.tv_sec) * 1000.0 + static_cast<double>(st.st_mtim.tv_nsec) / 1e6;
    f.ctimeMs = static_cast<double>(st.st_ctim.tv_sec) * 1000.0 + static_cast<double>(st.st_ctim.tv_nsec) / 1e6;
    f.birthtimeMs = f.ctimeMs;  // overwritten by statx btime when available
    f.birthtimeNs = f.ctimeNs;
}
#endif
JSValueRef fsn_stat_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                       const JSValueRef args[], JSValueRef* exc) {
    std::string path{fs_arg(ctx, argc, args, 0)};
    if (permission_deny_fs(ctx, exc, path, false)) return JSValueMakeUndefined(ctx);
    if (fs_path_too_long(ctx, exc, path, "stat")) return JSValueMakeUndefined(ctx);
    // 2nd arg true = lstat (do NOT follow symlink) — node/bun keep stat vs lstat
    // distinct; the shared JS shim passes the flag.
    const bool useLstat{argc > 1 && JSValueToBoolean(ctx, args[1])};
    FsStatFields f{};
#if defined(_WIN32)
    struct _stat64 st{};
    if (_stat64(path.c_str(), &st) != 0)
        return fs_throw(ctx, exc, "ENOENT: no such file or directory, stat '" + path + "'");
    f.dev = static_cast<long long>(st.st_dev);
    f.mode = static_cast<long long>(st.st_mode);
    f.nlink = static_cast<long long>(st.st_nlink);
    f.uid = static_cast<long long>(st.st_uid);
    f.gid = static_cast<long long>(st.st_gid);
    f.rdev = static_cast<long long>(st.st_rdev);
    f.ino = static_cast<long long>(st.st_ino);
    f.size = static_cast<long long>(st.st_size);
    f.blksize = 4096;
    f.blocks = 0;
    f.mtimeMs = static_cast<double>(st.st_mtime) * 1000.0;
    f.atimeMs = static_cast<double>(st.st_atime) * 1000.0;
    f.ctimeMs = static_cast<double>(st.st_ctime) * 1000.0;
    f.birthtimeMs = f.ctimeMs;
    f.mtimeNs = static_cast<long long>(st.st_mtime) * 1000000000LL;
    f.atimeNs = static_cast<long long>(st.st_atime) * 1000000000LL;
    f.ctimeNs = static_cast<long long>(st.st_ctime) * 1000000000LL;
    f.birthtimeNs = f.ctimeNs;
#else
    struct stat st{};
    const int rc{useLstat ? ::lstat(path.c_str(), &st) : ::stat(path.c_str(), &st)};
    if (rc != 0) {
        // Report the syscall's own code: a path component longer than NAME_MAX
        // (or a whole path past the kernel's PATH_MAX) fails ENAMETOOLONG, and
        // node surfaces that rather than flattening every failure to ENOENT.
        if (errno == ENAMETOOLONG)
            return fs_throw(ctx, exc, "ENAMETOOLONG: name too long, stat '" + path + "'");
        if (errno == ENOTDIR)
            return fs_throw(ctx, exc, "ENOTDIR: not a directory, stat '" + path + "'");
        // A symlink cycle must surface as ELOOP, not ENOENT: node's non-native
        // realpath resolver detects a cycle ONLY by the follow-stat failing that
        // way (lib/fs.js realpathSync calls binding.stat on each link before
        // reading it), and test-fs-realpath's cyclic-link-protection case matches
        // on `{ code: 'ELOOP' }` exactly.
        if (errno == ELOOP)
            return fs_throw(ctx, exc, "ELOOP: too many symbolic links encountered, stat '" + path + "'");
        // Everything else keeps its own errno too. Flattening the tail onto
        // ENOENT told fs.rm that a file it could not lstat for lack of directory
        // search permission (EACCES) simply did not exist, so { force: true }
        // silently skipped it (test-fs-rm).
        if (const std::string code{fd_errno_code(errno)}; !code.empty() && errno != ENOENT)
            return fs_throw(ctx, exc, code + ": " + fd_errno_message(errno) + ", stat '" + path + "'");
        return fs_throw(ctx, exc, "ENOENT: no such file or directory, stat '" + path + "'");
    }
    fs_fields_from_stat(st, f);
    // Linux struct stat has no st_birthtim; statx STATX_BTIME gives the real
    // creation time (node uses it — src/sys.rs). Fall back to ctime otherwise.
    struct statx stx{};
    if (::statx(AT_FDCWD, path.c_str(), useLstat ? AT_SYMLINK_NOFOLLOW : 0, STATX_BTIME, &stx) == 0
        && (stx.stx_mask & STATX_BTIME)) {
        f.birthtimeNs = static_cast<long long>(stx.stx_btime.tv_sec) * 1000000000LL + stx.stx_btime.tv_nsec;
        f.birthtimeMs = static_cast<double>(stx.stx_btime.tv_sec) * 1000.0 + static_cast<double>(stx.stx_btime.tv_nsec) / 1e6;
    }
#endif
    return fs_build_stat_object(ctx, f, exc);
}
// node fs.fstatSync(fd): stat over an open descriptor. Real fields (birthtime
// via statx on the fd) so createReadStream/FileHandle.stat report the same
// birthtime as stat/lstat (fs-birthtime-linux).
// The fd_table maps the JS-visible virtual fd (fd native, counter from 1000) to
// the owning io::File; defined further down this TU. Forward-declared so fstat
// can resolve the real OS descriptor.
std::unordered_map<int, mbun::core::io::File>& fd_table();
JSValueRef fsn_fstat_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                        const JSValueRef args[], JSValueRef* exc) {
    const int vfd{(argc > 0 && JSValueIsNumber(ctx, args[0])) ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    FsStatFields f{};
    auto& tbl{fd_table()};
    auto it{tbl.find(vfd)};
#if defined(_WIN32)
    if (it == tbl.end() || !it->second.is_open())
        return fs_throw(ctx, exc, "EBADF: bad file descriptor, fstat");
    (void)f;
    return fs_throw(ctx, exc, "EBADF: bad file descriptor, fstat");
#else
    // An fd mbun did not hand out is not automatically bad: stdio (0/1/2) and
    // any inherited descriptor are real and node fstats them happily. Only a
    // descriptor that is open at neither level is EBADF. `fs.fstat(0, …)` used
    // to throw here (test-fs-stat).
    int fd{-1};
    if (it != tbl.end() && it->second.is_open()) {
        fd = it->second.native_fd();
    } else if (vfd >= 0 && ::fcntl(vfd, F_GETFD) != -1) {
        fd = vfd;
    } else {
        return fs_throw(ctx, exc, "EBADF: bad file descriptor, fstat");
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0)
        return fs_throw(ctx, exc, "EBADF: bad file descriptor, fstat");
    fs_fields_from_stat(st, f);
    struct statx stx{};
    if (::statx(fd, "", AT_EMPTY_PATH, STATX_BTIME, &stx) == 0 && (stx.stx_mask & STATX_BTIME)) {
        f.birthtimeNs = static_cast<long long>(stx.stx_btime.tv_sec) * 1000000000LL + stx.stx_btime.tv_nsec;
        f.birthtimeMs = static_cast<double>(stx.stx_btime.tv_sec) * 1000.0 + static_cast<double>(stx.stx_btime.tv_nsec) / 1e6;
    }
    return fs_build_stat_object(ctx, f, exc);
#endif
}
// node fs.readlinkSync raw target: the link's own target string (NOT resolved
// via realpath). fs.cp preserves this verbatim; readlink of a copied link must
// return the original target, not the source symlink path (cp-symlink-target).
JSValueRef fsn_readlink_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                           const JSValueRef args[], JSValueRef* exc) {
    std::string path{fs_arg(ctx, argc, args, 0)};
    if (permission_deny_fs(ctx, exc, path, false)) return JSValueMakeUndefined(ctx);
    // Report the syscall's OWN errno. read_symlink's std::error_code carries it,
    // but flattening every failure onto EINVAL made readlink of a MISSING path
    // report "EINVAL: invalid argument" where node reports ENOENT — the last
    // assertion blocking test-fs-error-messages.
    std::error_code ec;
    auto target{std::filesystem::read_symlink(std::filesystem::path{path}, ec)};
    if (ec) {
        const int e{ec.value()};
        const std::string code{fd_errno_code(e)};
        return fs_throw(ctx, exc, (code.empty() ? std::string{"EINVAL"} : code) + ": "
                                      + fd_errno_message(e) + ", readlink '" + path + "'");
    }
    return make_string(ctx, target.string());
}
// node process.umask([mask]): read (no arg) or set the process file-mode mask.
// mkdir(2)/open(2) honor the real OS umask, so process.umask must reflect it —
// a JS-emulated default would disagree with created file modes (fs-mkdir mode
// bits). Read is umask(x) then restore. ref: bun src/runtime/node/process.
JSValueRef fsn_umask_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                        const JSValueRef args[], JSValueRef*) {
#if defined(_WIN32)
    return JSValueMakeNumber(ctx, 0);
#else
    if (argc > 0 && JSValueIsNumber(ctx, args[0])) {
        const mode_t prev{::umask(static_cast<mode_t>(JSValueToNumber(ctx, args[0], nullptr)) & 07777u)};
        return JSValueMakeNumber(ctx, static_cast<double>(prev));
    }
    const mode_t cur{::umask(022)};
    ::umask(cur);
    return JSValueMakeNumber(ctx, static_cast<double>(cur));
#endif
}
// node fs.utimesSync(path, atime, mtime): atime/mtime are seconds (may be
// fractional). Uses utimensat for nanosecond precision (node/bun preserve
// sub-ms — src/sys uses the *_nsec fields). No-op stub lost the fraction.
// `flags` is utimensat's flag word: 0 for utimes (follow the link) and
// AT_SYMLINK_NOFOLLOW for lutimes. Both entry points share this body so they
// share ONE Permission Model gate (permission_deny_fs, write scope) — the same
// gate node applies in node_file.cc's UTimes/LUTimes.
JSValueRef fsn_utimes_impl_(JSContextRef ctx, std::size_t argc, const JSValueRef args[],
                            JSValueRef* exc, int flags, const char* syscall) {
    std::string path{fs_arg(ctx, argc, args, 0)};
    if (permission_deny_fs(ctx, exc, path, true)) return JSValueMakeUndefined(ctx);
#if !defined(_WIN32)
    const double aSec{argc > 1 ? JSValueToNumber(ctx, args[1], nullptr) : 0.0};
    const double mSec{argc > 2 ? JSValueToNumber(ctx, args[2], nullptr) : 0.0};
    const auto toTs{[](double sec) {
        struct timespec ts{};
        double whole{std::floor(sec)};
        ts.tv_sec = static_cast<std::time_t>(whole);
        long nsec{static_cast<long>((sec - whole) * 1e9 + 0.5)};
        if (nsec >= 1000000000L) { ts.tv_sec += 1; nsec -= 1000000000L; }
        ts.tv_nsec = nsec;
        return ts;
    }};
    struct timespec times[2]{toTs(aSec), toTs(mSec)};
    if (::utimensat(AT_FDCWD, path.c_str(), times, flags) != 0) {
        const int e{errno};
        const std::string code{fd_errno_code(e)};
        return fs_throw(ctx, exc, (code.empty() ? std::string{"EUNKNOWN"} : code) + ": "
                                      + fd_errno_message(e) + ", " + syscall + " '" + path + "'");
    }
#else
    (void)flags;
    (void)syscall;
#endif
    return JSValueMakeUndefined(ctx);
}
JSValueRef fsn_utimes_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                         const JSValueRef args[], JSValueRef* exc) {
    return fsn_utimes_impl_(ctx, argc, args, exc, 0, "utime");
}
// node fs.lutimes / fs.lutimesSync: utimensat with AT_SYMLINK_NOFOLLOW, so the
// timestamps land on the SYMLINK rather than on whatever it points at. Without
// its own entry point the JS shim fell back to fsn_utimes_cb and followed the
// link (test-fs-utimes stamped the target and then failed lstat's mtime check).
JSValueRef fsn_lutimes_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                          const JSValueRef args[], JSValueRef* exc) {
    return fsn_utimes_impl_(ctx, argc, args, exc, AT_SYMLINK_NOFOLLOW, "lutime");
}
JSValueRef fsn_unlink_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                         const JSValueRef args[], JSValueRef* exc) {
    std::string path{fs_arg(ctx, argc, args, 0)};
    if (permission_deny_fs(ctx, exc, path, true)) return JSValueMakeUndefined(ctx);
    // std::filesystem::remove reports "nothing to remove" by returning FALSE with
    // an EMPTY error_code, so unlinkSync(<nonexistent>) silently succeeded — node
    // throws ENOENT. Go straight to unlink(2): it also keeps EISDIR/EPERM/EACCES
    // distinct, which `remove` (which happily rmdir's a directory) does not.
#if defined(_WIN32)
    std::error_code ec;
    if (!std::filesystem::remove(std::filesystem::path{path}, ec) || ec) {
        const int e{ec ? ec.value() : ENOENT};
        const std::string code{fd_errno_code(e)};
        return fs_throw(ctx, exc, (code.empty() ? std::string{"EPERM"} : code) + ": "
                                      + fd_errno_message(e) + ", unlink '" + path + "'");
    }
#else
    if (::unlink(path.c_str()) != 0) {
        const int e{errno};
        const std::string code{fd_errno_code(e)};
        return fs_throw(ctx, exc, (code.empty() ? std::string{"EPERM"} : code) + ": "
                                      + fd_errno_message(e) + ", unlink '" + path + "'");
    }
#endif
    return JSValueMakeUndefined(ctx);
}
// node fs.linkSync(existingPath, newPath): a HARD link, i.e. link(2). This was
// implemented as copy_file, which is not the same thing at all — the copy has
// its own inode, so nlink never rises, writes through one path are invisible
// through the other, and every error carried syscall 'copyfile' with the two
// paths mangled into one .path string.
JSValueRef fsn_link_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                       const JSValueRef args[], JSValueRef* exc) {
    const std::string oldPath{fs_arg(ctx, argc, args, 0)};
    const std::string newPath{fs_arg(ctx, argc, args, 1)};
    // node Link (node_file.cc): read(src), write(src), write(dest) — the source
    // is gated for WRITE too because a new hard link to it is a second writable
    // handle on the same inode.
    if (permission_deny_fs(ctx, exc, oldPath, false)) return JSValueMakeUndefined(ctx);
    if (permission_deny_fs(ctx, exc, oldPath, true)) return JSValueMakeUndefined(ctx);
    if (permission_deny_fs(ctx, exc, newPath, true)) return JSValueMakeUndefined(ctx);
#if defined(_WIN32)
    std::error_code ec;
    std::filesystem::create_hard_link(std::filesystem::path{oldPath},
                                      std::filesystem::path{newPath}, ec);
    if (ec) {
        const int e{ec.value()};
        const std::string code{fd_errno_code(e)};
        return fs_throw(ctx, exc, (code.empty() ? std::string{"EPERM"} : code) + ": "
                                      + fd_errno_message(e) + ", link '" + oldPath + "' -> '" + newPath + "'");
    }
#else
    if (::link(oldPath.c_str(), newPath.c_str()) != 0) {
        const int e{errno};
        const std::string code{fd_errno_code(e)};
        return fs_throw(ctx, exc, (code.empty() ? std::string{"EPERM"} : code) + ": "
                                      + fd_errno_message(e) + ", link '" + oldPath + "' -> '" + newPath + "'");
    }
#endif
    return JSValueMakeUndefined(ctx);
}
// Real chmod (node:fs chmodSync backing) — needed by Bun.which and any test
// that marks a fixture executable. The JS shim already normalizes string modes
// ("755") to an octal number, so `mode` here is a plain numeric permission set;
// only the low 12 bits (setuid/gid/sticky + rwx) are honored.
JSValueRef fsn_chmod_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                        const JSValueRef args[], JSValueRef* exc) {
    std::string path{fs_arg(ctx, argc, args, 0)};
    if (permission_deny_fs(ctx, exc, path, true)) return JSValueMakeUndefined(ctx);
    const auto modeNum{(argc > 1) ? JSValueToNumber(ctx, args[1], nullptr) : 0.0};
    const auto bits{static_cast<unsigned>(modeNum) & 07777u};
    // The message must be node's "<CODE>: <uv text>, chmod '<path>'": the old
    // "chmod '<path>': No such file or directory" form parsed as neither a code
    // nor a syscall, so chmodSync(<nonexistent>) threw an Error with no .code,
    // .errno, .syscall or .path at all.
#if defined(_WIN32)
    std::error_code ec;
    std::filesystem::permissions(std::filesystem::path{path},
                                 static_cast<std::filesystem::perms>(bits),
                                 std::filesystem::perm_options::replace, ec);
    if (ec) {
        const int e{ec.value()};
        const std::string code{fd_errno_code(e)};
        return fs_throw(ctx, exc, (code.empty() ? std::string{"EPERM"} : code) + ": "
                                      + fd_errno_message(e) + ", chmod '" + path + "'");
    }
#else
    if (::chmod(path.c_str(), static_cast<mode_t>(bits)) != 0) {
        const int e{errno};
        const std::string code{fd_errno_code(e)};
        return fs_throw(ctx, exc, (code.empty() ? std::string{"EPERM"} : code) + ": "
                                      + fd_errno_message(e) + ", chmod '" + path + "'");
    }
#endif
    return JSValueMakeUndefined(ctx);
}
JSValueRef fsn_realpath_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                           const JSValueRef args[], JSValueRef* exc) {
    std::string path{fs_arg(ctx, argc, args, 0)};
    if (permission_deny_fs(ctx, exc, path, false)) return JSValueMakeUndefined(ctx);
    if (fs_path_too_long(ctx, exc, path, "lstat")) return JSValueMakeUndefined(ctx);
    std::error_code ec;
    auto r{std::filesystem::weakly_canonical(std::filesystem::path{path}, ec)};
    if (ec) return fs_throw(ctx, exc, "realpath '" + path + "': " + ec.message());
    return make_string(ctx, r.string());
}
// realpathNative(path) — the STRICT resolver behind fs.realpathSync.native /
// fs.realpath.native, which node backs with uv_fs_realpath, i.e. realpath(3).
//
// It is deliberately a second entry point rather than a change to
// fsn_realpath_cb: that one is weakly_canonical, which resolves as far as the
// filesystem goes and then APPENDS the non-existent tail instead of failing.
// node's `.native` must fail — ENOENT for a missing component, ELOOP for a
// symlink cycle, ENOTDIR when a non-directory is used as one — and
// test-fs-realpath asserts all three. Keeping the lenient variant lets the
// internal callers that rely on "canonicalise what you can" stay as they are.
JSValueRef fsn_realpath_native_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                  const JSValueRef args[], JSValueRef* exc) {
    std::string path{fs_arg(ctx, argc, args, 0)};
    if (permission_deny_fs(ctx, exc, path, false)) return JSValueMakeUndefined(ctx);
    if (fs_path_too_long(ctx, exc, path, "realpath")) return JSValueMakeUndefined(ctx);
#if defined(_WIN32)
    std::error_code ec;
    auto r{std::filesystem::canonical(std::filesystem::path{path}, ec)};
    if (ec) return fs_throw(ctx, exc, "ENOENT: no such file or directory, realpath '" + path + "'");
    return make_string(ctx, r.string());
#else
    char* resolved{::realpath(path.c_str(), nullptr)};
    if (resolved == nullptr) {
        // uv_strerror wording, so the message reads exactly as node's uvException
        // builds it and fs_make_error can recover code/errno/syscall/path.
        const char* code{errno == ENOENT         ? "ENOENT: no such file or directory"
                         : errno == ELOOP        ? "ELOOP: too many symbolic links encountered"
                         : errno == ENOTDIR      ? "ENOTDIR: not a directory"
                         : errno == EACCES       ? "EACCES: permission denied"
                         : errno == ENAMETOOLONG ? "ENAMETOOLONG: name too long"
                                                 : "EINVAL: invalid argument"};
        return fs_throw(ctx, exc, std::string{code} + ", realpath '" + path + "'");
    }
    const std::string out{resolved};
    ::free(resolved);
    return make_string(ctx, out);
#endif
}
JSValueRef fsn_rename_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                         const JSValueRef args[], JSValueRef* exc) {
    const std::string oldPath{fs_arg(ctx, argc, args, 0)};
    const std::string newPath{fs_arg(ctx, argc, args, 1)};
    // node Rename: read(old), write(old), write(new) — in that order.
    if (permission_deny_fs(ctx, exc, oldPath, false)) return JSValueMakeUndefined(ctx);
    if (permission_deny_fs(ctx, exc, oldPath, true)) return JSValueMakeUndefined(ctx);
    if (permission_deny_fs(ctx, exc, newPath, true)) return JSValueMakeUndefined(ctx);
    // "rename: <libstdc++ text>" named no code, no syscall and no path, so
    // renameSync(<nonexistent>, x) threw a shapeless Error. node's form is
    // "<CODE>: <uv text>, rename '<old>' -> '<new>'" (path + dest).
    std::error_code ec;
    std::filesystem::rename(std::filesystem::path{oldPath}, std::filesystem::path{newPath}, ec);
    if (ec) {
        const int e{ec.value()};
        const std::string code{fd_errno_code(e)};
        return fs_throw(ctx, exc, (code.empty() ? std::string{"EPERM"} : code) + ": "
                                      + fd_errno_message(e) + ", rename '" + oldPath + "' -> '"
                                      + newPath + "'");
    }
    return JSValueMakeUndefined(ctx);
}
JSValueRef fsn_copyFile_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                           const JSValueRef args[], JSValueRef* exc) {
    const std::string src{fs_arg(ctx, argc, args, 0)};
    const std::string dest{fs_arg(ctx, argc, args, 1)};
    // node CopyFile: read(src) then write(dest).
    if (permission_deny_fs(ctx, exc, src, false)) return JSValueMakeUndefined(ctx);
    if (permission_deny_fs(ctx, exc, dest, true)) return JSValueMakeUndefined(ctx);
    std::error_code ec;
    std::filesystem::copy_file(std::filesystem::path{src}, std::filesystem::path{dest},
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        // Map the std::error_code to a node-style E* code so error.code is set
        // (test-fs-copyfile-respect-permissions asserts EACCES/EPERM). A
        // read-only destination surfaces EACCES when it cannot be opened for write.
        const int en{ec.value()};
        const char* code{"EACCES"};
        if (en == EPERM) code = "EPERM";
        else if (en == EACCES) code = "EACCES";
        else if (en == ENOENT) code = "ENOENT";
        else if (en == EEXIST) code = "EEXIST";
        else if (en == EISDIR) code = "EISDIR";
        else if (en == ENOTDIR) code = "ENOTDIR";
        else if (en == ENOSPC) code = "ENOSPC";
        std::string src{fs_arg(ctx, argc, args, 0)};
        std::string dst{fs_arg(ctx, argc, args, 1)};
        return fs_throw(ctx, exc, std::string{code} + ": " + ec.message() + ", copyfile '" + src + "' -> '" + dst + "'");
    }
    return JSValueMakeUndefined(ctx);
}
// Real symlink (node fs.symlinkSync(target, path) backing). `target` may be a
// non-existent / self-referential path — a dangling link is valid and needed by
// the recursive-readdir ELOOP test. create_symlink does not require the target
// to exist.
JSValueRef fsn_symlink_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                          const JSValueRef args[], JSValueRef* exc) {
    std::string target{fs_arg(ctx, argc, args, 0)};
    std::string linkpath{fs_arg(ctx, argc, args, 1)};
    // node Symlink: read(target), write(target), write(path). Gating the TARGET
    // for write is what stops a symlink being used to smuggle a write into a
    // read-only tree (test-permission-fs-symlink-target-write).
    if (permission_deny_fs(ctx, exc, target, false)) return JSValueMakeUndefined(ctx);
    if (permission_deny_fs(ctx, exc, target, true)) return JSValueMakeUndefined(ctx);
    if (permission_deny_fs(ctx, exc, linkpath, true)) return JSValueMakeUndefined(ctx);
    std::error_code ec;
    std::filesystem::create_symlink(std::filesystem::path{target},
                                    std::filesystem::path{linkpath}, ec);
    // node-shaped errno: fs.cp's onLink relies on EEXIST when the link path is
    // already taken (a symlink copied over a regular file).
    if (ec) {
        switch (ec.value()) {
            case EEXIST:
                return fs_throw(ctx, exc, "EEXIST: file already exists, symlink '" + target
                                              + "' -> '" + linkpath + "'");
            case ENOENT:
                return fs_throw(ctx, exc, "ENOENT: no such file or directory, symlink '" + target
                                              + "' -> '" + linkpath + "'");
            case EACCES:
                return fs_throw(ctx, exc, "EACCES: permission denied, symlink '" + target
                                              + "' -> '" + linkpath + "'");
            case ENOTDIR:
                return fs_throw(ctx, exc, "ENOTDIR: not a directory, symlink '" + target
                                              + "' -> '" + linkpath + "'");
            default:
                return fs_throw(ctx, exc, "EPERM: operation not permitted, symlink '" + target
                                              + "' -> '" + linkpath + "'");
        }
    }
    return JSValueMakeUndefined(ctx);
}
// Recursive readdir walk (node readdirSync(path, { recursive: true })). Mirrors
// bun's walker: each entry's target is resolved via status() (follows symlinks),
// so a self-referential symlink surfaces ELOOP — which, unlike the silently
// skipped NOENT/NOTDIR/PERM set, propagates as a thrown error. Returns parallel
// { names, types } arrays of relative paths; the JS layer builds Dirents from
// them when withFileTypes is set.
void fsn_readdir_recursive_walk(JSContextRef ctx, const std::filesystem::path& base,
                                const std::string& rel, std::vector<JSValueRef>& out,
                                std::vector<JSValueRef>& types, bool& threw, JSValueRef* exc) {
    if (threw) return;
    std::error_code ec;
    std::filesystem::directory_iterator it{base, ec};
    if (ec) {
        fs_throw(ctx, exc, "readdir '" + base.string() + "': " + ec.message());
        threw = true;
        return;
    }
    // Same alphasort parity as the non-recursive readdir above: libuv's scandir
    // sorts each directory level, and node's recursive readdir inherits that
    // order level by level.
    std::vector<std::filesystem::directory_entry> level{it, std::filesystem::directory_iterator{}};
    std::ranges::sort(level, {}, [](const std::filesystem::directory_entry& e) {
        return e.path().filename().string();
    });
    for (const auto& e : level) {
        if (threw) return;
        const std::string name{e.path().filename().string()};
        const std::string relPath{rel.empty() ? name : rel + "/" + name};
        std::error_code sec;
        auto st{std::filesystem::status(e.path(), sec)};
        if (sec) {
            const int v{sec.value()};
            if (v != ENOENT && v != ENOTDIR && v != EACCES && v != EPERM) {
                fs_throw(ctx, exc, "readdir '" + relPath + "': " + sec.message());
                threw = true;
                return;
            }
            continue;
        }
        const bool isDir{std::filesystem::is_directory(st)};
        const double type{isDir ? 2.0
                                : (std::filesystem::is_symlink(e.symlink_status()) ? 3.0 : 1.0)};
        out.push_back(make_string(ctx, relPath));
        types.push_back(JSValueMakeNumber(ctx, type));
        if (isDir) {
            fsn_readdir_recursive_walk(ctx, e.path(), relPath, out, types, threw, exc);
            if (threw) return;
        }
    }
}
JSValueRef fsn_readdirRecursive_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                   const JSValueRef args[], JSValueRef* exc) {
    std::string path{fs_arg(ctx, argc, args, 0)};
    if (permission_deny_fs(ctx, exc, path, false)) return JSValueMakeUndefined(ctx);
    std::vector<JSValueRef> names;
    std::vector<JSValueRef> types;
    bool threw{false};
    fsn_readdir_recursive_walk(ctx, std::filesystem::path{path}, "", names, types, threw, exc);
    if (threw) return JSValueMakeUndefined(ctx);
    JSObjectRef o{JSObjectMake(ctx, nullptr, nullptr)};
    set_prop(ctx, o, "names",
             JSObjectMakeArray(ctx, names.size(), names.empty() ? nullptr : names.data(), nullptr));
    set_prop(ctx, o, "types",
             JSObjectMakeArray(ctx, types.size(), types.empty() ? nullptr : types.data(), nullptr));
    return o;
}
JSValueRef fsn_mkdtemp_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                          const JSValueRef args[], JSValueRef* exc) {
    std::string prefix{fs_arg(ctx, argc, args, 0)};
    if (permission_deny_fs(ctx, exc, prefix, true)) return JSValueMakeUndefined(ctx);
    // create <prefix><suffix> with a clock-seeded counter — a plain 0-based
    // counter collides with directories left behind by previous runs.
    static std::atomic<std::uint64_t> counter{static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count())};
    // node replaces a SIX-character XXXXXX template, so the result's basename is
    // exactly prefix.length + 6 (test-fs-promises-mkdtempDisposable asserts it).
    static constexpr char ALPHABET[]{"0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"};
    std::error_code ec;
    for (int attempt = 0; attempt < 1000; ++attempt) {
        std::uint64_t v{counter.fetch_add(1)};
        std::string suffix(6, '0');
        for (int i = 0; i < 6; ++i) { suffix[i] = ALPHABET[v % 62]; v /= 62; }
        std::string dir{prefix + suffix};
        if (std::filesystem::create_directory(std::filesystem::path{dir}, ec) && !ec) {
            return make_string(ctx, dir);
        }
        // Only a name collision is worth retrying. Spinning 1000 times on a
        // missing parent directory and then throwing the codeless "mkdtemp
        // '<p>': failed" hid node's ENOENT (test-fs-error-messages matches the
        // message, .path, .errno, .code and .syscall).
        if (ec && ec.value() != EEXIST) {
            const int e{ec.value()};
            const std::string code{fd_errno_code(e)};
            return fs_throw(ctx, exc, (code.empty() ? std::string{"EPERM"} : code) + ": "
                                          + fd_errno_message(e) + ", mkdtemp '" + dir + "'");
        }
    }
    return fs_throw(ctx, exc, "EEXIST: file already exists, mkdtemp '" + prefix + "'");
}

// ── fd-level positioned I/O (node fs.openSync/readSync/writeSync/closeSync) ──
// Real descriptor table over mbun.core.io::File (native pread/pwrite per
// platform). Buffers cross zero-copy via JSC::JSArrayBufferView::vector().
//
// These are native host functions (see "Native host functions" above): the
// fs.readSync/writeSync hot loop previously went through the C API's
// APICallbackFunction wrapper, paying the JSLock grab/drop plus per-arg C API
// coercions on EVERY call — measured ~2.9x behind bun on the io suite. The
// interpreter now calls these directly with zero lock churn.

std::unordered_map<int, mbun::core::io::File>& fd_table() {
    static std::unordered_map<int, mbun::core::io::File> table;
    return table;
}

// Throw a plain JS string — the exact value the C API path used to hand back
// via *exc (fs_throw), so what scripts catch is unchanged.
JSC::EncodedJSValue fd_throw(JSC::JSGlobalObject* global, const std::string& msg) {
    JSC::VM& vm{global->vm()};
    auto scope{DECLARE_THROW_SCOPE(vm)};
    scope.throwException(global, toJS(global, make_string(toRef(global), msg)));
    return JSC::encodedJSValue();
}

// Map a raw errno to its Node.js error code string ("ENOENT", "ENXIO", ...).
std::string fd_errno_code(int e) {
    switch (e) {
        case ENOENT: return "ENOENT";
        case EACCES: return "EACCES";
        case EEXIST: return "EEXIST";
        case ENOTDIR: return "ENOTDIR";
        case EISDIR: return "EISDIR";
        case ENXIO: return "ENXIO";
        case EPERM: return "EPERM";
        case EMFILE: return "EMFILE";
        case ENFILE: return "ENFILE";
        case ELOOP: return "ELOOP";
        case ENAMETOOLONG: return "ENAMETOOLONG";
        case EINVAL: return "EINVAL";
        case EBADF: return "EBADF";
        case ENOSPC: return "ENOSPC";
        case EROFS: return "EROFS";
        case EBUSY: return "EBUSY";
        case ENOTEMPTY: return "ENOTEMPTY";
        case EAGAIN: return "EAGAIN";
        // link(2)/rename(2) reach these and nothing else does; without them a
        // cross-device rename or a hard link at the link limit fell through to
        // the caller's hand-picked fallback code.
        case EXDEV: return "EXDEV";
        case EMLINK: return "EMLINK";
#if !defined(_WIN32)
        case EOVERFLOW: return "EOVERFLOW";
        case EFBIG: return "EFBIG";
#endif
        default: return {};
    }
}

// libuv `uv_strerror` text for the errnos fd_errno_code knows. node's fs errors
// are formatted from libuv, NOT from the C library: glibc's strerror(ENOENT) is
// "No such file or directory" but uv_strerror(UV_ENOENT) is "no such file or
// directory", and the corpus compares those messages verbatim. Kept in sync with
// the UV_LINUX table in builtins/node_util_extra.cppm (util.getSystemErrorMap).
std::string fd_errno_message(int e) {
    switch (e) {
        case ENOENT: return "no such file or directory";
        case EACCES: return "permission denied";
        case EEXIST: return "file already exists";
        case ENOTDIR: return "not a directory";
        case EISDIR: return "illegal operation on a directory";
        case ENXIO: return "no such device or address";
        case EPERM: return "operation not permitted";
        case EMFILE: return "too many open files";
        case ENFILE: return "file table overflow";
        case ELOOP: return "too many symbolic links encountered";
        case ENAMETOOLONG: return "name too long";
        case EINVAL: return "invalid argument";
        case EBADF: return "bad file descriptor";
        case ENOSPC: return "no space left on device";
        case EROFS: return "read-only file system";
        case EBUSY: return "resource busy or locked";
        case ENOTEMPTY: return "directory not empty";
        case EAGAIN: return "resource temporarily unavailable";
        case EXDEV: return "cross-device link not permitted";
        case EMLINK: return "too many links";
#if !defined(_WIN32)
        case EOVERFLOW: return "value too large for defined data type";
        case EFBIG: return "file too large";
#endif
        default: return std::strerror(e);
    }
}

// Throw a Node-style fs error carrying .code/.errno/.syscall/.path, so JS
// `catch (e) { e.code === "ENXIO" }` works (Node semantics; a bare string
// would leave .code undefined).
JSC::EncodedJSValue fd_throw_errno(JSC::JSGlobalObject* global, int e,
                                   const std::string& syscall, const std::string& path) {
    JSContextRef ctx{toRef(global)};
    const std::string code{fd_errno_code(e)};
    std::string msg{(code.empty() ? "E" + std::to_string(e) : code) + ": "
                    + fd_errno_message(e) + ", " + syscall + " '" + path + "'"};
    JSValueRef m{make_string(ctx, msg)};
    JSObjectRef err{JSObjectMakeError(ctx, 1, &m, nullptr)};
    // As in fs_make_error: node populates these by plain assignment, so a
    // poisoned Object.prototype setter for errno/fd must propagate ITS error
    // rather than be swallowed here (test-fs-writesync-crash).
    JSValueRef setExc{nullptr};
    const auto put{[&](const char* name, JSValueRef v) {
        if (setExc != nullptr) return;
        JSStringRef n{JSStringCreateWithUTF8CString(name)};
        JSObjectSetProperty(ctx, err, n, v, kJSPropertyAttributeNone, &setExc);
        JSStringRelease(n);
    }};
    if (!code.empty()) put("code", make_string(ctx, code));
    put("errno", JSValueMakeNumber(ctx, -e));
    put("syscall", make_string(ctx, syscall));
    put("path", make_string(ctx, path));
    JSC::VM& vm{global->vm()};
    auto scope{DECLARE_THROW_SCOPE(vm)};
    scope.throwException(
        global, toJS(global, setExc != nullptr ? setExc : static_cast<JSValueRef>(err)));
    return JSC::encodedJSValue();
}

// ERR_ACCESS_DENIED on the native host-function path. Same error object as the
// C-API gate (permission_denied_error), thrown through JSC's throw scope.
JSC::EncodedJSValue fd_throw_access_denied(JSC::JSGlobalObject* global,
                                          mbun::permission::Scope scope,
                                          const std::string& path) {
    JSValueRef err{permission_denied_error(toRef(global), scope, path)};
    JSC::VM& vm{global->vm()};
    auto throwScope{DECLARE_THROW_SCOPE(vm)};
    throwScope.throwException(global, toJS(global, err));
    return JSC::encodedJSValue();
}

// node node_file.cc CheckOpenPermissions: an open() needs fs.read unless it is
// write-ONLY, and fs.write whenever it is not read-only OR carries a flag with a
// write-like side effect (O_APPEND / O_CREAT / O_TRUNC). The second half is the
// fix for nodejs/node#47090 — "extra flags should not enable trivially
// bypassing all restrictions" — and the corpus asserts it directly.
std::optional<mbun::permission::Scope> fd_open_denied_scope(const std::string& path,
                                                            const mbun::core::io::OpenOptions& o) {
    if (!gPermission.enabled()) return std::nullopt;
    namespace io = mbun::core::io;
    const bool writeOnly{o.access == io::Access::WriteOnly};
    const bool readOnly{o.access == io::Access::ReadOnly};
    const bool writeSideEffect{o.create || o.truncate || o.append};
    if (!writeOnly && !gPermission.is_granted(mbun::permission::Scope::FileSystemRead, path)) {
        if (!gPermission.warning_only()) return mbun::permission::Scope::FileSystemRead;
    }
    if ((!readOnly || writeSideEffect) &&
        !gPermission.is_granted(mbun::permission::Scope::FileSystemWrite, path)) {
        if (!gPermission.warning_only()) return mbun::permission::Scope::FileSystemWrite;
    }
    return std::nullopt;
}

// Number coercion matching JSValueToNumber (fast path: already a number).
double fd_num(JSC::JSGlobalObject* global, JSC::JSValue v) {
    return v.isNumber() ? v.asNumber() : v.toNumber(global);
}

JSC_DEFINE_HOST_FUNCTION(fdn_open_host, (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    namespace io = mbun::core::io;
    JsStrArg pathArg{global, native_arg(frame, 0)};
    std::string path{pathArg.view()};
    std::string flags{"r"};
    if (frame->argumentCount() > 1) {
        JSC::JSValue flagV{frame->argument(1)};
        // node accepts the numeric O_* bitmask everywhere a flag string goes
        // (fs.open(p, fs.constants.O_WRONLY | O_CREAT), writeFileSync flag).
        // Stringifying it produced "577", whose first char selected the
        // read-only default — the write silently went to a read-only fd.
#if defined(_WIN32)
        constexpr int O_APPEND{0x0008}, O_CREAT{0x0100}, O_TRUNC{0x0200}, O_EXCL{0x0400};
#endif
        if (flagV.isNumber()) {
            const int bits{static_cast<int>(flagV.asNumber())};
            const int access{bits & 3};
            const bool plus{access == 2};
            if ((bits & O_APPEND) != 0) flags = plus ? "a+" : "a";
            else if (access == 0) flags = "r";
            else if ((bits & (O_TRUNC | O_CREAT)) != 0) flags = plus ? "w+" : "w";
            else flags = plus ? "r+" : "r+";
            if ((bits & O_EXCL) != 0) flags += "x";
        } else {
            JsStrArg f{global, flagV};
            flags.assign(f.view());
        }
    }
    io::OpenOptions o{};
    const bool plus{flags.find('+') != std::string::npos};
    switch (flags.empty() ? 'r' : flags[0]) {
        case 'w':
            o.access = plus ? io::Access::ReadWrite : io::Access::WriteOnly;
            o.create = true;
            o.truncate = true;
            break;
        case 'a':
            o.access = plus ? io::Access::ReadWrite : io::Access::WriteOnly;
            o.create = true;
            o.append = true;
            break;
        default:
            o.access = plus ? io::Access::ReadWrite : io::Access::ReadOnly;
            break;
    }
    if (flags.find('x') != std::string::npos) o.exclusive = true;
    JSC::JSValue modeV{native_arg(frame, 2)};
    if (modeV && modeV.isNumber()) o.mode = static_cast<std::uint32_t>(modeV.asNumber());
    // Optional 4th arg: open in non-blocking mode (O_NONBLOCK). Used by the
    // Bun.file() FIFO reader/writer path (a synchronous runtime cannot block a
    // fifo open waiting for the peer — FileSink over a named pipe).
    JSC::JSValue nbV{native_arg(frame, 3)};
    if (nbV && nbV.toBoolean(global)) o.nonblock = true;
    // The Permission Model gate for the fd path. Everything downstream
    // (fdn_read/write/ftruncate/close) takes an fd that can only have come from
    // HERE, so gating the open gates the whole descriptor.
    if (const auto denied{fd_open_denied_scope(path, o)}) {
        return fd_throw_access_denied(global, *denied, path);
    }
    auto file{io::File::open(std::filesystem::path{path}, o)};
    if (!file) return fd_throw_errno(global, file.error().nativeCode, "open", path);
    // Reuse the lowest free virtual fd (like the OS descriptor table) rather than
    // a monotonic counter, so a closed fd's number is handed out again — a test
    // that opens/closes repeatedly sees a stable max fd (fs-leak).
    auto& tbl{fd_table()};
    int fd{1000};
    while (tbl.contains(fd)) ++fd;
    tbl.emplace(fd, std::move(*file));
    return JSC::JSValue::encode(JSC::jsNumber(fd));
}

JSC_DEFINE_HOST_FUNCTION(fdn_close_host, (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    if (frame->argumentCount() > 0) {
        const int fd{static_cast<int>(fd_num(global, frame->argument(0)))};
        if (fd_table().erase(fd) == 0) {
            // close(2) on a descriptor that is not open is EBADF, and node
            // surfaces it (test-fs-error-messages' close block runs closeSync on
            // common.runWithInvalidFD's ~2^30 fd and asserts the message, errno,
            // code and syscall). Erasing a missing key silently returned
            // undefined, so both the sync throw and the async callback's error
            // were absent. An fd we do not own but which IS open at the OS level
            // (an inherited descriptor, stdio) is left exactly as before — this
            // adds the error, it does not add a close.
#if !defined(_WIN32)
            const bool openAtOs{::fcntl(fd, F_GETFD) != -1};
#else
            const bool openAtOs{false};
#endif
            if (!openAtOs) {
                JSC::VM& vm{global->vm()};
                auto scope{DECLARE_THROW_SCOPE(vm)};
                JSValueRef err{fs_make_error(toRef(global), "EBADF: bad file descriptor, close")};
                scope.throwException(global, toJS(global, err));
                return JSC::encodedJSValue();
            }
        }
    }
    return JSC::JSValue::encode(JSC::jsUndefined());
}

// Shared arg decode for read/write: (fd, typedArray, offset, length, position).
// position < 0 → current-cursor read/write; otherwise positioned (pread/pwrite).
struct FdIoArgs {
    mbun::core::io::File* file{nullptr};
    std::byte* base{nullptr};
    std::size_t len{0};
    double pos{-1};
};

// node's fd-level errors carry NO path: "EBADF: bad file descriptor, read".
// fd_throw_errno always appends " '<path>'", and the older EBADF sites threw a
// bare JS *string*, so `err.code` / `err.syscall` were undefined on every one of
// them (test-fs-error-messages checks read/write/ftruncate/close/f* that way).
JSC::EncodedJSValue fd_throw_errno_nopath(JSC::JSGlobalObject* global, int e,
                                          const std::string& syscall) {
    const std::string code{fd_errno_code(e)};
    JSValueRef err{fs_make_error(toRef(global),
                                 (code.empty() ? "E" + std::to_string(e) : code) + ": "
                                     + fd_errno_message(e) + ", " + syscall)};
    JSC::VM& vm{global->vm()};
    auto scope{DECLARE_THROW_SCOPE(vm)};
    scope.throwException(global, toJS(global, err));
    return JSC::encodedJSValue();
}

std::optional<FdIoArgs> fd_io_args(JSC::JSGlobalObject* global, JSC::CallFrame* frame,
                                   const char* syscall) {
    if (frame->argumentCount() < 2) return std::nullopt;
    const int fd{static_cast<int>(fd_num(global, frame->argument(0)))};
    auto it{fd_table().find(fd)};
    if (it == fd_table().end()) {
        fd_throw_errno_nopath(global, EBADF, syscall);
        return std::nullopt;
    }
    JSC::JSValue bufV{frame->argument(1)};
    JSC::JSArrayBufferView* view{
        bufV.isCell() ? WTF::dynamicDowncast<JSC::JSArrayBufferView>(bufV.asCell()) : nullptr};
    if (view == nullptr || view->isDetached() || view->vector() == nullptr) {
        // Mirror the C API path: a non-object buffer silently reads/writes 0
        // bytes; an object that is not a (attached) typed array throws EINVAL.
        if (bufV.isObject()) fd_throw(global, "EINVAL: buffer is not a typed array");
        return std::nullopt;
    }
    // Pin+lock the backing store exactly like the C API's
    // JSObjectGetTypedArrayBytesPtr did: the caller's ArrayBuffer must not
    // become transferable/detachable once native I/O has seen its bytes
    // (fs.test.ts "transferred while the read is pending"). This also
    // materializes the buffer for fast typed arrays, so vector() below is the
    // final, stable pointer.
    if (JSC::ArrayBuffer* ab{view->unsharedBuffer()}) ab->pinAndLock();
    auto* base{static_cast<std::byte*>(view->vector())};
    const std::size_t cap{view->byteLength()};
    JSC::JSValue offV{frame->argument(2)};
    JSC::JSValue lenV{frame->argument(3)};
    JSC::JSValue posV{frame->argument(4)};
    std::size_t off{frame->argumentCount() > 2 ? static_cast<std::size_t>(fd_num(global, offV))
                                               : 0};
    std::size_t len{lenV.isNumber() ? static_cast<std::size_t>(lenV.asNumber()) : cap};
    if (off > cap) off = cap;
    if (len > cap - off) len = cap - off;
    double pos{-1};
    if (posV.isNumber()) pos = posV.asNumber();
    return FdIoArgs{.file = &it->second, .base = base + off, .len = len, .pos = pos};
}

JSC_DEFINE_HOST_FUNCTION(fdn_read_host, (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    auto a{fd_io_args(global, frame, "read")};
    // On decode failure any exception is already on the VM; the C API version
    // returned 0 alongside, which callers with a pending exception never see.
    if (!a) return JSC::JSValue::encode(JSC::jsNumber(0));
    std::span<std::byte> out{a->base, a->len};
    // A position past 2^53 cannot round-trip through a JS double, so pread(2)
    // would receive a wrapped (negative) off_t and report EINVAL. node's own
    // corpus accepts EFBIG/EOVERFLOW for such positions — report EOVERFLOW
    // rather than a bare EIO string with no `code` on it.
    if (a->pos > 9007199254740992.0) return fd_throw_errno(global, EOVERFLOW, "read", "");
    auto r{a->pos >= 0 ? a->file->read_at(static_cast<std::uint64_t>(a->pos), out)
                       : a->file->read(out)};
    if (!r) {
        // A non-blocking fd with no data ready signals backpressure, not a hard
        // error: report -1 so the JS FIFO reader can retry instead of throwing.
        if (r.error().code == mbun::core::io::ErrorCode::WouldBlock)
            return JSC::JSValue::encode(JSC::jsNumber(-1));
        return fd_throw_errno(global, r.error().nativeCode != 0 ? r.error().nativeCode : EIO,
                              "read", "");
    }
    return JSC::JSValue::encode(JSC::jsNumber(static_cast<double>(*r)));
}

JSC_DEFINE_HOST_FUNCTION(fdn_write_host, (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    auto a{fd_io_args(global, frame, "write")};
    if (!a) return JSC::JSValue::encode(JSC::jsNumber(0));
    std::span<const std::byte> in{a->base, a->len};
    if (a->pos > 9007199254740992.0) return fd_throw_errno(global, EOVERFLOW, "write", "");
    auto r{a->pos >= 0 ? a->file->write_at(static_cast<std::uint64_t>(a->pos), in)
                       : a->file->write(in)};
    if (!r) {
        // Non-blocking fd whose kernel buffer is full: report -1 (backpressure)
        // so the JS FileSink buffers the remainder rather than throwing.
        if (r.error().code == mbun::core::io::ErrorCode::WouldBlock)
            return JSC::JSValue::encode(JSC::jsNumber(-1));
        return fd_throw_errno(global, r.error().nativeCode != 0 ? r.error().nativeCode : EIO,
                              "write", "");
    }
    return JSC::JSValue::encode(JSC::jsNumber(static_cast<double>(*r)));
}

// __mbunFdNative.valid(fd) — is this descriptor open, at either level? mbun's
// own fds live in fd_table; stdio and inherited descriptors are only real at the
// OS level. The JS fd shims that have no native row (fsync/fdatasync/fchmod/
// fchown/futimes) need exactly this question answered so they can raise node's
// EBADF instead of silently succeeding on a closed fd.
JSC_DEFINE_HOST_FUNCTION(fdn_valid_host, (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    if (frame->argumentCount() < 1) return JSC::JSValue::encode(JSC::jsBoolean(false));
    const int fd{static_cast<int>(fd_num(global, frame->argument(0)))};
    if (fd < 0) return JSC::JSValue::encode(JSC::jsBoolean(false));
    auto it{fd_table().find(fd)};
    if (it != fd_table().end() && it->second.is_open())
        return JSC::JSValue::encode(JSC::jsBoolean(true));
#if defined(_WIN32)
    return JSC::JSValue::encode(JSC::jsBoolean(false));
#else
    return JSC::JSValue::encode(JSC::jsBoolean(::fcntl(fd, F_GETFD) != -1));
#endif
}

// ftruncate(2) on a virtual fd. fs.ftruncateSync / filehandle.truncate() were
// no-ops, so `await handle.truncate(5)` left the file at its original length.
JSC_DEFINE_HOST_FUNCTION(fdn_ftruncate_host, (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    if (frame->argumentCount() < 1) return JSC::JSValue::encode(JSC::jsUndefined());
    const int fd{static_cast<int>(fd_num(global, frame->argument(0)))};
    auto it{fd_table().find(fd)};
    if (it == fd_table().end()) {
        fd_throw_errno_nopath(global, EBADF, "ftruncate");
        return JSC::JSValue::encode(JSC::jsUndefined());
    }
    const double len{frame->argumentCount() > 1 ? fd_num(global, frame->argument(1)) : 0.0};
#if defined(_WIN32)
    (void)len;
    return fd_throw(global, "ENOSYS: ftruncate is not supported on Windows");
#else
    if (::ftruncate(it->second.native_fd(), static_cast<off_t>(len)) != 0)
        return fd_throw_errno(global, errno, "ftruncate", std::to_string(fd));
    return JSC::JSValue::encode(JSC::jsUndefined());
#endif
}

// bun:internal-for-testing createSocketPair() → [readFd, writeFd]. A connected
// AF_UNIX SOCK_STREAM pair, both non-blocking and adopted into the virtual fd
// table so FileSink (writer) and the streaming reader can drive real socket
// backpressure on the single JS thread. POSIX only.
JSC_DEFINE_HOST_FUNCTION(create_socket_pair_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    (void)frame;
#if defined(_WIN32)
    return fd_throw(global, "ENOSYS: createSocketPair is not supported on Windows");
#else
    int sv[2]{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        return fd_throw(global, "EMFILE: socketpair failed");
    }
    for (int i = 0; i < 2; ++i) {
        const int fl{::fcntl(sv[i], F_GETFL, 0)};
        if (fl >= 0) (void)::fcntl(sv[i], F_SETFL, fl | O_NONBLOCK);
    }
    auto& tbl{fd_table()};
    int vfds[2]{-1, -1};
    for (int i = 0; i < 2; ++i) {
        int fd{1000};
        while (tbl.contains(fd)) ++fd;
        tbl.emplace(fd, mbun::core::io::File::adopt(sv[i]));
        vfds[i] = fd;
    }
    JSC::JSArray* arr{JSC::constructEmptyArray(global, nullptr, 2)};
    if (arr != nullptr) {
        arr->putDirectIndex(global, 0, JSC::jsNumber(vfds[0]));
        arr->putDirectIndex(global, 1, JSC::jsNumber(vfds[1]));
    }
    return JSC::JSValue::encode(arr != nullptr ? JSC::JSValue{arr} : JSC::jsUndefined());
#endif
}

// node:child_process.spawnSync(file, args, options) → { status, signal, pid,
// stdout, stderr }. Real fork/exec with stdout+stderr pipes; `input`/`cwd` from
// options honoured. POSIX only (Windows subprocess is DEFERRED).
