// test_image.cpp — mbun.image kernel + PNG codec unit tests.
//
// Mirrors the kernel properties compat/bun/test/js/bun/image/image-kernels.test.ts
// pins (DC preservation, nearest single-tap, identity resize, FS dither
// tracking) at the C++ layer, plus PNG round-trip identity, so a JS-layer
// regression can be split from a kernel regression.
import std;
import mbun.image;

namespace {

namespace img = mbun::image;
using Bytes = std::vector<std::uint8_t>;

int gChecks{0};
int gFailures{0};

void check(bool ok, std::string_view what) {
    ++gChecks;
    if (!ok) {
        ++gFailures;
        std::println("  FAIL {}", what);
    }
}

Bytes flat_rgba(std::uint32_t w, std::uint32_t h, std::array<std::uint8_t, 4> px) {
    Bytes b(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i{0}; i < b.size(); i += 4)
        std::copy(px.begin(), px.end(), b.begin() + static_cast<std::ptrdiff_t>(i));
    return b;
}

}  // namespace

int main() {
    constexpr std::array FILTERS{img::Filter::Box,      img::Filter::Bilinear,
                                 img::Filter::Lanczos3, img::Filter::Mitchell,
                                 img::Filter::Nearest,  img::Filter::Cubic,
                                 img::Filter::Lanczos2, img::Filter::Mks2013,
                                 img::Filter::Mks2021};

    // DC gain: a flat field must come back flat under every filter/scale.
    for (const auto f : FILTERS) {
        const Bytes src{flat_rgba(9, 7, {173, 173, 173, 255})};
        for (const auto [w, h] : {std::pair{3U, 3U}, {18U, 14U}, {9U, 7U}}) {
            const Bytes out{img::resize_rgba8(src, 9, 7, w, h, f)};
            bool ok{true};
            for (std::size_t i{0}; i < out.size(); i += 4)
                ok = ok && out[i] == 173 && out[i + 3] == 255;
            check(ok, std::format("flat field filter={} {}x{}", static_cast<int>(f), w, h));
        }
    }

    // nearest keeps discrete values on a checkerboard downscale.
    {
        Bytes checker(8 * 8 * 4);
        for (std::size_t y{0}; y < 8; y++)
            for (std::size_t x{0}; x < 8; x++) {
                const std::uint8_t v{((x + y) & 1U) != 0U ? std::uint8_t{255} : std::uint8_t{0}};
                const std::size_t i{(y * 8 + x) * 4};
                checker[i] = v;
                checker[i + 1] = v;
                checker[i + 2] = v;
                checker[i + 3] = 255;
            }
        const Bytes out{img::resize_rgba8(checker, 8, 8, 4, 4, img::Filter::Nearest)};
        bool ok{true};
        for (std::size_t i{0}; i < out.size(); i += 4) ok = ok && (out[i] == 0 || out[i] == 255);
        check(ok, "nearest checker stays B/W");
    }

    // rotate 90: dst(h-1-x' ... ) — corner tracking on a 4×3 pattern.
    {
        Bytes src(4 * 3 * 4, 0);
        auto set{[&](std::size_t x, std::size_t y, std::uint8_t r) { src[(y * 4 + x) * 4] = r; }};
        set(0, 0, 11);  // TL
        set(3, 0, 22);  // TR
        set(0, 2, 33);  // BL
        set(3, 2, 44);  // BR
        const Bytes r90{img::rotate_rgba8(src, 4, 3, 90)};
        // dst is 3×4: TL→(2,0) TR→(2,3) BL→(0,0) BR→(0,3)
        check(r90[(0 * 3 + 2) * 4] == 11 && r90[(3 * 3 + 2) * 4] == 22
                  && r90[(0 * 3 + 0) * 4] == 33 && r90[(3 * 3 + 0) * 4] == 44,
              "rotate 90 corners");
        const Bytes r180{img::rotate_rgba8(src, 4, 3, 180)};
        check(r180[(2 * 4 + 3) * 4] == 11 && r180[(0 * 4 + 0) * 4] == 44, "rotate 180 corners");
    }

    // PNG round-trip identity (RGBA, filter 0 encode; decoder handles 0–4).
    {
        Bytes src(7 * 5 * 4);
        for (std::size_t i{0}; i < src.size(); i++)
            src[i] = static_cast<std::uint8_t>((i * 37 + 11) & 255);
        const Bytes png{img::png_encode_rgba(src, 7, 5, -1)};
        const auto d{img::png_decode(png, img::DEFAULT_MAX_PIXELS)};
        check(d.has_value() && d->width == 7 && d->height == 5 && d->rgba == src,
              "png rgba round-trip");
    }

    // Indexed PNG: 2×2 four exact colours at colors=4 round-trips exactly.
    {
        Bytes src{0,   0, 255, 255, 255, 0,   0, 255,
                  0, 255,   0, 255, 255, 255, 255, 255};
        const Bytes png{img::png_encode_indexed(src, 2, 2, -1, 4, false)};
        check(png[25] == 3, "indexed colour type byte");
        const auto d{img::png_decode(png, img::DEFAULT_MAX_PIXELS)};
        check(d.has_value() && d->rgba == src, "indexed png round-trip");
    }

    // maxPixels guard fires from the header, decode of junk fails cleanly.
    {
        const Bytes src{flat_rgba(4, 4, {1, 2, 3, 255})};
        const Bytes png{img::png_encode_rgba(src, 4, 4, -1)};
        const auto d{img::png_decode(png, 10)};
        check(!d.has_value() && d.error() == img::ERR_TOO_MANY_PIXELS, "maxPixels guard");
        const auto j{img::png_decode(Bytes{1, 2, 3, 4, 5, 6, 7, 8, 9}, 1 << 20)};
        check(!j.has_value(), "junk decode fails");
    }

    // FS dither on a 0..255 ramp with colors=2: outside-gamut saturates, and
    // window means inside the gamut track the source.
    {
        Bytes ramp(256ULL * 8 * 4);
        for (std::size_t y{0}; y < 8; y++)
            for (std::size_t x{0}; x < 256; x++) {
                const std::size_t i{(y * 256 + x) * 4};
                ramp[i] = static_cast<std::uint8_t>(x);
                ramp[i + 1] = static_cast<std::uint8_t>(x);
                ramp[i + 2] = static_cast<std::uint8_t>(x);
                ramp[i + 3] = 255;
            }
        const img::QuantizeResult q{img::quantize(ramp, 256, 8, 2, true)};
        check(q.colors == 2, "quantize ramp k=2");
        const std::uint8_t lo{std::min(q.palette[0], q.palette[4])};
        const std::uint8_t hi{std::max(q.palette[0], q.palette[4])};
        check(lo >= 56 && lo <= 72 && hi >= 184 && hi <= 200,
              std::format("median-cut palette ≈ [64,192], got [{},{}]", lo, hi));
        bool track{true};
        for (std::uint32_t cx{static_cast<std::uint32_t>(lo) + 16};
             cx + 16 <= static_cast<std::uint32_t>(hi); cx += 16) {
            std::uint64_t sum{0};
            for (std::int32_t dx{-8}; dx < 8; dx++)
                for (std::size_t y{0}; y < 8; y++) {
                    const std::size_t px{y * 256 + cx + static_cast<std::size_t>(dx)};
                    sum += q.palette[static_cast<std::size_t>(q.indices[px]) * 4];
                }
            const double mean{static_cast<double>(sum) / 128.0};
            if (std::fabs(mean - cx) >= 12.0) track = false;
        }
        check(track, "dither window mean tracks source");
        // dither=false is a single hard step.
        const img::QuantizeResult qs{img::quantize(ramp, 256, 8, 2, false)};
        int transitions{0};
        for (std::size_t x{1}; x < 256; x++)
            if (qs.indices[x] != qs.indices[x - 1]) transitions++;
        check(transitions == 1, "no-dither single step");
    }

    // JPEG decoder robustness against corrupt/truncated input (was: segfault).
    // A fuzzed/damaged JPEG must return a bounded error, never read out of
    // bounds. Build a valid 2×2 baseline JPEG, then mutate it.
    {
        const Bytes src{flat_rgba(2, 2, {200, 100, 50, 255})};
        const Bytes jpeg{img::jpg::jpeg_encode_rgba(src, 2, 2, 90, false, nullptr)};

        // Sanity: the clean stream decodes to the right dimensions.
        const auto ok{img::jpg::jpeg_decode(jpeg, img::DEFAULT_MAX_PIXELS)};
        check(ok.has_value() && ok->width == 2 && ok->height == 2, "jpeg 2x2 round-trip");

        // Documented repro vector: offset 167 is the SOF0 component-count (Nf)
        // byte. The encoder writes 3 there; flipping it to 0xFC (252) used to
        // drive the component reader ~750 bytes past the ~600-byte buffer and
        // segfault. It must now be a bounded error.
        check(jpeg.size() > 167 && jpeg[167] == 3, "jpeg offset-167 is Nf==3");
        {
            Bytes corrupt{jpeg};
            corrupt[167] = 0xFC;
            const auto d{img::jpg::jpeg_decode(corrupt, img::DEFAULT_MAX_PIXELS)};
            check(!d.has_value(), "jpeg corrupt Nf (offset 167) -> bounded error");
        }

        // Single-byte-flip sweep over the whole stream: every mutation must
        // return (value or error) without crashing — covers DQT/DHT/SOF/SOS
        // length, count and run-length overreads.
        {
            bool survived{true};
            for (std::size_t i{2}; i < jpeg.size(); ++i)
                for (const std::uint8_t mut : {std::uint8_t{0x00}, std::uint8_t{0xFC},
                                               std::uint8_t{0xFF}}) {
                    Bytes c{jpeg};
                    c[i] = mut;
                    const auto d{img::jpg::jpeg_decode(c, img::DEFAULT_MAX_PIXELS)};
                    (void)d;  // reaching here without crashing is the assertion
                }
            check(survived, "jpeg single-byte-flip sweep does not crash");
        }

        // Truncation sweep: every prefix must return a bounded error, not read
        // past the shortened buffer.
        {
            bool survived{true};
            for (std::size_t n{2}; n < jpeg.size(); ++n) {
                const Bytes c{jpeg.begin(), jpeg.begin() + static_cast<std::ptrdiff_t>(n)};
                const auto d{img::jpg::jpeg_decode(c, img::DEFAULT_MAX_PIXELS)};
                (void)d;
            }
            check(survived, "jpeg truncation sweep does not crash");
        }
    }

    std::println("image tests: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
