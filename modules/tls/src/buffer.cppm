// buffer.cppm — bounded-free byte queues used by the TLS/socket seam.
//
// ref: bun-ref/src/http/ThreadSafeStreamBuffer.rs and
// bun-zig-src/src/http/ThreadSafeStreamBuffer.zig.  The native uSockets
// read/write callbacks remain DEFERRED(S-net); this module keeps ownership and
// partial-read semantics independent of that backend.
export module mbun.tls.buffer;

import std;

namespace mbun::tls {

export class ByteBuffer {
private:
    std::vector<std::uint8_t> bytes_ {};
    std::size_t offset_ {0};

    void compact_() {
        if (offset_ == 0) {
            return;
        }
        if (offset_ == bytes_.size()) {
            bytes_.clear();
            offset_ = 0;
            return;
        }
        bytes_.erase(bytes_.begin(), bytes_.begin() + static_cast<std::ptrdiff_t>(offset_));
        offset_ = 0;
    }

public:
    ByteBuffer() = default;

    explicit ByteBuffer(std::size_t reserveSize) {
        bytes_.reserve(reserveSize);
    }

    [[nodiscard]] bool empty() const noexcept {
        return readable_size() == 0;
    }

    [[nodiscard]] std::size_t readable_size() const noexcept {
        return bytes_.size() - offset_;
    }

    [[nodiscard]] std::span<const std::uint8_t> readable() const noexcept {
        return {bytes_.data() + offset_, readable_size()};
    }

    void append(std::span<const std::uint8_t> data) {
        if (data.empty()) {
            return;
        }
        if (offset_ != 0 && bytes_.capacity() - (bytes_.size() - offset_) >= data.size()) {
            compact_();
        }
        bytes_.insert(bytes_.end(), data.begin(), data.end());
    }

    std::size_t consume(std::size_t count) noexcept {
        const auto consumed {std::min(count, readable_size())};
        offset_ += consumed;
        if (offset_ == bytes_.size()) {
            bytes_.clear();
            offset_ = 0;
        }
        return consumed;
    }

    void clear() noexcept {
        bytes_.clear();
        offset_ = 0;
    }
};

} // namespace mbun::tls
