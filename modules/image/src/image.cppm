// image.cppm — mbun.image: Bun.Image pixel kernels + minimal PNG codec.
//
// Blueprint references (移植三段法 step 1: faithful translation):
//   - bun src/jsc/bindings/image_resize.cpp — separable two-pass resize with
//     i16 fixed-point (1<<14) weights, half-pixel-centre `(i+0.5)/scale-0.5`,
//     clamp-span + renormalise-truncated-weights edge mode; rotate/flip;
//     modulate; nearest-palette search. The Highway SIMD dispatch is dropped
//     here — this is the clean scalar version (the inner loops are written so
//     the autovectorizer can still do its thing; explicit SIMD is a later
//     optimization pass).
//   - bun src/runtime/image/quantize.rs — Heckbert median-cut quantizer +
//     serpentine Floyd–Steinberg error diffusion.
//   - bun src/runtime/image/codec_png.rs uses libspng; mbun implements the
//     subset the pipeline needs directly on mbun.core.compress: 8-bit
//     non-interlaced PNGs (colour types 0/2/3/4/6, filters 0–4, PLTE/tRNS,
//     iCCP passthrough) decode to RGBA8; encode emits colour type 6
//     (truecolour+alpha) or 3 (indexed via the quantizer).
//
// Behaviour is pinned by compat/bun/test/js/bun/image/image-kernels.test.ts and the
// PNG-path parts of image.test.ts.
export module mbun.image;

export import mbun.image.jpeg;  // baseline JPEG codec (leaf submodule)
export import mbun.image.webp;  // WebP data model/probe/native codec seam

import std;
import mbun.core.compress;

namespace mbun::image {

using Bytes = std::vector<std::uint8_t>;
using ByteView = std::span<const std::uint8_t>;
namespace compress = mbun::core::compress;

// ─── resize kernels (port of image_resize.cpp, scalar) ──────────────────────

// Filter kinds — numbering matches image_resize.cpp / codecs.rs Filter enum.
export enum class Filter : int {
    Box = 0,
    Bilinear = 1,
    Lanczos3 = 2,
    Mitchell = 3,
    Nearest = 4,
    Cubic = 5,  // Catmull-Rom
    Lanczos2 = 6,
    Mks2013 = 7,  // Magic Kernel Sharp
    Mks2021 = 8,
};

export std::optional<Filter> filter_from_name(std::string_view s) {
    if (s == "box") return Filter::Box;
    if (s == "bilinear" || s == "linear") return Filter::Bilinear;
    if (s == "lanczos3") return Filter::Lanczos3;
    if (s == "mitchell") return Filter::Mitchell;
    if (s == "nearest") return Filter::Nearest;
    if (s == "cubic") return Filter::Cubic;
    if (s == "lanczos2") return Filter::Lanczos2;
    if (s == "mks2013") return Filter::Mks2013;
    if (s == "mks2021") return Filter::Mks2021;
    return std::nullopt;
}

namespace {

// One contribution span: for output pixel `i`, sum src[start..start+n) * w[..n).
struct Span {
    std::int32_t start;
    std::int32_t n;
};

// Fixed-point shift. Weights are i16 with Σw = 1<<K_FIX_SHIFT; products go
// into i32 (max |255 · Σ|w| · (1<<14)| ≈ 5.4M for lanczos3, well inside i32).
constexpr int K_FIX_SHIFT{14};
constexpr std::int32_t K_FIX_ROUND{1 << (K_FIX_SHIFT - 1)};
constexpr std::int32_t K_FIX_ONE{1 << K_FIX_SHIFT};

double sinc(double x) {
    if (std::fabs(x) < 1e-8) return 1.0;
    const double px{std::numbers::pi * x};
    return std::sin(px) / px;
}

// BC-spline cubic (Mitchell & Netravali 1988), radius 2.
//   B=1/3, C=1/3 → Mitchell (minimal ringing, no overshoot)
//   B=0,   C=1/2 → Catmull-Rom ("cubic" in Sharp; sharper, slight ring)
double bc_cubic(double b, double c, double x) {
    x = std::fabs(x);
    const double xx{x * x};
    if (x < 1.0)
        return ((12 - 9 * b - 6 * c) * xx * x + (-18 + 12 * b + 6 * c) * xx + (6 - 2 * b)) / 6.0;
    if (x < 2.0)
        return ((-b - 6 * c) * xx * x + (6 * b + 30 * c) * xx + (-12 * b - 48 * c) * x
                + (8 * b + 24 * c))
               / 6.0;
    return 0.0;
}

// Filter kernel; radius is the support half-width in source pixels at scale=1.
double filter_eval(int kind, double x) {
    switch (kind) {
        case 0:  // box
        case 4:  // nearest — same kernel; radius<1 collapses to a single tap
            return (x > -0.5 && x <= 0.5) ? 1.0 : 0.0;
        case 1:  // bilinear / triangle
            x = std::fabs(x);
            return x < 1.0 ? 1.0 - x : 0.0;
        case 3:  // mitchell
            return bc_cubic(1.0 / 3.0, 1.0 / 3.0, x);
        case 5:  // cubic (Catmull-Rom)
            return bc_cubic(0.0, 0.5, x);
        case 6:  // lanczos2
            x = std::fabs(x);
            return x < 2.0 ? sinc(x) * sinc(x / 2.0) : 0.0;
        case 7: {  // mks2013 — Magic Kernel Sharp 2013 (Costella)
            x = std::fabs(x);
            if (x >= 2.5) return 0.0;
            if (x >= 1.5) return -(x - 2.5) * (x - 2.5) / 8.0;
            if (x >= 0.5) return (4.0 * x * x - 11.0 * x + 7.0) / 4.0;
            return 17.0 / 16.0 - 7.0 * x * x / 4.0;
        }
        case 8: {  // mks2021 — refined MKS, radius 4.5
            x = std::fabs(x);
            if (x >= 4.5) return 0.0;
            if (x >= 3.5) return -(4.0 * x * x - 36.0 * x + 81.0) / 1152.0;
            if (x >= 2.5) return (4.0 * x * x - 27.0 * x + 45.0) / 144.0;
            if (x >= 1.5) return -(24.0 * x * x - 113.0 * x + 130.0) / 144.0;
            if (x >= 0.5) return (140.0 * x * x - 379.0 * x + 239.0) / 144.0;
            return 577.0 / 576.0 - 239.0 * x * x / 144.0;
        }
        default:  // lanczos3
            x = std::fabs(x);
            return x < 3.0 ? sinc(x) * sinc(x / 3.0) : 0.0;
    }
}

double filter_radius(int kind) {
    switch (kind) {
        case 0:  // box
        case 4:  // nearest
            return 0.5;
        case 1:  // bilinear
            return 1.0;
        case 3:  // mitchell
        case 5:  // cubic
        case 6:  // lanczos2
            return 2.0;
        case 7:  // mks2013
            return 2.5;
        case 8:  // mks2021
            return 4.5;
        default:  // lanczos3
            return 3.0;
    }
}

// Precompute spans + normalised weights for one axis. Weights are i16
// fixed-point with Σw = 1<<K_FIX_SHIFT exactly: each set is normalised in f64,
// scaled, rounded, then the largest tap absorbs the rounding residual so DC
// gain is bit-exact (a flat field comes back flat).
void build_weights(int kind, std::int32_t srcLen, std::int32_t dstLen, Span* spans,
                   std::int16_t* weights, std::int32_t wstride) {
    const double scale{static_cast<double>(dstLen) / static_cast<double>(srcLen)};
    // When downscaling, stretch the kernel by 1/scale so it covers the whole
    // source footprint of a destination pixel. `nearest` is the exception: it
    // must pick exactly ONE source sample at any scale, so keep fscale=1.
    const double fscale{(kind == 4 || scale >= 1.0) ? 1.0 : scale};
    // Cap support so the per-pixel span fits the 256-tap stack buffer below;
    // capping BEFORE start/end are derived keeps the window centred.
    const double support{std::min(filter_radius(kind) / fscale, 127.0)};
    for (std::int32_t i{0}; i < dstLen; i++) {
        const double center{(i + 0.5) / scale - 0.5};
        std::int32_t start{static_cast<std::int32_t>(std::floor(center - support + 0.5))};
        std::int32_t end{static_cast<std::int32_t>(std::floor(center + support + 0.5))};
        if (start < 0) start = 0;
        if (end >= srcLen) end = srcLen - 1;
        std::int32_t n{end - start + 1};
        if (n > wstride) n = wstride;
        // Evaluate in f64, normalise, then quantise.
        std::array<double, 256> fw{};
        double sum{0.0};
        for (std::int32_t k{0}; k < n; k++) {
            fw[static_cast<std::size_t>(k)] = filter_eval(kind, ((start + k) - center) * fscale);
            sum += fw[static_cast<std::size_t>(k)];
        }
        const double inv{sum != 0.0 ? 1.0 / sum : 0.0};
        std::int16_t* w{weights + static_cast<std::size_t>(i) * static_cast<std::size_t>(wstride)};
        std::int32_t isum{0};
        std::int32_t big{0};
        for (std::int32_t k{0}; k < n; k++) {
            auto q{static_cast<std::int32_t>(
                std::lrint(fw[static_cast<std::size_t>(k)] * inv * K_FIX_ONE))};
            // Clip — extreme aspect ratios can push a single tap past i16.
            q = q < -32768 ? -32768 : q > 32767 ? 32767 : q;
            w[k] = static_cast<std::int16_t>(q);
            isum += q;
            if (std::abs(q) > std::abs(w[big])) big = k;
        }
        // Make the integer sum exact so a flat field stays flat.
        w[big] = static_cast<std::int16_t>(w[big] + (K_FIX_ONE - isum));
        spans[i] = {start, n};
    }
}

inline std::uint8_t clamp_u8(std::int32_t v) {
    return static_cast<std::uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v);
}

// Horizontal pass: src_w×src_h → dst_w×src_h. spans/weights index by dst x.
// All addressing is via running pointers — no index*stride multiplies in the
// inner loop (same shape as the blueprint; the 4-channel body autovectorizes).
void horiz_pass(const std::uint8_t* src, std::size_t srcW, std::size_t srcH, std::uint8_t* dst,
                std::size_t dstW, const Span* spans, const std::int16_t* weights,
                std::size_t wstride) {
    const std::size_t srcRow{srcW * 4};
    const std::size_t dstRow{dstW * 4};
    const std::uint8_t* srow{src};
    std::uint8_t* drow{dst};
    for (std::size_t y{0}; y < srcH; y++, srow += srcRow, drow += dstRow) {
        const std::int16_t* w{weights};
        std::uint8_t* dp{drow};
        for (std::size_t x{0}; x < dstW; x++, w += wstride, dp += 4) {
            const Span s{spans[x]};
            const std::uint8_t* sp{srow + static_cast<std::size_t>(s.start) * 4};
            std::int32_t acc0{K_FIX_ROUND};
            std::int32_t acc1{K_FIX_ROUND};
            std::int32_t acc2{K_FIX_ROUND};
            std::int32_t acc3{K_FIX_ROUND};
            for (std::int32_t k{0}; k < s.n; k++, sp += 4) {
                const std::int32_t wk{w[k]};
                acc0 += sp[0] * wk;
                acc1 += sp[1] * wk;
                acc2 += sp[2] * wk;
                acc3 += sp[3] * wk;
            }
            dp[0] = clamp_u8(acc0 >> K_FIX_SHIFT);
            dp[1] = clamp_u8(acc1 >> K_FIX_SHIFT);
            dp[2] = clamp_u8(acc2 >> K_FIX_SHIFT);
            dp[3] = clamp_u8(acc3 >> K_FIX_SHIFT);
        }
    }
}

// Vertical pass: dst_w×src_h → dst_w×dst_h. Inner loop walks a row at a time
// (contiguous bytes) so the tap stride is `rowBytes` — no per-tap multiply.
void vert_pass(const std::uint8_t* src, std::size_t dstW, std::uint8_t* dst, std::size_t dstH,
               const Span* spans, const std::int16_t* weights, std::size_t wstride) {
    const std::size_t rowBytes{dstW * 4};
    std::uint8_t* drow{dst};
    const std::int16_t* w{weights};
    for (std::size_t y{0}; y < dstH; y++, drow += rowBytes, w += wstride) {
        const Span s{spans[y]};
        const std::uint8_t* col0{src + static_cast<std::size_t>(s.start) * rowBytes};
        for (std::size_t i{0}; i < rowBytes; i++) {
            std::int32_t acc{K_FIX_ROUND};
            const std::uint8_t* sp{col0 + i};
            for (std::int32_t k{0}; k < s.n; k++, sp += rowBytes)
                acc += static_cast<std::int32_t>(*sp) * w[k];
            drow[i] = clamp_u8(acc >> K_FIX_SHIFT);
        }
    }
}

}  // namespace

// Resize RGBA8 via separable two-pass (horizontal then vertical).
export Bytes resize_rgba8(ByteView src, std::uint32_t srcW, std::uint32_t srcH, std::uint32_t dstW,
                          std::uint32_t dstH, Filter filter) {
    const int kind{static_cast<int>(filter)};
    const double xs{static_cast<double>(dstW) / srcW};
    const double ys{static_cast<double>(dstH) / srcH};
    // 256 mirrors build_weights' support cap.
    const auto wsx{std::min<std::size_t>(
        256, static_cast<std::size_t>(std::ceil(filter_radius(kind) / (xs < 1.0 ? xs : 1.0))) * 2
                 + 2)};
    const auto wsy{std::min<std::size_t>(
        256, static_cast<std::size_t>(std::ceil(filter_radius(kind) / (ys < 1.0 ? ys : 1.0))) * 2
                 + 2)};
    std::vector<Span> xspans(dstW);
    std::vector<Span> yspans(dstH);
    std::vector<std::int16_t> xw(static_cast<std::size_t>(dstW) * wsx);
    std::vector<std::int16_t> yw(static_cast<std::size_t>(dstH) * wsy);
    build_weights(kind, static_cast<std::int32_t>(srcW), static_cast<std::int32_t>(dstW),
                  xspans.data(), xw.data(), static_cast<std::int32_t>(wsx));
    build_weights(kind, static_cast<std::int32_t>(srcH), static_cast<std::int32_t>(dstH),
                  yspans.data(), yw.data(), static_cast<std::int32_t>(wsy));

    Bytes tmp(static_cast<std::size_t>(dstW) * srcH * 4);
    Bytes out(static_cast<std::size_t>(dstW) * dstH * 4);
    horiz_pass(src.data(), srcW, srcH, tmp.data(), dstW, xspans.data(), xw.data(), wsx);
    vert_pass(tmp.data(), dstW, out.data(), dstH, yspans.data(), yw.data(), wsy);
    return out;
}

// ─── rotate / flip / modulate (port of image_resize.cpp) ────────────────────

// degrees ∈ {90, 180, 270}; anything else copies. dst dims swap for 90/270.
export Bytes rotate_rgba8(ByteView src, std::uint32_t w, std::uint32_t h, std::uint32_t degrees) {
    Bytes dst(src.size());
    const std::uint8_t* sp{src.data()};
    if (degrees == 90) {
        // 90° CW: dst[x, y] = src[y, src_h-1-x]; dst is h×w.
        const std::size_t dstRow{static_cast<std::size_t>(h) * 4};
        std::uint8_t* dcol{dst.data() + dstRow - 4};
        for (std::uint32_t y{0}; y < h; y++, dcol -= 4) {
            std::uint8_t* dp{dcol};
            for (std::uint32_t x{0}; x < w; x++, sp += 4, dp += dstRow)
                std::memcpy(dp, sp, 4);
        }
    } else if (degrees == 180) {
        const std::size_t total{static_cast<std::size_t>(w) * h};
        std::uint8_t* dp{dst.data() + (total - 1) * 4};
        for (std::size_t i{0}; i < total; i++, sp += 4, dp -= 4) std::memcpy(dp, sp, 4);
    } else if (degrees == 270) {
        const std::size_t dstRow{static_cast<std::size_t>(h) * 4};
        std::uint8_t* dcol{dst.data()};
        for (std::uint32_t y{0}; y < h; y++, dcol += 4) {
            std::uint8_t* dp{dcol + static_cast<std::size_t>(w - 1) * dstRow};
            for (std::uint32_t x{0}; x < w; x++, sp += 4, dp -= dstRow)
                std::memcpy(dp, sp, 4);
        }
    } else {
        std::memcpy(dst.data(), sp, src.size());
    }
    return dst;
}

export Bytes flip_rgba8(ByteView src, std::uint32_t w, std::uint32_t h, bool horizontal) {
    Bytes dst(src.size());
    const std::size_t row{static_cast<std::size_t>(w) * 4};
    if (horizontal) {
        const std::uint8_t* srow{src.data()};
        std::uint8_t* drow{dst.data()};
        for (std::uint32_t y{0}; y < h; y++, srow += row, drow += row) {
            const std::uint8_t* sp{srow};
            std::uint8_t* dp{drow + row - 4};
            for (std::uint32_t x{0}; x < w; x++, sp += 4, dp -= 4) std::memcpy(dp, sp, 4);
        }
    } else {
        const std::uint8_t* sp{src.data() + static_cast<std::size_t>(h - 1) * row};
        std::uint8_t* dp{dst.data()};
        for (std::uint32_t y{0}; y < h; y++, sp -= row, dp += row) std::memcpy(dp, sp, row);
    }
    return dst;
}

// In-place brightness × saturation on RGBA8. saturation lerps each channel
// toward the pixel's Rec.601 luma (0 → greyscale, 1 → identity, >1 → boost);
// brightness is a straight multiply on the result. Alpha untouched. Linear,
// no gamma correction — same model Sharp's `modulate` uses.
export void modulate_rgba8(std::uint8_t* buf, std::size_t len, float brightness,
                           float saturation) {
    auto clampf{[](float v) {
        return static_cast<std::uint8_t>(std::min(std::max(v + 0.5f, 0.0f), 255.0f));
    }};
    for (std::size_t i{0}; i + 4 <= len; i += 4) {
        const auto r{static_cast<float>(buf[i + 0])};
        const auto g{static_cast<float>(buf[i + 1])};
        const auto b{static_cast<float>(buf[i + 2])};
        const float y{0.299f * r + 0.587f * g + 0.114f * b};
        buf[i + 0] = clampf(((r - y) * saturation + y) * brightness);
        buf[i + 1] = clampf(((g - y) * saturation + y) * brightness);
        buf[i + 2] = clampf(((b - y) * saturation + y) * brightness);
        // alpha unchanged
    }
}

// ─── palette quantizer (port of quantize.rs) ────────────────────────────────

namespace {

// Nearest-palette index for one RGBA point. Squared Euclidean over all four
// channels; strict `<` keeps the first (lowest) index on ties.
std::uint32_t nearest_palette(const std::uint8_t* palette, std::uint32_t k, std::int32_t r,
                              std::int32_t g, std::int32_t b, std::int32_t a) {
    std::uint32_t best{0};
    std::int32_t bestD{0x7fffffff};
    for (std::uint32_t i{0}; i < k; i++) {
        const std::int32_t dr{r - palette[i * 4 + 0]};
        const std::int32_t dg{g - palette[i * 4 + 1]};
        const std::int32_t db{b - palette[i * 4 + 2]};
        const std::int32_t da{a - palette[i * 4 + 3]};
        const std::int32_t d{dr * dr + dg * dg + db * db + da * da};
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

struct ColorBox {
    // Slice into the shared `order` index buffer.
    std::uint32_t lo;
    std::uint32_t hi;
    std::array<std::uint8_t, 4> min;
    std::array<std::uint8_t, 4> max;

    [[nodiscard]] std::size_t widest_channel() const {
        std::size_t best{0};
        std::int32_t span{-1};
        for (std::size_t c{0}; c < 4; c++) {
            const std::int32_t s{static_cast<std::int32_t>(max[c])
                                 - static_cast<std::int32_t>(min[c])};
            if (s > span) {
                span = s;
                best = c;
            }
        }
        return best;
    }
};

// Recompute a box's tight min/max over its pixel slice.
ColorBox shrink(const std::uint8_t* rgba, const std::uint32_t* order, std::uint32_t lo,
                std::uint32_t hi) {
    ColorBox b{.lo = lo, .hi = hi, .min = {255, 255, 255, 255}, .max = {0, 0, 0, 0}};
    for (std::uint32_t i{lo}; i < hi; i++) {
        const std::uint8_t* p{rgba + static_cast<std::size_t>(order[i]) * 4};
        for (std::size_t c{0}; c < 4; c++) {
            if (p[c] < b.min[c]) b.min[c] = p[c];
            if (p[c] > b.max[c]) b.max[c] = p[c];
        }
    }
    return b;
}

inline std::int32_t clamp255(std::int32_t v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

// Floyd–Steinberg error diffusion, serpentine scan, classic 7/3/5/1 ÷16
// kernel. Two i32 error rows (i16 overflows when the palette doesn't span the
// source range — the residual grows without bound across the row).
void map_floyd_steinberg(const std::uint8_t* rgba, std::uint32_t w, std::uint32_t h,
                         const std::uint8_t* palette, std::uint32_t k, std::uint8_t* indices) {
    const std::size_t stride{static_cast<std::size_t>(w) * 4};
    std::vector<std::int32_t> cur(stride, 0);
    std::vector<std::int32_t> nxt(stride, 0);

    for (std::uint32_t y{0}; y < h; y++) {
        const bool ltr{(y & 1U) == 0};
        const std::int64_t step{ltr ? 1 : -1};
        std::int64_t x{ltr ? 0 : static_cast<std::int64_t>(w) - 1};
        while (x >= 0 && x < static_cast<std::int64_t>(w)) {
            const std::size_t px{static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)};
            const std::size_t off{static_cast<std::size_t>(x) * 4};

            // Candidate colour = source + accumulated error (clamped for the
            // search; the *unclamped* error is what propagates so rounding
            // doesn't accumulate bias).
            std::array<std::int32_t, 4> cand{};
            for (std::size_t c{0}; c < 4; c++)
                cand[c] = static_cast<std::int32_t>(rgba[px * 4 + c]) + cur[off + c];

            const std::uint32_t idx{nearest_palette(palette, k, clamp255(cand[0]),
                                                    clamp255(cand[1]), clamp255(cand[2]),
                                                    clamp255(cand[3]))};
            indices[px] = static_cast<std::uint8_t>(idx);

            const std::int64_t xr{x + step};
            const std::int64_t xl{x - step};
            const bool xrOk{xr >= 0 && xr < static_cast<std::int64_t>(w)};
            const bool xlOk{xl >= 0 && xl < static_cast<std::int64_t>(w)};
            for (std::size_t c{0}; c < 4; c++) {
                const std::int32_t err{cand[c]
                                       - static_cast<std::int32_t>(palette[idx * 4 + c])};
                if (xrOk) cur[static_cast<std::size_t>(xr) * 4 + c] += (err * 7) >> 4;
                if (xlOk) nxt[static_cast<std::size_t>(xl) * 4 + c] += (err * 3) >> 4;
                nxt[off + c] += (err * 5) >> 4;
                if (xrOk) nxt[static_cast<std::size_t>(xr) * 4 + c] += err >> 4;
            }
            x += step;
        }
        // Slide: next row's error becomes current; clear next.
        std::swap(cur, nxt);
        std::ranges::fill(nxt, 0);
    }
}

}  // namespace

export struct QuantizeResult {
    Bytes palette;  // [colors][4] RGBA
    Bytes indices;  // one palette index per input pixel
    std::uint32_t colors{};
    bool hasAlpha{};  // any palette entry alpha < 255 → caller writes tRNS
};

// Median-cut colour quantizer (Heckbert '82): treat RGBA pixels as points in
// a 4-D box, repeatedly split the box with the largest channel range at that
// channel's median until there are N boxes; each box's mean becomes a palette
// entry. Mapping is nearest-entry, optionally with Floyd–Steinberg dithering.
export QuantizeResult quantize(ByteView rgba, std::uint32_t w, std::uint32_t h,
                               std::uint32_t maxColors, bool dither) {
    const auto n{static_cast<std::uint32_t>(rgba.size() / 4)};
    const std::uint32_t want{std::max(1U, std::min(maxColors, 256U))};

    // `order` is a permutation of pixel indices partitioned in place; each
    // ColorBox owns a contiguous [lo,hi) slice of it.
    std::vector<std::uint32_t> order(n);
    std::iota(order.begin(), order.end(), 0U);

    std::vector<ColorBox> boxes{};
    boxes.reserve(want);
    boxes.push_back(shrink(rgba.data(), order.data(), 0, n));

    while (boxes.size() < want) {
        // Pick the box with the largest single-channel range.
        std::size_t pick{0};
        std::int32_t best{-1};
        for (std::size_t i{0}; i < boxes.size(); i++) {
            const std::size_t c{boxes[i].widest_channel()};
            const std::int32_t s{static_cast<std::int32_t>(boxes[i].max[c])
                                 - static_cast<std::int32_t>(boxes[i].min[c])};
            if (s > best) {
                best = s;
                pick = i;
            }
        }
        if (best <= 0) break;  // every remaining box is a single colour
        const ColorBox b{boxes[pick]};
        if (b.hi - b.lo < 2) break;

        const std::size_t ch{b.widest_channel()};
        std::sort(order.begin() + b.lo, order.begin() + b.hi,
                  [rgba = rgba.data(), ch](std::uint32_t pa, std::uint32_t pb) {
                      return rgba[static_cast<std::size_t>(pa) * 4 + ch]
                             < rgba[static_cast<std::size_t>(pb) * 4 + ch];
                  });
        const std::uint32_t mid{b.lo + (b.hi - b.lo) / 2};
        boxes[pick] = shrink(rgba.data(), order.data(), b.lo, mid);
        boxes.push_back(shrink(rgba.data(), order.data(), mid, b.hi));
    }

    const auto k{static_cast<std::uint32_t>(boxes.size())};
    QuantizeResult q{};
    q.colors = k;
    q.palette.assign(static_cast<std::size_t>(k) * 4, 0);
    for (std::size_t i{0}; i < boxes.size(); i++) {
        const ColorBox& b{boxes[i]};
        std::array<std::uint64_t, 4> sum{};
        for (std::uint32_t j{b.lo}; j < b.hi; j++) {
            const std::uint8_t* p{rgba.data() + static_cast<std::size_t>(order[j]) * 4};
            for (std::size_t c{0}; c < 4; c++) sum[c] += p[c];
        }
        const std::uint64_t cnt{b.hi - b.lo};
        for (std::size_t c{0}; c < 4; c++)
            q.palette[i * 4 + c] = static_cast<std::uint8_t>((sum[c] + cnt / 2) / cnt);
        if (q.palette[i * 4 + 3] < 255) q.hasAlpha = true;
    }

    q.indices.assign(n, 0);
    if (dither) {
        map_floyd_steinberg(rgba.data(), w, h, q.palette.data(), k, q.indices.data());
    } else {
        for (std::size_t px{0}; px < n; px++) {
            const std::uint8_t* p{rgba.data() + px * 4};
            q.indices[px] = static_cast<std::uint8_t>(
                nearest_palette(q.palette.data(), k, p[0], p[1], p[2], p[3]));
        }
    }
    return q;
}

// ─── minimal PNG codec (subset of codec_png.rs, self-hosted on core.compress) ─

export struct Decoded {
    Bytes rgba;
    std::uint32_t width{};
    std::uint32_t height{};
    std::optional<Bytes> iccProfile;  // decompressed iCCP payload, if present
};

export constexpr std::uint64_t DEFAULT_MAX_PIXELS{0x3FFFULL * 0x3FFFULL};

// Error messages mirror Image.rs's reject_error strings — the tests match on
// /maxPixels/, /decode failed/ and /unrecognised|decode/.
export constexpr std::string_view ERR_TOO_MANY_PIXELS{"Image: input exceeds maxPixels limit"};
export constexpr std::string_view ERR_DECODE{"Image: decode failed"};
export constexpr std::string_view ERR_UNKNOWN{
    "Image: unrecognised format (expected JPEG, PNG, WebP, GIF, BMP, TIFF, HEIC or AVIF)"};

namespace {

constexpr std::array<std::uint8_t, 8> PNG_SIG{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

std::uint32_t be32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16
           | static_cast<std::uint32_t>(p[2]) << 8 | p[3];
}

void put_be32(Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

void put_chunk(Bytes& out, std::string_view type, ByteView data) {
    put_be32(out, static_cast<std::uint32_t>(data.size()));
    const std::size_t crcStart{out.size()};
    out.insert(out.end(), type.begin(), type.end());
    out.insert(out.end(), data.begin(), data.end());
    const std::uint32_t crc{
        compress::crc32(ByteView{out.data() + crcStart, out.size() - crcStart})};
    put_be32(out, crc);
}

std::uint8_t paeth(std::int32_t a, std::int32_t b, std::int32_t c) {
    const std::int32_t p{a + b - c};
    const std::int32_t pa{std::abs(p - a)};
    const std::int32_t pb{std::abs(p - b)};
    const std::int32_t pc{std::abs(p - c)};
    if (pa <= pb && pa <= pc) return static_cast<std::uint8_t>(a);
    if (pb <= pc) return static_cast<std::uint8_t>(b);
    return static_cast<std::uint8_t>(c);
}

}  // namespace

// Sniff the container format from magic bytes (codecs.rs Format::sniff).
// Returns "" when unrecognised.
export std::string_view sniff_format(ByteView bytes) {
    if (bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF)
        return "jpeg";
    if (bytes.size() >= 8 && std::equal(PNG_SIG.begin(), PNG_SIG.end(), bytes.begin()))
        return "png";
    if (bytes.size() >= 12 && std::equal(bytes.begin(), bytes.begin() + 4, "RIFF")
        && std::equal(bytes.begin() + 8, bytes.begin() + 12, "WEBP"))
        return "webp";
    if (bytes.size() >= 2 && bytes[0] == 'B' && bytes[1] == 'M') return "bmp";
    if (bytes.size() >= 4
        && (std::equal(bytes.begin(), bytes.begin() + 4, "II*\x00")
            || std::equal(bytes.begin(), bytes.begin() + 4, "MM\x00*")))
        return "tiff";
    if (bytes.size() >= 6
        && (std::equal(bytes.begin(), bytes.begin() + 6, "GIF87a")
            || std::equal(bytes.begin(), bytes.begin() + 6, "GIF89a")))
        return "gif";
    // ISO BMFF: u32be box-size · "ftyp" · brands. HEIC and AVIF share the
    // container; scan the whole brand list and let a codec-specific brand win.
    if (bytes.size() >= 16 && std::equal(bytes.begin() + 4, bytes.begin() + 8, "ftyp")) {
        const std::size_t box{std::min(bytes.size(),
                                       std::max<std::size_t>(16, be32(bytes.data())))};
        bool miaf{false};
        std::size_t off{8};
        while (off + 4 <= box) {
            if (off == 12) {
                off += 4;  // minor_version
                continue;
            }
            const std::string_view b{reinterpret_cast<const char*>(bytes.data() + off), 4};
            if (b == "avif" || b == "avis") return "avif";
            if (b == "heic" || b == "heix" || b == "hevc" || b == "hevx") return "heic";
            if (b == "mif1" || b == "msf1") miaf = true;
            off += 4;
        }
        if (miaf) return "heic";
    }
    return "";
}

// Header-only probe: PNG IHDR dimensions without decoding.
export std::optional<std::pair<std::uint32_t, std::uint32_t>> png_probe(ByteView bytes) {
    if (bytes.size() < 8 + 8 + 13 || !std::equal(PNG_SIG.begin(), PNG_SIG.end(), bytes.begin()))
        return std::nullopt;
    if (!std::equal(bytes.begin() + 12, bytes.begin() + 16, "IHDR")) return std::nullopt;
    return std::pair{be32(bytes.data() + 16), be32(bytes.data() + 20)};
}

// Decode an 8-bit non-interlaced PNG (colour types 0/2/3/4/6) to RGBA8.
// Filters 0–4 per row; PLTE + tRNS applied for indexed; iCCP payload
// (deflate-decompressed) is carried through in `iccProfile`.
export std::expected<Decoded, std::string> png_decode(ByteView bytes, std::uint64_t maxPixels) {
    if (bytes.size() < 8 + 25 || !std::equal(PNG_SIG.begin(), PNG_SIG.end(), bytes.begin()))
        return std::unexpected{std::string{ERR_DECODE}};

    std::uint32_t w{0};
    std::uint32_t h{0};
    std::uint8_t bitDepth{0};
    std::uint8_t colorType{0};
    std::uint8_t interlace{0};
    bool sawIhdr{false};
    Bytes idat{};
    Bytes plte{};
    Bytes trns{};
    std::optional<Bytes> icc{};

    std::size_t off{8};
    while (off + 8 <= bytes.size()) {
        const std::uint32_t len{be32(bytes.data() + off)};
        if (off + 12 + static_cast<std::size_t>(len) > bytes.size())
            return std::unexpected{std::string{ERR_DECODE}};
        const std::string_view type{reinterpret_cast<const char*>(bytes.data() + off + 4), 4};
        const std::uint8_t* data{bytes.data() + off + 8};
        if (type == "IHDR") {
            if (len != 13) return std::unexpected{std::string{ERR_DECODE}};
            w = be32(data);
            h = be32(data + 4);
            bitDepth = data[8];
            colorType = data[9];
            interlace = data[12];
            sawIhdr = true;
            if (w == 0 || h == 0) return std::unexpected{std::string{ERR_DECODE}};
            // Decompression-bomb guard: checked against header dims BEFORE
            // any pixel buffer is allocated.
            if (static_cast<std::uint64_t>(w) * h > maxPixels)
                return std::unexpected{std::string{ERR_TOO_MANY_PIXELS}};
        } else if (type == "PLTE") {
            plte.assign(data, data + len);
        } else if (type == "tRNS") {
            trns.assign(data, data + len);
        } else if (type == "iCCP") {
            // keyword\0 compression_method(=0) deflate-stream
            std::size_t nameEnd{0};
            while (nameEnd < len && data[nameEnd] != 0) nameEnd++;
            if (nameEnd + 2 <= len) {
                auto prof{compress::zlib_decompress(ByteView{data + nameEnd + 2,
                                                             len - nameEnd - 2})};
                if (prof) icc = std::move(*prof);
            }
        } else if (type == "IDAT") {
            idat.insert(idat.end(), data, data + len);
        } else if (type == "IEND") {
            break;
        }
        off += 12 + static_cast<std::size_t>(len);
    }
    if (!sawIhdr || idat.empty()) return std::unexpected{std::string{ERR_DECODE}};
    // Subset codec: 8-bit non-interlaced only (everything the pipeline emits
    // and the test suite builds).
    if (bitDepth != 8 || interlace != 0) return std::unexpected{std::string{ERR_DECODE}};
    std::size_t channels{0};
    switch (colorType) {
        case 0: channels = 1; break;  // greyscale
        case 2: channels = 3; break;  // truecolour
        case 3: channels = 1; break;  // indexed
        case 4: channels = 2; break;  // grey + alpha
        case 6: channels = 4; break;  // truecolour + alpha
        default: return std::unexpected{std::string{ERR_DECODE}};
    }
    if (colorType == 3 && plte.empty()) return std::unexpected{std::string{ERR_DECODE}};

    auto raw{compress::zlib_decompress(idat)};
    if (!raw) return std::unexpected{std::string{ERR_DECODE}};
    const std::size_t stride{static_cast<std::size_t>(w) * channels};
    if (raw->size() < static_cast<std::size_t>(h) * (stride + 1))
        return std::unexpected{std::string{ERR_DECODE}};

    // Undo per-row filters in place (bpp = channels at depth 8).
    Bytes pix(static_cast<std::size_t>(h) * stride);
    const std::uint8_t* rp{raw->data()};
    for (std::uint32_t y{0}; y < h; y++) {
        const std::uint8_t f{*rp++};
        std::uint8_t* row{pix.data() + static_cast<std::size_t>(y) * stride};
        const std::uint8_t* prev{y > 0 ? row - stride : nullptr};
        if (f > 4) return std::unexpected{std::string{ERR_DECODE}};
        for (std::size_t i{0}; i < stride; i++) {
            const std::int32_t a{i >= channels ? row[i - channels] : 0};
            const std::int32_t b{prev != nullptr ? prev[i] : 0};
            const std::int32_t c{prev != nullptr && i >= channels ? prev[i - channels] : 0};
            std::int32_t v{rp[i]};
            switch (f) {
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) >> 1; break;
                case 4: v += paeth(a, b, c); break;
                default: break;
            }
            row[i] = static_cast<std::uint8_t>(v & 255);
        }
        rp += stride;
    }

    // Expand to RGBA8.
    Decoded d{};
    d.width = w;
    d.height = h;
    d.iccProfile = std::move(icc);
    d.rgba.assign(static_cast<std::size_t>(w) * h * 4, 255);
    const std::size_t n{static_cast<std::size_t>(w) * h};
    switch (colorType) {
        case 0:
            for (std::size_t i{0}; i < n; i++) {
                const std::uint8_t g{pix[i]};
                d.rgba[i * 4] = g;
                d.rgba[i * 4 + 1] = g;
                d.rgba[i * 4 + 2] = g;
            }
            break;
        case 2:
            for (std::size_t i{0}; i < n; i++) {
                d.rgba[i * 4] = pix[i * 3];
                d.rgba[i * 4 + 1] = pix[i * 3 + 1];
                d.rgba[i * 4 + 2] = pix[i * 3 + 2];
            }
            break;
        case 3: {
            const std::size_t entries{plte.size() / 3};
            for (std::size_t i{0}; i < n; i++) {
                const std::size_t idx{pix[i]};
                if (idx >= entries) return std::unexpected{std::string{ERR_DECODE}};
                d.rgba[i * 4] = plte[idx * 3];
                d.rgba[i * 4 + 1] = plte[idx * 3 + 1];
                d.rgba[i * 4 + 2] = plte[idx * 3 + 2];
                d.rgba[i * 4 + 3] = idx < trns.size() ? trns[idx] : 255;
            }
            break;
        }
        case 4:
            for (std::size_t i{0}; i < n; i++) {
                const std::uint8_t g{pix[i * 2]};
                d.rgba[i * 4] = g;
                d.rgba[i * 4 + 1] = g;
                d.rgba[i * 4 + 2] = g;
                d.rgba[i * 4 + 3] = pix[i * 2 + 1];
            }
            break;
        default:  // 6
            d.rgba = std::move(pix);
            break;
    }
    return d;
}

namespace {

// Per-row adaptive filter selection: the classic minimum-sum-of-absolute-
// differences heuristic (what libpng/libspng use). `rows` is unfiltered
// bpp-bytes-per-pixel scanline data; returns (1+stride)-per-row filtered
// scanlines ready for the IDAT deflate.
Bytes filter_scanlines(ByteView pix, std::uint32_t w, std::uint32_t h, std::size_t bpp) {
    const std::size_t stride{static_cast<std::size_t>(w) * bpp};
    Bytes raw(static_cast<std::size_t>(h) * (stride + 1));
    Bytes best(stride);
    Bytes cand(stride);
    auto sad{[](ByteView v) {
        std::uint64_t s{0};
        for (std::uint8_t b : v) {
            const auto sb{static_cast<std::int8_t>(b)};
            s += static_cast<std::uint64_t>(sb < 0 ? -static_cast<int>(sb) : sb);
        }
        return s;
    }};
    const std::uint8_t* prev{nullptr};
    for (std::uint32_t y{0}; y < h; y++) {
        const std::uint8_t* row{pix.data() + static_cast<std::size_t>(y) * stride};
        std::uint8_t bestF{0};
        std::memcpy(best.data(), row, stride);
        std::uint64_t bestS{sad(best)};
        for (std::uint8_t f{1}; f <= 4; f++) {
            for (std::size_t i{0}; i < stride; i++) {
                const std::int32_t a{i >= bpp ? row[i - bpp] : 0};
                const std::int32_t b{prev != nullptr ? prev[i] : 0};
                const std::int32_t c{prev != nullptr && i >= bpp ? prev[i - bpp] : 0};
                std::int32_t p{0};
                switch (f) {
                    case 1: p = a; break;
                    case 2: p = b; break;
                    case 3: p = (a + b) >> 1; break;
                    default: p = paeth(a, b, c); break;
                }
                cand[i] = static_cast<std::uint8_t>((row[i] - p) & 255);
            }
            const std::uint64_t s{sad(cand)};
            if (s < bestS) {
                bestS = s;
                bestF = f;
                std::swap(best, cand);
            }
        }
        std::uint8_t* rp{raw.data() + static_cast<std::size_t>(y) * (stride + 1)};
        rp[0] = bestF;
        std::memcpy(rp + 1, best.data(), stride);
        prev = row;
    }
    return raw;
}

// Shared preamble/epilogue for both encoders: signature + IHDR (+iCCP), then
// deflate(scanlines) as one IDAT + IEND. `level` < 0 → default 6.
Bytes png_assemble(std::uint32_t w, std::uint32_t h, std::uint8_t colorType, ByteView extraChunks,
                   ByteView scanlines, int level, const Bytes* icc) {
    Bytes out{};
    out.insert(out.end(), PNG_SIG.begin(), PNG_SIG.end());
    Bytes ihdr{};
    put_be32(ihdr, w);
    put_be32(ihdr, h);
    ihdr.push_back(8);          // bit depth
    ihdr.push_back(colorType);  // 6 = RGBA, 3 = indexed
    ihdr.push_back(0);          // compression
    ihdr.push_back(0);          // filter method
    ihdr.push_back(0);          // interlace
    put_chunk(out, "IHDR", ihdr);
    if (icc != nullptr && !icc->empty()) {
        // keyword ("ICC Profile", per codec_png.rs — informational only) +
        // NUL + compression_method 0 + deflate(profile).
        Bytes body{};
        constexpr std::string_view NAME{"ICC Profile"};
        body.insert(body.end(), NAME.begin(), NAME.end());
        body.push_back(0);
        body.push_back(0);
        const Bytes z{compress::zlib_compress(*icc, 6)};
        body.insert(body.end(), z.begin(), z.end());
        put_chunk(out, "iCCP", body);
    }
    out.insert(out.end(), extraChunks.begin(), extraChunks.end());
    const Bytes z{compress::zlib_compress(scanlines, level < 0 ? 6 : std::min(level, 9))};
    put_chunk(out, "IDAT", z);
    put_chunk(out, "IEND", ByteView{});
    return out;
}

}  // namespace

// Encode RGBA8 as a truecolour+alpha PNG (colour type 6, filter 0 rows —
// deterministic output; smarter per-row filter selection is a later ratio
// optimization, not a correctness need).
export Bytes png_encode_rgba(ByteView rgba, std::uint32_t w, std::uint32_t h, int level,
                             const Bytes* icc = nullptr) {
    return png_assemble(w, h, 6, ByteView{}, filter_scanlines(rgba, w, h, 4), level, icc);
}

// Quantize RGBA8 to ≤ `colors` entries and emit an indexed (colour-type 3)
// PNG with PLTE (+ tRNS when any palette entry is translucent).
export Bytes png_encode_indexed(ByteView rgba, std::uint32_t w, std::uint32_t h, int level,
                                std::uint32_t colors, bool dither, const Bytes* icc = nullptr) {
    const QuantizeResult q{quantize(rgba, w, h, colors, dither)};
    Bytes extra{};
    Bytes plte{};
    plte.reserve(static_cast<std::size_t>(q.colors) * 3);
    for (std::uint32_t i{0}; i < q.colors; i++) {
        plte.push_back(q.palette[i * 4]);
        plte.push_back(q.palette[i * 4 + 1]);
        plte.push_back(q.palette[i * 4 + 2]);
    }
    put_chunk(extra, "PLTE", plte);
    if (q.hasAlpha) {
        Bytes trns{};
        trns.reserve(q.colors);
        for (std::uint32_t i{0}; i < q.colors; i++) trns.push_back(q.palette[i * 4 + 3]);
        put_chunk(extra, "tRNS", trns);
    }
    return png_assemble(w, h, 3, extra, filter_scanlines(q.indices, w, h, 1), level, icc);
}

// ─── BMP decode (port of codec_bmp.rs) ──────────────────────────────────────
// BITMAPINFOHEADER (≥40 bytes), uncompressed BI_RGB or BI_BITFIELDS, 24/32-bit
// — what clipboards actually emit. For 32-bit BI_RGB the high byte is
// *reserved* (CF_DIB / GetDIBits / Pillow BGRX write 0 there); alpha is only
// honoured for BI_BITFIELDS with an explicit V4+ mask.
namespace {

// One contiguous run of bits in `mask` → (right-shift, bit-width).
constexpr std::pair<std::uint32_t, std::uint32_t> shift_width(std::uint32_t mask) {
    if (mask == 0) return {0, 0};
    return {static_cast<std::uint32_t>(std::countr_zero(mask)),
            static_cast<std::uint32_t>(std::popcount(mask))};
}

// Expand a `width`-bit channel value to 8-bit by scaling so 5-bit 0b11111 →
// 255 (not 248) and 1-bit alpha → 0/255.
constexpr std::uint8_t to8(std::uint32_t v, std::uint32_t width) {
    if (width == 0) return 0xFF;  // unused channel → opaque/full
    if (width == 8) return static_cast<std::uint8_t>(v);
    return static_cast<std::uint8_t>((v * 255) / ((1U << width) - 1));
}

}  // namespace

export std::expected<Decoded, std::string> bmp_decode(ByteView b, std::uint64_t maxPixels) {
    auto le16{[&](std::size_t i) {
        return static_cast<std::uint32_t>(b[i]) | static_cast<std::uint32_t>(b[i + 1]) << 8;
    }};
    auto le32{[&](std::size_t i) {
        return static_cast<std::uint32_t>(b[i]) | static_cast<std::uint32_t>(b[i + 1]) << 8
               | static_cast<std::uint32_t>(b[i + 2]) << 16
               | static_cast<std::uint32_t>(b[i + 3]) << 24;
    }};
    if (b.size() < 54 || b[0] != 'B' || b[1] != 'M')
        return std::unexpected{std::string{ERR_DECODE}};
    const std::uint32_t pixOff{le32(10)};
    const std::uint32_t ihSize{le32(14)};
    if (ihSize < 40 || 14 + static_cast<std::size_t>(ihSize) > b.size())
        return std::unexpected{std::string{ERR_DECODE}};
    const auto wRaw{static_cast<std::int32_t>(le32(18))};
    const auto hRaw{static_cast<std::int32_t>(le32(22))};
    if (wRaw <= 0 || hRaw == 0 || hRaw == std::numeric_limits<std::int32_t>::min())
        return std::unexpected{std::string{ERR_DECODE}};
    const std::uint32_t bpp{le16(28)};
    const std::uint32_t compression{le32(30)};
    if (bpp != 24 && bpp != 32) return std::unexpected{std::string{ERR_DECODE}};
    // BI_RGB = 0, BI_BITFIELDS = 3. RLE/JPEG/PNG-in-BMP need a real codec.
    if (compression != 0 && compression != 3) return std::unexpected{std::string{ERR_DECODE}};

    const auto width{static_cast<std::uint32_t>(wRaw)};
    const auto height{static_cast<std::uint32_t>(hRaw < 0 ? -hRaw : hRaw)};
    const bool topDown{hRaw < 0};
    std::uint32_t rMask{0x00FF0000};
    std::uint32_t gMask{0x0000FF00};
    std::uint32_t bMask{0x000000FF};
    std::uint32_t aMask{0};
    if (compression == 3) {
        if (b.size() < 14 + 40 + 12) return std::unexpected{std::string{ERR_DECODE}};
        rMask = le32(54);
        gMask = le32(58);
        bMask = le32(62);
        // Alpha mask is V4+ only (offset 66). V3+BITFIELDS has no alpha.
        aMask = ihSize >= 108 && b.size() >= 70 ? le32(66) : 0;
    }
    // Reject anything that isn't a single ≤8-bit-wide contiguous run.
    for (const std::uint32_t m : {rMask, gMask, bMask, aMask}) {
        if (m != 0) {
            const std::uint32_t run{m >> std::countr_zero(m)};
            if ((run & (run + 1)) != 0 || std::popcount(m) > 8)
                return std::unexpected{std::string{ERR_DECODE}};
        }
    }
    if (static_cast<std::uint64_t>(width) * height > maxPixels)
        return std::unexpected{std::string{ERR_TOO_MANY_PIXELS}};

    const std::uint32_t bppBytes{bpp / 8};
    // Rows are padded to 4-byte (DWORD) boundaries.
    const std::size_t stride{(static_cast<std::size_t>(width) * bppBytes + 3) / 4 * 4};
    if (static_cast<std::size_t>(pixOff) + stride * height > b.size())
        return std::unexpected{std::string{ERR_DECODE}};

    const auto [rs, rw]{shift_width(rMask)};
    const auto [gs, gw]{shift_width(gMask)};
    const auto [bs, bw]{shift_width(bMask)};
    const auto [as_, aw]{shift_width(aMask)};

    Decoded d{};
    d.width = width;
    d.height = height;
    d.rgba.assign(static_cast<std::size_t>(width) * height * 4, 0);
    for (std::uint32_t y{0}; y < height; y++) {
        const std::size_t srcY{topDown ? y : height - 1 - y};
        const std::uint8_t* row{b.data() + pixOff + srcY * stride};
        std::uint8_t* dst{d.rgba.data() + static_cast<std::size_t>(y) * width * 4};
        for (std::uint32_t x{0}; x < width; x++) {
            const std::uint32_t pix{
                bppBytes == 3
                    ? static_cast<std::uint32_t>(row[x * 3])
                          | static_cast<std::uint32_t>(row[x * 3 + 1]) << 8
                          | static_cast<std::uint32_t>(row[x * 3 + 2]) << 16
                    : static_cast<std::uint32_t>(row[x * 4])
                          | static_cast<std::uint32_t>(row[x * 4 + 1]) << 8
                          | static_cast<std::uint32_t>(row[x * 4 + 2]) << 16
                          | static_cast<std::uint32_t>(row[x * 4 + 3]) << 24};
            dst[x * 4] = to8((pix >> rs) & ((1U << rw) - 1), rw);
            dst[x * 4 + 1] = to8((pix >> gs) & ((1U << gw) - 1), gw);
            dst[x * 4 + 2] = to8((pix >> bs) & ((1U << bw) - 1), bw);
            dst[x * 4 + 3] = aMask == 0 ? 0xFF : to8((pix >> as_) & ((1U << aw) - 1), aw);
        }
    }
    return d;
}

// ─── GIF decode (port of codec_gif.rs) ──────────────────────────────────────
// GIF89a/87a first-frame decode: honours interlace and the GCE transparency
// index; animated/disposal/NETSCAPE loop are skipped (Sharp's `pages:1`).
namespace {

// Sub-block-aware LSB-first bit reader over GIF's length-prefixed sub-blocks.
struct GifBits {
    ByteView src;
    std::size_t i{};
    std::size_t block{};  // bytes remaining in the current sub-block
    std::uint32_t acc{};
    std::uint8_t nbits{};
    bool eof{};

    std::uint16_t read(std::uint8_t n) {
        while (nbits < n && !eof) {
            if (block == 0) {
                if (i >= src.size()) {
                    eof = true;
                    break;
                }
                block = src[i++];
                if (block == 0) {
                    eof = true;
                    break;
                }
            }
            if (i >= src.size()) {
                eof = true;
                break;
            }
            acc |= static_cast<std::uint32_t>(src[i++]) << nbits;
            block--;
            nbits += 8;
        }
        const auto v{static_cast<std::uint16_t>(acc & ((1U << n) - 1))};
        acc >>= n;
        nbits = nbits > n ? static_cast<std::uint8_t>(nbits - n) : 0;
        return v;
    }
};

// Classic LZW dict: string = prefix string + one suffix byte; reconstruct by
// walking `prefix` back to a root code (< clear). 4096 = GIF 12-bit cap.
struct GifDict {
    std::array<std::uint16_t, 4096> prefix{};
    std::array<std::uint8_t, 4096> suffix{};

    // Walk the chain into `scratch` (reversed), copy forwards into out.
    // Returns bytes written and the FIRST byte of the string (K-ω-K case).
    std::pair<std::size_t, std::uint8_t> emit(std::uint16_t code, std::uint16_t clear,
                                              std::span<std::uint8_t> out,
                                              std::span<std::uint8_t, 4097> scratch) const {
        std::size_t n{0};
        while (code >= clear) {
            scratch[n++] = suffix[code];
            code = prefix[code];
        }
        scratch[n++] = static_cast<std::uint8_t>(code);  // root: literal byte
        const std::uint8_t first{scratch[n - 1]};
        const std::size_t cap{std::min(n, out.size())};
        for (std::size_t k{0}; k < cap; k++) out[k] = scratch[n - 1 - k];
        return {cap, first};
    }
};

void gif_expand_row(const std::uint8_t* idx, std::uint8_t* out, std::size_t w,
                    const std::array<std::array<std::uint8_t, 4>, 256>& pal) {
    for (std::size_t x{0}; x < w; x++) std::memcpy(out + x * 4, pal[idx[x]].data(), 4);
}

}  // namespace

export std::expected<Decoded, std::string> gif_decode(ByteView bytes, std::uint64_t maxPixels) {
    if (bytes.size() < 13
        || !(std::equal(bytes.begin(), bytes.begin() + 6, "GIF89a")
             || std::equal(bytes.begin(), bytes.begin() + 6, "GIF87a")))
        return std::unexpected{std::string{ERR_DECODE}};
    const std::uint8_t lsdPacked{bytes[10]};
    const bool hasGct{(lsdPacked & 0x80U) != 0};
    const std::size_t gctSize{hasGct ? std::size_t{1} << ((lsdPacked & 7U) + 1) : 0};

    std::size_t i{13 + gctSize * 3};
    if (i > bytes.size()) return std::unexpected{std::string{ERR_DECODE}};
    const ByteView gct{hasGct ? bytes.subspan(13, gctSize * 3) : ByteView{}};

    std::optional<std::uint8_t> trns{};  // transparency index from the latest GCE

    // Block stream: skip extensions, take the first Image Descriptor.
    while (i < bytes.size()) {
        const std::uint8_t marker{bytes[i]};
        if (marker == 0x3B) return std::unexpected{std::string{ERR_DECODE}};  // early trailer
        if (marker == 0x21) {
            if (i + 2 > bytes.size()) return std::unexpected{std::string{ERR_DECODE}};
            const std::uint8_t label{bytes[i + 1]};
            i += 2;
            if (label == 0xF9 && i + 6 <= bytes.size() && bytes[i] == 4) {
                // GCE: blocksize=4 · packed · delay(u16) · trns-idx · 0
                if ((bytes[i + 1] & 1U) != 0) trns = bytes[i + 4];
            }
            // Skip sub-blocks regardless of label (255-byte XMP/ICC blocks
            // included — widen before the add).
            while (i < bytes.size()) {
                const std::size_t n{bytes[i]};
                i += 1 + n;
                if (n == 0) break;
            }
            continue;
        }
        if (marker != 0x2C) return std::unexpected{std::string{ERR_DECODE}};
        // Image Descriptor.
        if (i + 10 > bytes.size()) return std::unexpected{std::string{ERR_DECODE}};
        const std::uint32_t w{static_cast<std::uint32_t>(bytes[i + 5])
                              | static_cast<std::uint32_t>(bytes[i + 6]) << 8};
        const std::uint32_t h{static_cast<std::uint32_t>(bytes[i + 7])
                              | static_cast<std::uint32_t>(bytes[i + 8]) << 8};
        const std::uint8_t ipacked{bytes[i + 9]};
        const bool interlace{(ipacked & 0x40U) != 0};
        const bool hasLct{(ipacked & 0x80U) != 0};
        const std::size_t lctSize{hasLct ? std::size_t{1} << ((ipacked & 7U) + 1) : 0};
        i += 10;
        if (w == 0 || h == 0) return std::unexpected{std::string{ERR_DECODE}};
        if (static_cast<std::uint64_t>(w) * h > maxPixels)
            return std::unexpected{std::string{ERR_TOO_MANY_PIXELS}};
        ByteView ct{gct};
        if (hasLct) {
            if (i + lctSize * 3 > bytes.size()) return std::unexpected{std::string{ERR_DECODE}};
            ct = bytes.subspan(i, lctSize * 3);
            i += lctSize * 3;
        }
        if (ct.empty()) return std::unexpected{std::string{ERR_DECODE}};  // no palette at all
        if (i >= bytes.size()) return std::unexpected{std::string{ERR_DECODE}};
        const auto minCode{static_cast<std::uint8_t>(std::clamp<int>(bytes[i], 2, 11))};
        i += 1;

        // ── LZW decode ──────────────────────────────────────────────────────
        const std::size_t npix{static_cast<std::size_t>(w) * h};
        const auto clear{static_cast<std::uint16_t>(1U << minCode)};
        const auto eoi{static_cast<std::uint16_t>(clear + 1)};
        auto size{static_cast<std::uint8_t>(minCode + 1)};
        auto avail{static_cast<std::uint16_t>(eoi + 1)};
        std::optional<std::uint16_t> prev{};
        auto dict{std::make_unique<GifDict>()};
        std::array<std::uint8_t, 4097> scratch{};
        Bytes idx(npix, 0);
        std::size_t written{0};
        GifBits bits{.src = bytes, .i = i};
        while (written < npix) {
            const std::uint16_t code{bits.read(size)};
            if (bits.eof && code == 0) break;
            if (code == clear) {
                size = static_cast<std::uint8_t>(minCode + 1);
                avail = static_cast<std::uint16_t>(eoi + 1);
                prev.reset();
                continue;
            }
            if (code == eoi) break;
            std::uint8_t first{};
            if (code < avail) {
                const auto r{dict->emit(code, clear,
                                        std::span{idx}.subspan(written), scratch)};
                written += r.first;
                first = r.second;
            } else if (code == avail && prev.has_value()) {
                // K-ω-K: emit prev's expansion, then append its first byte.
                const auto r{dict->emit(*prev, clear,
                                        std::span{idx}.subspan(written), scratch)};
                written += r.first;
                first = r.second;
                if (written < npix) idx[written++] = first;
            } else {
                return std::unexpected{std::string{ERR_DECODE}};  // out-of-range code
            }
            // Deferred clear: past 4096 the encoder may keep emitting 12-bit
            // codes without growing until it sends a clear.
            if (prev.has_value() && avail < 4096) {
                dict->prefix[avail] = *prev;
                dict->suffix[avail] = first;
                avail++;
                if (avail == (1U << size) && size < 12) size++;
            }
            prev = code;
        }
        // Short/truncated streams leave a tail — fill with the transparent
        // index (or 0) instead of leaking heap bytes through the palette.
        if (written < npix)
            std::fill(idx.begin() + static_cast<std::ptrdiff_t>(written), idx.end(),
                      trns.value_or(0));

        std::array<std::array<std::uint8_t, 4>, 256> pal{};
        for (auto& p : pal) p = {0, 0, 0, 255};
        for (std::size_t c{0}; c < ct.size() / 3; c++)
            pal[c] = {ct[c * 3], ct[c * 3 + 1], ct[c * 3 + 2], 255};
        if (trns.has_value()) pal[*trns] = {0, 0, 0, 0};

        Decoded d{};
        d.width = w;
        d.height = h;
        d.rgba.assign(npix * 4, 0);
        if (interlace) {
            // Pass order: every 8th from 0, every 8th from 4, every 4th from
            // 2, every 2nd from 1 — remap to scan order while expanding.
            constexpr std::array<std::array<std::uint32_t, 2>, 4> PASSES{
                {{0, 8}, {4, 8}, {2, 4}, {1, 2}}};
            std::size_t srcY{0};
            for (const auto& p : PASSES) {
                for (std::uint32_t y{p[0]}; y < h; y += p[1], srcY++)
                    gif_expand_row(idx.data() + srcY * w,
                                   d.rgba.data() + static_cast<std::size_t>(y) * w * 4, w, pal);
            }
        } else {
            for (std::uint32_t y{0}; y < h; y++)
                gif_expand_row(idx.data() + static_cast<std::size_t>(y) * w,
                               d.rgba.data() + static_cast<std::size_t>(y) * w * 4, w, pal);
        }
        return d;
    }
    return std::unexpected{std::string{ERR_DECODE}};
}

// ─── ThumbHash (port of thumbhash.rs — Evan Wallace's LQIP, public domain) ──
// ~21–25 bytes encode the low-order DCT coefficients of L/P/Q (+A) of a
// ≤100×100 image; decoding gives a ≤32px blur with the right average colour,
// aspect ratio and rough structure.
namespace thumbhash {

constexpr std::size_t MAX_LEN{25};

struct Channel {
    float dc{};
    float scale{};
    std::array<float, 49> ac{};
    std::size_t n{};
};

// Triangular DCT-II for the (cx,cy) where cx·ny < nx·(ny−cy) — the diagonal
// half ThumbHash keeps. AC coeffs normalised to [0,1] by the per-channel max.
Channel th_dct(const float* chan, std::uint32_t w, std::uint32_t h, std::uint32_t nx,
               std::uint32_t ny) {
    Channel c{};
    std::array<float, 100> fx{};
    for (std::uint32_t cy{0}; cy < ny; cy++) {
        for (std::uint32_t cx{0}; cx * ny < nx * (ny - cy); cx++) {
            for (std::uint32_t x{0}; x < w; x++)
                fx[x] = std::cos(std::numbers::pi_v<float> / static_cast<float>(w)
                                 * static_cast<float>(cx) * (static_cast<float>(x) + 0.5f));
            float f{0.0f};
            for (std::uint32_t y{0}; y < h; y++) {
                const float fy{std::cos(std::numbers::pi_v<float> / static_cast<float>(h)
                                        * static_cast<float>(cy)
                                        * (static_cast<float>(y) + 0.5f))};
                for (std::uint32_t x{0}; x < w; x++)
                    f += chan[x + static_cast<std::size_t>(y) * w] * fx[x] * fy;
            }
            f /= static_cast<float>(w * h);
            if (cx == 0 && cy == 0) {
                c.dc = f;
            } else {
                c.ac[c.n++] = f;
                c.scale = std::max(c.scale, std::fabs(f));
            }
        }
    }
    if (c.scale > 0.0f)
        for (std::size_t k{0}; k < c.n; k++) c.ac[k] = 0.5f + 0.5f / c.scale * c.ac[k];
    return c;
}

// Encode ≤100×100 RGBA into `out`; returns the hash length.
std::size_t encode(std::array<std::uint8_t, MAX_LEN>& out, std::uint32_t w, std::uint32_t h,
                   ByteView rgba) {
    // Average colour (alpha-weighted so transparent pixels don't tug it).
    std::array<float, 4> avg{};
    for (std::size_t i{0}; i < rgba.size(); i += 4) {
        const float a{static_cast<float>(rgba[i + 3]) / 255.0f};
        avg[0] += a / 255.0f * static_cast<float>(rgba[i]);
        avg[1] += a / 255.0f * static_cast<float>(rgba[i + 1]);
        avg[2] += a / 255.0f * static_cast<float>(rgba[i + 2]);
        avg[3] += a;
    }
    if (avg[3] > 0.0f)
        for (std::size_t c{0}; c < 3; c++) avg[c] /= avg[3];

    const auto npix{static_cast<float>(w * h)};
    const bool hasAlpha{avg[3] < npix};
    const float lLimit{hasAlpha ? 5.0f : 7.0f};  // fewer luma bits if alpha
    const auto maxWh{static_cast<float>(std::max(w, h))};
    const auto lx{std::max(
        1U, static_cast<std::uint32_t>(std::lround(lLimit * static_cast<float>(w) / maxWh)))};
    const auto ly{std::max(
        1U, static_cast<std::uint32_t>(std::lround(lLimit * static_cast<float>(h) / maxWh)))};

    // RGBA → LPQA, compositing transparent pixels onto the average so the
    // DCT doesn't see a black fringe.
    std::vector<float> l(static_cast<std::size_t>(w) * h);
    std::vector<float> p(l.size());
    std::vector<float> q(l.size());
    std::vector<float> a(l.size());
    for (std::size_t i{0}, px{0}; i < rgba.size(); i += 4, px++) {
        const float al{static_cast<float>(rgba[i + 3]) / 255.0f};
        const float r{avg[0] * (1.0f - al) + al / 255.0f * static_cast<float>(rgba[i])};
        const float g{avg[1] * (1.0f - al) + al / 255.0f * static_cast<float>(rgba[i + 1])};
        const float b{avg[2] * (1.0f - al) + al / 255.0f * static_cast<float>(rgba[i + 2])};
        l[px] = (r + g + b) / 3.0f;
        p[px] = (r + g) / 2.0f - b;
        q[px] = r - g;
        a[px] = al;
    }

    const Channel lc{th_dct(l.data(), w, h, std::max(lx, 3U), std::max(ly, 3U))};
    const Channel pc{th_dct(p.data(), w, h, 3, 3)};
    const Channel qc{th_dct(q.data(), w, h, 3, 3)};
    Channel acCh{};
    if (hasAlpha) {
        acCh = th_dct(a.data(), w, h, 5, 5);
    } else {
        acCh.dc = 1.0f;
        acCh.scale = 1.0f;
    }

    const bool land{w > h};
    const std::uint32_t h24{
        static_cast<std::uint32_t>(std::lround(63.0f * lc.dc))
        | static_cast<std::uint32_t>(std::lround(31.5f + 31.5f * pc.dc)) << 6
        | static_cast<std::uint32_t>(std::lround(31.5f + 31.5f * qc.dc)) << 12
        | static_cast<std::uint32_t>(std::lround(31.0f * lc.scale)) << 18
        | static_cast<std::uint32_t>(hasAlpha) << 23};
    const std::uint16_t h16{static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(land ? ly : lx)
        | static_cast<std::uint16_t>(std::lround(63.0f * pc.scale)) << 3
        | static_cast<std::uint16_t>(std::lround(63.0f * qc.scale)) << 9
        | static_cast<std::uint16_t>(land) << 15)};
    out[0] = static_cast<std::uint8_t>(h24);
    out[1] = static_cast<std::uint8_t>(h24 >> 8);
    out[2] = static_cast<std::uint8_t>(h24 >> 16);
    out[3] = static_cast<std::uint8_t>(h16);
    out[4] = static_cast<std::uint8_t>(h16 >> 8);
    std::size_t n{5};
    if (hasAlpha) {
        out[5] = static_cast<std::uint8_t>(
            static_cast<std::uint8_t>(std::lround(15.0f * acCh.dc))
            | static_cast<std::uint8_t>(std::lround(15.0f * acCh.scale)) << 4);
        n = 6;
    }
    bool odd{false};
    for (const Channel* ch :
         {&lc, &pc, &qc, static_cast<const Channel*>(&acCh)}) {
        for (std::size_t k{0}; k < ch->n; k++) {
            const auto u{static_cast<std::uint8_t>(std::lround(15.0f * ch->ac[k]))};
            if (odd) {
                out[n - 1] |= static_cast<std::uint8_t>(u << 4);
            } else {
                out[n++] = u;
            }
            odd = !odd;
        }
    }
    return n;
}

struct NibbleReader {
    ByteView src;
    std::size_t i{};
    bool hi{};

    std::optional<std::uint8_t> next() {
        if (i >= src.size()) return std::nullopt;
        const std::uint8_t v{
            static_cast<std::uint8_t>(hi ? src[i] >> 4 : src[i] & 15U)};
        if (hi) i++;
        hi = !hi;
        return v;
    }

    std::optional<std::size_t> channel(float* out, std::uint32_t nx, std::uint32_t ny,
                                       float scale) {
        std::size_t n{0};
        for (std::uint32_t cy{0}; cy < ny; cy++) {
            for (std::uint32_t cx{cy > 0 ? 0U : 1U}; cx * ny < nx * (ny - cy); cx++) {
                const auto v{next()};
                if (!v) return std::nullopt;
                out[n++] = (static_cast<float>(*v) / 7.5f - 1.0f) * scale;
            }
        }
        return n;
    }
};

float th_idct(const float* ac, std::size_t, std::uint32_t nx, std::uint32_t ny,
              const std::array<float, 7>& fx, const std::array<float, 7>& fy) {
    float v{0.0f};
    std::size_t j{0};
    for (std::uint32_t cy{0}; cy < ny; cy++) {
        const float fy2{fy[cy] * 2.0f};
        for (std::uint32_t cx{cy > 0 ? 0U : 1U}; cx * ny < nx * (ny - cy); cx++)
            v += ac[j++] * fx[cx] * fy2;
    }
    return v;
}

inline std::uint8_t clamp8(float v) {
    return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f);
}

// Decode a hash to a ≤32px RGBA image.
std::expected<Decoded, std::string> decode(ByteView hash) {
    if (hash.size() < 5) return std::unexpected{std::string{ERR_DECODE}};
    const std::uint32_t h24{static_cast<std::uint32_t>(hash[0])
                            | static_cast<std::uint32_t>(hash[1]) << 8
                            | static_cast<std::uint32_t>(hash[2]) << 16};
    const std::uint16_t h16{static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(hash[3]) | static_cast<std::uint16_t>(hash[4]) << 8)};
    const float lDc{static_cast<float>(h24 & 63U) / 63.0f};
    const float pDc{static_cast<float>((h24 >> 6) & 63U) / 31.5f - 1.0f};
    const float qDc{static_cast<float>((h24 >> 12) & 63U) / 31.5f - 1.0f};
    const float lScale{static_cast<float>((h24 >> 18) & 31U) / 31.0f};
    const bool hasAlpha{(h24 >> 23) != 0};
    const float pScale{static_cast<float>((h16 >> 3) & 63U) / 63.0f};
    const float qScale{static_cast<float>((h16 >> 9) & 63U) / 63.0f};
    const bool land{(h16 >> 15) != 0};
    const std::uint32_t lMax{hasAlpha ? 5U : 7U};
    const std::uint32_t lx{std::max(3U, land ? lMax : static_cast<std::uint32_t>(h16 & 7U))};
    const std::uint32_t ly{std::max(3U, land ? static_cast<std::uint32_t>(h16 & 7U) : lMax)};
    float aDc{1.0f};
    float aScale{1.0f};
    std::size_t off{5};
    if (hasAlpha) {
        if (hash.size() < 6) return std::unexpected{std::string{ERR_DECODE}};
        aDc = static_cast<float>(hash[5] & 15U) / 15.0f;
        aScale = static_cast<float>(hash[5] >> 4) / 15.0f;
        off = 6;
    }

    NibbleReader nibbles{.src = hash, .i = off};
    std::array<float, 49> lAc{};
    std::array<float, 5> pAc{};
    std::array<float, 5> qAc{};
    std::array<float, 14> aAc{};
    const auto ln{nibbles.channel(lAc.data(), lx, ly, lScale)};
    // 1.25× saturation boost compensates for 4-bit quantisation washing the
    // chroma out — matches the reference impl.
    const auto pn{nibbles.channel(pAc.data(), 3, 3, pScale * 1.25f)};
    const auto qn{nibbles.channel(qAc.data(), 3, 3, qScale * 1.25f)};
    std::optional<std::size_t> an{0};
    if (hasAlpha) an = nibbles.channel(aAc.data(), 5, 5, aScale);
    if (!ln || !pn || !qn || !an) return std::unexpected{std::string{ERR_DECODE}};

    const float ratio{static_cast<float>(lx) / static_cast<float>(ly)};
    const std::uint32_t w{
        ratio > 1.0f ? 32U : static_cast<std::uint32_t>(std::lround(32.0f * ratio))};
    const std::uint32_t h{
        ratio > 1.0f ? static_cast<std::uint32_t>(std::lround(32.0f / ratio)) : 32U};
    Decoded d{};
    d.width = w;
    d.height = h;
    d.rgba.assign(static_cast<std::size_t>(w) * h * 4, 0);

    std::array<float, 7> fx{};
    std::array<float, 7> fy{};
    for (std::uint32_t y{0}; y < h; y++) {
        for (std::uint32_t x{0}; x < w; x++) {
            float lv{lDc};
            float pv{pDc};
            float qv{qDc};
            float av{aDc};
            const std::uint32_t nfx{std::max(lx, hasAlpha ? 5U : 3U)};
            for (std::uint32_t c{0}; c < nfx; c++)
                fx[c] = std::cos(std::numbers::pi_v<float> / static_cast<float>(w)
                                 * (static_cast<float>(x) + 0.5f) * static_cast<float>(c));
            for (std::uint32_t c{0}; c < std::max(ly, hasAlpha ? 5U : 3U); c++)
                fy[c] = std::cos(std::numbers::pi_v<float> / static_cast<float>(h)
                                 * (static_cast<float>(y) + 0.5f) * static_cast<float>(c));
            lv += th_idct(lAc.data(), *ln, lx, ly, fx, fy);
            pv += th_idct(pAc.data(), *pn, 3, 3, fx, fy);
            qv += th_idct(qAc.data(), *qn, 3, 3, fx, fy);
            if (hasAlpha) av += th_idct(aAc.data(), *an, 5, 5, fx, fy);
            const float b{lv - 2.0f / 3.0f * pv};
            const float r{(3.0f * lv - b + qv) / 2.0f};
            const float g{r - qv};
            const std::size_t o{(static_cast<std::size_t>(y) * w + x) * 4};
            d.rgba[o] = clamp8(r);
            d.rgba[o + 1] = clamp8(g);
            d.rgba[o + 2] = clamp8(b);
            d.rgba[o + 3] = clamp8(av);
        }
    }
    return d;
}

}  // namespace thumbhash

// `.placeholder()` body (Image.rs make_placeholder): box-downscale to ≤100
// (box is the only filter that's correct for "average everything in a cell" —
// lanczos would ring into the DCT), ThumbHash encode → decode, PNG-encode the
// ≤32px render.
export std::expected<Decoded, std::string> make_placeholder_render(ByteView rgba,
                                                                   std::uint32_t sw,
                                                                   std::uint32_t sh) {
    constexpr std::uint32_t MAX_IN{100};
    std::uint32_t w{sw};
    std::uint32_t h{sh};
    Bytes owned{};
    ByteView pixels{rgba};
    if (w > MAX_IN || h > MAX_IN) {
        const float r{static_cast<float>(w) / static_cast<float>(h)};
        if (r > 1.0f) {
            w = MAX_IN;
            h = std::max(1U, static_cast<std::uint32_t>(
                                 std::lround(static_cast<float>(MAX_IN) / r)));
        } else {
            h = MAX_IN;
            w = std::max(1U, static_cast<std::uint32_t>(
                                 std::lround(static_cast<float>(MAX_IN) * r)));
        }
        owned = resize_rgba8(rgba, sw, sh, w, h, Filter::Box);
        pixels = owned;
    }
    std::array<std::uint8_t, thumbhash::MAX_LEN> buf{};
    const std::size_t n{thumbhash::encode(buf, w, h, pixels)};
    return thumbhash::decode(ByteView{buf.data(), n});
}

}  // namespace mbun::image
