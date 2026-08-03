// mbun.io — file-oriented semantics built on the mbun.sys seam.
//
// Ref: bun-ref/src/io/source.rs, openForWriting.rs, PipeReader.rs,
// PipeWriter.rs and sys/file.rs; Zig equivalents live in src/io/source.zig,
// openForWriting.zig and src/sys/File.zig. This first port records ownership,
// partial-I/O, EOF, and fd-relative operation boundaries only.
export module mbun.io;

import std;
import mbun.sys;

namespace mbun::io {

export enum class OpenMode : std::uint32_t {
    read_only = 1u << 0,
    write_only = 1u << 1,
    create = 1u << 2,
    truncate = 1u << 3,
    close_on_exec = 1u << 4,
};

export constexpr OpenMode operator|(OpenMode left, OpenMode right) {
    return static_cast<OpenMode>(static_cast<std::uint32_t>(left)
                                 | static_cast<std::uint32_t>(right));
}

// Move-only ownership wrapper. Destruction is intentionally best-effort;
// callers that need observable close errors use close() explicitly.
export class File {
private:
    sys::Fd fd_ {};
    sys::SyscallBackend* backend_ {nullptr};

public:
    File() = default;
    File(sys::Fd fd, sys::SyscallBackend& backend)
        : fd_ {fd}
        , backend_ {&backend} {}
    ~File() { reset_(); }

    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&& other) noexcept
        : fd_ {std::exchange(other.fd_, sys::Fd::invalid())}
        , backend_ {std::exchange(other.backend_, nullptr)} {}
    File& operator=(File&& other) noexcept {
        if (this != &other) {
            reset_();
            fd_ = std::exchange(other.fd_, sys::Fd::invalid());
            backend_ = std::exchange(other.backend_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] sys::Fd fd() const { return fd_; }
    [[nodiscard]] bool is_open() const { return fd_.is_valid() && backend_ != nullptr; }

    sys::Result<std::size_t> read(std::span<std::byte> buffer) {
        if (!is_open()) {
            return std::unexpected(sys::SystemError {
                .code = sys::ErrnoCode::bad_file_descriptor,
                .fd = fd_,
                .syscall = sys::SyscallTag::read,
            });
        }
        return backend_->read(fd_, buffer);
    }

    sys::Result<std::size_t> write(std::span<const std::byte> buffer) {
        if (!is_open()) {
            return std::unexpected(sys::SystemError {
                .code = sys::ErrnoCode::bad_file_descriptor,
                .fd = fd_,
                .syscall = sys::SyscallTag::write,
            });
        }
        return backend_->write(fd_, buffer);
    }

    sys::Result<std::size_t> read_at(std::span<std::byte> buffer, std::uint64_t offset) {
        if (!is_open()) {
            return std::unexpected(sys::SystemError {
                .code = sys::ErrnoCode::bad_file_descriptor,
                .fd = fd_,
                .syscall = sys::SyscallTag::pread,
            });
        }
        return backend_->read_at(fd_, buffer, offset);
    }

    sys::Result<std::size_t> write_at(std::span<const std::byte> buffer,
                                      std::uint64_t offset) {
        if (!is_open()) {
            return std::unexpected(sys::SystemError {
                .code = sys::ErrnoCode::bad_file_descriptor,
                .fd = fd_,
                .syscall = sys::SyscallTag::pwrite,
            });
        }
        return backend_->write_at(fd_, buffer, offset);
    }

    sys::Result<void> close() {
        if (!is_open()) {
            return {};
        }
        auto result {backend_->close(fd_)};
        fd_ = sys::Fd::invalid();
        backend_ = nullptr;
        return result;
    }

private:
    void reset_() noexcept {
        if (is_open()) {
            (void)backend_->close(fd_);
            fd_ = sys::Fd::invalid();
            backend_ = nullptr;
        }
    }
};

} // namespace mbun::io
