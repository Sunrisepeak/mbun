// buffer.cppm — the allocation-owning output buffer seam.
//
// Bun's PrettyBuf owns the rendered bytes while Source owns fixed 4096-byte
// stdout/stderr staging buffers. This type is the shared pure-logic layer:
// callers may reserve the staging capacity, append without intermediate
// strings, and expose a read-only view to a future sink.
// ref: .mbun/bun-ref/src/bun_core/output.rs (PrettyBuf, Source)
// ref: .mbun/bun-zig-src/src/bun_core/output.zig (Source buffers)
export module mbun.output.buffer;

import std;

export namespace mbun::output {

class Buffer {
private:
    std::vector<std::byte> bytes_;

public:
    explicit Buffer(std::size_t capacity = 4096) {
        bytes_.reserve(capacity);
    }

    void reserve(std::size_t capacity) {
        bytes_.reserve(capacity);
    }

    void append(std::span<const std::byte> bytes) {
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    }

    void append(std::string_view text) {
        const auto* first{reinterpret_cast<const std::byte*>(text.data())};
        append(std::span<const std::byte>{first, text.size()});
    }

    void push_back(std::byte value) {
        bytes_.push_back(value);
    }

    void clear() noexcept {
        bytes_.clear();
    }

    [[nodiscard]] bool empty() const noexcept {
        return bytes_.empty();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return bytes_.size();
    }

    [[nodiscard]] std::span<const std::byte> view() const noexcept {
        return bytes_;
    }

    [[nodiscard]] std::string_view text() const noexcept {
        return {reinterpret_cast<const char*>(bytes_.data()), bytes_.size()};
    }

    [[nodiscard]] std::vector<std::byte> take() && {
        return std::move(bytes_);
    }
};

}  // namespace mbun::output
