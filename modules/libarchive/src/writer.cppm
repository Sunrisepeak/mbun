// Writer state machine for Bun.Archive output, driving the real libarchive
// in-memory writer.
// ref: .mbun/bun-ref/src/libarchive/lib.rs (WriteArchive/GrowingBuffer) and
//      .mbun/bun-ref/src/runtime/api/Archive.rs (PAX restricted + gzip).
export module mbun.libarchive.writer;

import std;
import mbun.libarchive.entry;
import mbun.libarchive.native_backend;

export namespace mbun::libarchive {

enum class Compression : std::uint8_t { None, Gzip };

// Output container format. Tar = PAX-restricted ustar (bun's default); Zip =
// the ZIP container.
enum class Format : std::uint8_t { Tar, Zip };

struct GzipOptions {
    std::uint8_t level { 6 };
};

struct WriterOptions {
    Format format { Format::Tar };
    Compression compression { Compression::None };
    GzipOptions gzip {};
};

class Writer {
private:
    WriterOptions options_;
    MemoryWriter backend_;
    bool open_ { false };
    // The header/data/finish protocol is buffered into a single add_entry call
    // so callers can stream data across write_data() invocations.
    Entry pending_ {};
    std::vector<std::byte> pending_data_;
    bool have_pending_ { false };

    [[nodiscard]] WriteFormat native_format() const {
        return options_.format == Format::Zip ? WriteFormat::Zip
                                              : WriteFormat::PaxRestricted;
    }
    [[nodiscard]] WriteFilter native_filter() const {
        return options_.compression == Compression::Gzip ? WriteFilter::Gzip
                                                          : WriteFilter::None;
    }

public:
    explicit Writer(WriterOptions options = {}) : options_ { options } {}

    [[nodiscard]] const WriterOptions& options() const { return options_; }
    [[nodiscard]] std::span<const std::byte> output() const { return backend_.bytes(); }
    [[nodiscard]] bool is_open() const { return open_; }

    [[nodiscard]] std::expected<void, Error> open_memory() {
        auto opened = backend_.open(native_format(), native_filter(), options_.gzip.level);
        if (!opened) {
            open_ = false;
            return std::unexpected(opened.error());
        }
        open_ = true;
        return {};
    }

    [[nodiscard]] std::expected<void, Error> write_header(const Entry& entry) {
        if (!open_) {
            return std::unexpected(Error { .message = "writer not open" });
        }
        // Flush any previous entry that never saw finish_entry().
        if (have_pending_) {
            auto flushed = finish_entry();
            if (!flushed) {
                return flushed;
            }
        }
        pending_ = entry;
        pending_data_.clear();
        have_pending_ = true;
        return {};
    }

    [[nodiscard]] std::expected<void, Error> write_data(std::span<const std::byte> data) {
        if (!have_pending_) {
            return std::unexpected(Error { .message = "write_data without write_header" });
        }
        pending_data_.insert(pending_data_.end(), data.begin(), data.end());
        return {};
    }

    [[nodiscard]] std::expected<void, Error> finish_entry() {
        if (!have_pending_) {
            return {};
        }
        pending_.size = static_cast<std::int64_t>(pending_data_.size());
        auto added = backend_.add_entry(pending_, pending_data_);
        have_pending_ = false;
        pending_data_.clear();
        if (!added) {
            return std::unexpected(added.error());
        }
        return {};
    }

    [[nodiscard]] std::expected<void, Error> close() {
        if (!open_) {
            return std::unexpected(Error { .message = "writer not open" });
        }
        if (have_pending_) {
            auto flushed = finish_entry();
            if (!flushed) {
                return flushed;
            }
        }
        auto closed = backend_.close();
        open_ = false;
        if (!closed) {
            return std::unexpected(closed.error());
        }
        return {};
    }

    // Convenience: the assembled archive image after close().
    [[nodiscard]] std::vector<std::byte> take() { return backend_.take(); }
};

} // namespace mbun::libarchive
