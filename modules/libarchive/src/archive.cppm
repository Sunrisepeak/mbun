// Bun.Archive facade: pure data model and reader/writer composition.
// ref: .mbun/bun-ref/src/runtime/api/Archive.rs (Archive) and the matching
// Zig implementation in src/libarchive/libarchive.zig.
// libarchive linkage is live (native_backend drives the real archive_read_*/
// archive_write_* API); only the JavaScriptCore bindings remain DEFERRED.
export module mbun.libarchive;

export import mbun.libarchive.entry;
export import mbun.libarchive.native_backend;
export import mbun.libarchive.reader;
export import mbun.libarchive.writer;

import std;

export namespace mbun::libarchive {

struct ArchiveOptions {
    Compression compression { Compression::None };
    GzipOptions gzip {};
};

class Archive {
private:
    std::vector<std::byte> data_;
    ArchiveOptions options_;

public:
    Archive() = default;
    explicit Archive(std::span<const std::byte> data, ArchiveOptions options = {})
        : data_ { data.begin(), data.end() }, options_ { options } {}

    [[nodiscard]] std::span<const std::byte> data() const { return data_; }
    [[nodiscard]] const ArchiveOptions& options() const { return options_; }
    [[nodiscard]] bool backend_available() const {
        return NativeBackend::capabilities().available;
    }

    [[nodiscard]] Reader reader(ReaderOptions options = {}) const {
        return Reader { options };
    }

    [[nodiscard]] Writer writer() const {
        return Writer {
            WriterOptions {
                .compression = options_.compression,
                .gzip = options_.gzip,
            },
        };
    }
};

}  // namespace mbun::libarchive
