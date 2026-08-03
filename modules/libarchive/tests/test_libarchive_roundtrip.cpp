// test_libarchive_roundtrip.cpp — exercises the real libarchive native backend
// end to end: construct an archive in memory (tar / gzip-compressed tar / zip)
// via Writer, then read it back with Reader and verify entry metadata + data.
import std;
import mbun.libarchive;

using namespace mbun::libarchive;

namespace {

int failed{0};

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failed;
    }
}

std::vector<std::byte> bytes_of(std::string_view s) {
    std::vector<std::byte> v(s.size());
    std::memcpy(v.data(), s.data(), s.size());
    return v;
}

std::string string_of(std::span<const std::byte> b) {
    std::string s(b.size(), '\0');
    if (!b.empty()) {
        std::memcpy(s.data(), b.data(), b.size());
    }
    return s;
}

struct NamedBlob {
    std::string path;
    std::string data;
};

// Write the blobs into an archive image using the high-level Writer.
std::vector<std::byte> build_archive(Format format, Compression compression,
                                     std::span<const NamedBlob> blobs) {
    Writer writer{WriterOptions{.format = format, .compression = compression}};
    auto opened = writer.open_memory();
    expect(opened.has_value(), "writer.open_memory succeeds");
    for (const auto& blob : blobs) {
        Entry e;
        e.pathname = blob.path;
        e.fileType = FileType::Regular;
        e.permissions = 0644;
        auto h = writer.write_header(e);
        expect(h.has_value(), "write_header succeeds");
        auto d = writer.write_data(bytes_of(blob.data));
        expect(d.has_value(), "write_data succeeds");
        auto f = writer.finish_entry();
        expect(f.has_value(), "finish_entry succeeds");
    }
    auto closed = writer.close();
    expect(closed.has_value(), "writer.close succeeds");
    return writer.take();
}

// Read an archive image back and collect (path,data) pairs.
std::vector<NamedBlob> read_archive(std::span<const std::byte> image) {
    std::vector<NamedBlob> out;
    Reader reader;
    auto opened = reader.open_memory(image);
    expect(opened.has_value(), "reader.open_memory succeeds");
    if (!opened) {
        std::cerr << "  open error: " << opened.error().message << '\n';
        return out;
    }
    for (;;) {
        auto header = reader.next_header();
        expect(header.has_value(), "next_header succeeds");
        if (!header || !header->has_value()) {
            break;
        }
        const Entry& e = **header;
        auto data = reader.read_entry_data(e);
        expect(data.has_value(), "read_entry_data succeeds");
        out.push_back(NamedBlob{e.pathname, data ? string_of(*data) : std::string{}});
    }
    return out;
}

void check_roundtrip(std::string_view label, Format format, Compression compression) {
    const std::vector<NamedBlob> blobs{
        {"hello.txt", "Hello, libarchive!"},
        {"dir/nested.bin", std::string(5000, 'x')},
        {"empty.txt", ""},
    };
    auto image = build_archive(format, compression, blobs);
    expect(!image.empty(), std::string{label} + ": archive image is non-empty");

    auto got = read_archive(image);
    expect(got.size() == blobs.size(), std::string{label} + ": entry count matches");
    for (std::size_t i = 0; i < blobs.size() && i < got.size(); ++i) {
        expect(got[i].path == blobs[i].path, std::string{label} + ": pathname matches");
        expect(got[i].data == blobs[i].data, std::string{label} + ": content matches");
    }
}

} // namespace

int main() {
    // Native backend is wired to the real library.
    auto caps = NativeBackend::capabilities();
    expect(caps.available, "libarchive backend available");
    expect(caps.tar && caps.gzip, "tar + gzip capabilities reported");
    expect(NativeBackend::version().find("libarchive 3.") != std::string::npos ||
               NativeBackend::version().rfind("libarchive", 0) == 0,
           "version string looks like libarchive 3.x");

    // tar (PAX-restricted ustar), gzip-compressed tar, and zip round-trips.
    check_roundtrip("tar", Format::Tar, Compression::None);
    check_roundtrip("tar.gz", Format::Tar, Compression::Gzip);
    check_roundtrip("zip", Format::Zip, Compression::None);

    // A gzip-compressed tar image must actually differ from the raw tar image
    // (the gzip filter ran) yet decode to identical contents.
    const std::vector<NamedBlob> blob{{"repeat.txt", std::string(20000, 'A')}};
    auto raw = build_archive(Format::Tar, Compression::None, blob);
    auto gz = build_archive(Format::Tar, Compression::Gzip, blob);
    expect(gz.size() < raw.size(), "gzip filter compresses a highly repetitive tar");
    // gzip magic 0x1f 0x8b.
    expect(gz.size() >= 2 && gz[0] == std::byte{0x1f} && gz[1] == std::byte{0x8b},
           "gzip image carries gzip magic bytes");

    if (failed == 0) {
        std::cout << "all libarchive round-trip tests passed\n";
    }
    return failed == 0 ? 0 : 1;
}
