// Native backend for libarchive — the real archive_read_*/archive_write_* C
// API wired in through a global module fragment.
// ref: .mbun/bun-ref/src/libarchive/lib.rs (ReadArchive/WriteArchive) and
//      .mbun/bun-zig-src/src/libarchive/libarchive.zig (BufferReadStream/
//      GrowingBuffer). Matches bun's usage: read supports every format+filter
//      (archive_read_support_format_all + filter_all); write uses PAX-restricted
//      ustar (optionally gzip-filtered) or zip, into an in-memory growing buffer.
module;

#include <archive.h>
#include <archive_entry.h>

export module mbun.libarchive.native_backend;

import std;
import mbun.libarchive.entry;

export namespace mbun::libarchive {

enum class Status : std::int8_t {
    Eof = 1,
    Ok = 0,
    Retry = -10,
    Warn = -20,
    Failed = -25,
    Fatal = -30,
};

[[nodiscard]] constexpr bool succeeded(Status status) {
    return status == Status::Ok || status == Status::Warn;
}

struct Error {
    Status status { Status::Failed };
    std::string message;
};

struct BackendCapabilities {
    bool available { false };
    bool tar { false };
    bool gzip { false };
    bool streaming { false };
};

// Output archive format/compression selection for the writer.
enum class WriteFormat : std::uint8_t {
    // PAX-restricted ustar — the tar flavour bun's Bun.Archive emits.
    PaxRestricted,
    // ZIP container (deflate).
    Zip,
};

enum class WriteFilter : std::uint8_t {
    None,
    Gzip,
};

// Stable capability probe. The library is compiled in, so it is always
// available; report the format/filter surface the round-trip tests rely on.
class NativeBackend {
public:
    [[nodiscard]] static BackendCapabilities capabilities() {
        return BackendCapabilities {
            .available = archive_version_number() > 0,
            .tar = true,
            .gzip = true,
            .streaming = true,
        };
    }

    [[nodiscard]] static std::string version() {
        const char* s = archive_version_string();
        return s ? std::string { s } : std::string {};
    }
};

namespace detail {

inline FileType file_type_from_ae(unsigned int mode) {
    switch (mode & AE_IFMT) {
    case AE_IFREG: return FileType::Regular;
    case AE_IFDIR: return FileType::Directory;
    case AE_IFLNK: return FileType::Symlink;
    default:       return FileType::Other;
    }
}

inline unsigned int ae_from_file_type(FileType t) {
    switch (t) {
    case FileType::Regular:   return AE_IFREG;
    case FileType::Directory: return AE_IFDIR;
    case FileType::Symlink:   return AE_IFLNK;
    default:                  return AE_IFREG;
    }
}

inline Error err_from(struct archive* a, std::string_view fallback) {
    const char* m = a ? archive_error_string(a) : nullptr;
    return Error {
        .status = Status::Failed,
        .message = m ? std::string { m } : std::string { fallback },
    };
}

} // namespace detail

// In-memory reader over a byte buffer. Supports every format/filter libarchive
// was built with (tar, zip, gzip, ...). Non-copyable, RAII-owns the handle.
class MemoryReader {
private:
    struct archive* a_ { nullptr };
    bool open_ { false };

public:
    MemoryReader() = default;
    MemoryReader(const MemoryReader&) = delete;
    MemoryReader& operator=(const MemoryReader&) = delete;
    MemoryReader(MemoryReader&& other) noexcept
        : a_ { other.a_ }, open_ { other.open_ } {
        other.a_ = nullptr;
        other.open_ = false;
    }
    MemoryReader& operator=(MemoryReader&& other) noexcept {
        if (this != &other) {
            reset();
            a_ = other.a_;
            open_ = other.open_;
            other.a_ = nullptr;
            other.open_ = false;
        }
        return *this;
    }
    ~MemoryReader() { reset(); }

    void reset() {
        if (a_ != nullptr) {
            archive_read_free(a_);
            a_ = nullptr;
        }
        open_ = false;
    }

    // Open the reader over an in-memory archive image.
    [[nodiscard]] std::expected<void, Error> open(std::span<const std::byte> input) {
        reset();
        a_ = archive_read_new();
        if (a_ == nullptr) {
            return std::unexpected(Error { .status = Status::Fatal,
                                           .message = "archive_read_new failed" });
        }
        archive_read_support_format_all(a_);
        archive_read_support_filter_all(a_);
        if (archive_read_open_memory(a_, input.data(), input.size()) != ARCHIVE_OK) {
            auto e = detail::err_from(a_, "archive_read_open_memory failed");
            reset();
            return std::unexpected(std::move(e));
        }
        open_ = true;
        return {};
    }

    // Advance to the next entry header. Returns nullopt at end of archive.
    [[nodiscard]] std::expected<std::optional<Entry>, Error> next() {
        if (!open_) {
            return std::unexpected(Error { .message = "reader not open" });
        }
        struct archive_entry* ae = nullptr;
        int rc = archive_read_next_header(a_, &ae);
        if (rc == ARCHIVE_EOF) {
            return std::optional<Entry> {};
        }
        if (rc != ARCHIVE_OK && rc != ARCHIVE_WARN) {
            return std::unexpected(detail::err_from(a_, "archive_read_next_header failed"));
        }
        Entry e;
        if (const char* p = archive_entry_pathname(ae)) {
            e.pathname = p;
        }
        if (const char* s = archive_entry_symlink(ae)) {
            e.symlink = s;
        }
        e.size = static_cast<std::int64_t>(archive_entry_size(ae));
        e.fileType = detail::file_type_from_ae(archive_entry_filetype(ae));
        e.permissions = static_cast<std::uint32_t>(archive_entry_perm(ae));
        e.mtimeSeconds = static_cast<std::int64_t>(archive_entry_mtime(ae));
        e.mtimeNanoseconds = static_cast<std::int32_t>(archive_entry_mtime_nsec(ae));
        return std::optional<Entry> { std::move(e) };
    }

    // Read the current entry's data body fully into memory.
    [[nodiscard]] std::expected<std::vector<std::byte>, Error> read_data(
        std::size_t maxSize = 256ull * 1024 * 1024) {
        if (!open_) {
            return std::unexpected(Error { .message = "reader not open" });
        }
        std::vector<std::byte> out;
        std::array<std::byte, 64 * 1024> buf {};
        for (;;) {
            la_ssize_t n = archive_read_data(a_, buf.data(), buf.size());
            if (n < 0) {
                return std::unexpected(detail::err_from(a_, "archive_read_data failed"));
            }
            if (n == 0) {
                break;
            }
            if (out.size() + static_cast<std::size_t>(n) > maxSize) {
                return std::unexpected(Error { .message = "archive entry exceeds max size" });
            }
            out.insert(out.end(), buf.begin(), buf.begin() + n);
        }
        return out;
    }
};

// In-memory writer producing an archive image (tar/zip, optionally gzip). Uses
// a custom write callback appending to a growing std::vector — the same
// growing-buffer strategy bun's WriteArchive uses.
class MemoryWriter {
private:
    struct archive* a_ { nullptr };
    std::vector<std::byte> buffer_;
    bool open_ { false };

    static la_ssize_t write_cb(struct archive*, void* client, const void* data, std::size_t len) {
        auto* self = static_cast<MemoryWriter*>(client);
        const auto* p = static_cast<const std::byte*>(data);
        self->buffer_.insert(self->buffer_.end(), p, p + len);
        return static_cast<la_ssize_t>(len);
    }

public:
    MemoryWriter() = default;
    MemoryWriter(const MemoryWriter&) = delete;
    MemoryWriter& operator=(const MemoryWriter&) = delete;
    ~MemoryWriter() { reset(); }

    void reset() {
        if (a_ != nullptr) {
            archive_write_free(a_);
            a_ = nullptr;
        }
        open_ = false;
    }

    [[nodiscard]] std::expected<void, Error> open(
        WriteFormat format,
        WriteFilter filter = WriteFilter::None,
        int gzipLevel = 6) {
        reset();
        buffer_.clear();
        a_ = archive_write_new();
        if (a_ == nullptr) {
            return std::unexpected(Error { .status = Status::Fatal,
                                           .message = "archive_write_new failed" });
        }
        switch (format) {
        case WriteFormat::PaxRestricted:
            archive_write_set_format_pax_restricted(a_);
            break;
        case WriteFormat::Zip:
            archive_write_set_format_zip(a_);
            break;
        }
        if (filter == WriteFilter::Gzip) {
            archive_write_add_filter_gzip(a_);
            std::string opt = "gzip:compression-level=" + std::to_string(gzipLevel);
            archive_write_set_options(a_, opt.c_str());
        }
        // One byte per block avoids trailing NUL padding on the memory image.
        archive_write_set_bytes_in_last_block(a_, 1);
        if (archive_write_open(a_, this, nullptr, &MemoryWriter::write_cb, nullptr) != ARCHIVE_OK) {
            auto e = detail::err_from(a_, "archive_write_open failed");
            reset();
            return std::unexpected(std::move(e));
        }
        open_ = true;
        return {};
    }

    // Write one entry header followed by its data body.
    [[nodiscard]] std::expected<void, Error> add_entry(
        const Entry& entry,
        std::span<const std::byte> data) {
        if (!open_) {
            return std::unexpected(Error { .message = "writer not open" });
        }
        struct archive_entry* ae = archive_entry_new();
        if (ae == nullptr) {
            return std::unexpected(Error { .status = Status::Fatal,
                                           .message = "archive_entry_new failed" });
        }
        archive_entry_set_pathname(ae, entry.pathname.c_str());
        archive_entry_set_size(ae, static_cast<la_int64_t>(entry.size));
        archive_entry_set_filetype(ae, detail::ae_from_file_type(entry.fileType));
        archive_entry_set_perm(ae, static_cast<mode_t>(entry.permissions));
        archive_entry_set_mtime(ae, static_cast<time_t>(entry.mtimeSeconds), entry.mtimeNanoseconds);
        if (entry.fileType == FileType::Symlink && !entry.symlink.empty()) {
            archive_entry_set_symlink(ae, entry.symlink.c_str());
        }
        int rc = archive_write_header(a_, ae);
        if (rc != ARCHIVE_OK && rc != ARCHIVE_WARN) {
            auto e = detail::err_from(a_, "archive_write_header failed");
            archive_entry_free(ae);
            return std::unexpected(std::move(e));
        }
        if (!data.empty()) {
            la_ssize_t n = archive_write_data(a_, data.data(), data.size());
            if (n < 0) {
                auto e = detail::err_from(a_, "archive_write_data failed");
                archive_entry_free(ae);
                return std::unexpected(std::move(e));
            }
        }
        archive_entry_free(ae);
        return {};
    }

    // Flush and close; the archive image is then available via take()/bytes().
    [[nodiscard]] std::expected<void, Error> close() {
        if (a_ == nullptr) {
            return std::unexpected(Error { .message = "writer not open" });
        }
        if (archive_write_close(a_) != ARCHIVE_OK) {
            return std::unexpected(detail::err_from(a_, "archive_write_close failed"));
        }
        open_ = false;
        return {};
    }

    [[nodiscard]] std::span<const std::byte> bytes() const { return buffer_; }
    [[nodiscard]] std::vector<std::byte> take() { return std::move(buffer_); }
};

} // namespace mbun::libarchive
