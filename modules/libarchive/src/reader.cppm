// Reader state machine for Bun.Archive input, driving the real libarchive
// in-memory reader.
// ref: .mbun/bun-ref/src/libarchive/lib.rs (ReadArchive) and
//      .mbun/bun-zig-src/src/libarchive/libarchive.zig (BufferReadStream).
export module mbun.libarchive.reader;

import std;
import mbun.libarchive.entry;
import mbun.libarchive.native_backend;

export namespace mbun::libarchive {

enum class ReaderState : std::uint8_t { New, Open, EntryData, Eof, Failed };

struct ReaderOptions {
    bool supportTar { true };
    bool supportGnuTar { true };
    bool supportGzip { true };
    bool readConcatenatedArchives { true };
};

class Reader {
private:
    ReaderOptions options_;
    ReaderState state_ { ReaderState::New };
    std::span<const std::byte> input_ {};
    MemoryReader backend_;
    Entry current_ {};
    bool have_current_ { false };

public:
    explicit Reader(ReaderOptions options = {}) : options_ { options } {}

    [[nodiscard]] ReaderState state() const { return state_; }
    [[nodiscard]] const ReaderOptions& options() const { return options_; }
    [[nodiscard]] std::span<const std::byte> input() const { return input_; }

    // Open the reader over an in-memory archive image (any format libarchive
    // supports: tar/zip/gzip/...).
    [[nodiscard]] std::expected<void, Error> open_memory(std::span<const std::byte> input) {
        input_ = input;
        auto opened = backend_.open(input);
        if (!opened) {
            state_ = ReaderState::Failed;
            return std::unexpected(opened.error());
        }
        state_ = ReaderState::Open;
        return {};
    }

    // Fetch the next entry header, or nullopt at end of archive.
    [[nodiscard]] std::expected<std::optional<Entry>, Error> next_header() {
        if (state_ == ReaderState::Failed || state_ == ReaderState::New) {
            return std::unexpected(Error { .message = "reader not open" });
        }
        auto next = backend_.next();
        if (!next) {
            state_ = ReaderState::Failed;
            return std::unexpected(next.error());
        }
        if (!next->has_value()) {
            state_ = ReaderState::Eof;
            have_current_ = false;
            return std::optional<Entry> {};
        }
        current_ = **next;
        have_current_ = true;
        state_ = ReaderState::EntryData;
        return next;
    }

    // Read the current entry's data fully into memory.
    [[nodiscard]] std::expected<std::vector<std::byte>, Error> read_entry_data(
        const Entry& entry,
        std::size_t maxSize = 64 * 1024 * 1024) {
        if (entry.size < 0 || static_cast<std::uint64_t>(entry.size) > maxSize) {
            return std::unexpected(Error { .message = "invalid archive entry size" });
        }
        if (state_ != ReaderState::EntryData) {
            return std::unexpected(Error { .message = "no current entry to read" });
        }
        return backend_.read_data(maxSize);
    }
};

} // namespace mbun::libarchive
