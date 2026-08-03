export module mbun.runtime_socket.buffer;

import std;

export namespace mbun::runtime_socket {

class StreamBuffer {
private:
    std::vector<std::byte> storage_ {};
    std::size_t cursor_ { 0 };

    void compact_() {
        if (cursor_ == 0)
            return;
        if (cursor_ == storage_.size()) {
            storage_.clear();
            cursor_ = 0;
            return;
        }
        storage_.erase(storage_.begin(), storage_.begin() + static_cast<std::ptrdiff_t>(cursor_));
        cursor_ = 0;
    }

public:
    void append(std::span<const std::byte> bytes) {
        if (bytes.empty())
            return;
        compact_();
        storage_.insert(storage_.end(), bytes.begin(), bytes.end());
    }

    std::span<const std::byte> readable() const {
        return { storage_.data() + cursor_, storage_.size() - cursor_ };
    }

    std::vector<std::byte> consume(std::size_t count) {
        count = std::min(count, readable().size());
        std::vector<std::byte> result { readable().begin(), readable().begin() + static_cast<std::ptrdiff_t>(count) };
        cursor_ += count;
        if (cursor_ > storage_.size() / 2)
            compact_();
        return result;
    }

    void clear() {
        storage_.clear();
        cursor_ = 0;
    }

    std::size_t size() const { return readable().size(); }
    bool empty() const { return size() == 0; }
};

}
