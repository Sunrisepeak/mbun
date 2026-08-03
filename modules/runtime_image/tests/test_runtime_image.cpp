import std;
import mbun.runtime_image;

namespace {
int failures{0};
void check(bool value, std::string_view label) {
    if (!value) { ++failures; std::println("FAIL {}", label); }
}
}

int main() {
    using namespace mbun::runtime_image;
    check(format_from_extension("thumb.JPEG") == Format::jpeg, "extension dispatch");
    check(format_from_extension("x.tiff") == Format::tiff, "long extension dispatch");
    check(!format_from_extension("x.raw").has_value(), "unknown extension");
    const std::array<std::uint8_t, 8> png{0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    check(sniff_format(png) == Format::png, "magic dispatch");
    check(mime_type(Format::webp) == "image/webp", "mime mapping");

    Image image{std::vector<std::uint8_t>{png.begin(), png.end()}};
    image.resize(ResizeOptions{100, 80, true}).rotate(450).flip().flop().output(EncodeOptions{Format::webp, 80});
    check(image.resize_options().width == 100 && image.resize_options().without_enlargement, "pipeline resize slot");
    check(image.rotation() == 90 && image.flipped() && image.flopped(), "pipeline transform slots");
    check(image.output(EncodeOptions{Format::jpeg}).metadata(DeferredBackend{}).error() == ImageError::backend_unavailable,
          "metadata backend seam");
    check(image.encode(DeferredBackend{}).error() == ImageError::backend_unavailable, "codec backend seam");

    std::println("runtime_image: {} failures", failures);
    return failures == 0 ? 0 : 1;
}
