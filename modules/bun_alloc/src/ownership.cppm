export module mbun.bun_alloc.ownership;

import std;

export namespace mbun::bun_alloc {

enum class Error : std::uint8_t {
    invalid_alignment,
    size_overflow,
    out_of_memory,
    invalid_owner,
    stale_generation,
};

[[nodiscard]] constexpr bool valid_alignment(std::size_t alignment) noexcept {
    return alignment != 0 && std::has_single_bit(alignment);
}

class BlockView {
private:
    std::byte* data_ { nullptr };
    std::size_t size_ { 0 };
    std::size_t alignment_ { 1 };

    constexpr BlockView(std::byte* data, std::size_t size, std::size_t alignment) noexcept
        : data_ { data }
        , size_ { size }
        , alignment_ { alignment } {}

public:
    [[nodiscard]] std::byte* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t alignment() const noexcept { return alignment_; }
    [[nodiscard]] std::span<std::byte> bytes() const noexcept { return { data_, size_ }; }

    friend class Block;
};

class Block {
private:
    std::byte* data_ { nullptr };
    std::size_t size_ { 0 };
    std::size_t alignment_ { 1 };
    std::uint64_t ownerId_ { 0 };
    std::uint64_t generation_ { 0 };

    Block(std::byte* data, std::size_t size, std::size_t alignment, std::uint64_t ownerId,
          std::uint64_t generation) noexcept
        : data_ { data }
        , size_ { size }
        , alignment_ { alignment }
        , ownerId_ { ownerId }
        , generation_ { generation } {}

public:
    static Block make(std::byte* data, std::size_t size, std::size_t alignment,
                      std::uint64_t ownerId, std::uint64_t generation) noexcept {
        return Block { data, size, alignment, ownerId, generation };
    }

    void consume() noexcept {
        data_ = nullptr;
        size_ = 0;
        alignment_ = 1;
        ownerId_ = 0;
        generation_ = 0;
    }

public:
    Block() = delete;
    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;
    Block(Block&& other) noexcept { *this = std::move(other); }
    Block& operator=(Block&& other) noexcept {
        if (this != &other) {
            data_ = std::exchange(other.data_, nullptr);
            size_ = std::exchange(other.size_, 0);
            alignment_ = std::exchange(other.alignment_, 1);
            ownerId_ = std::exchange(other.ownerId_, 0);
            generation_ = std::exchange(other.generation_, 0);
        }
        return *this;
    }
    ~Block() = default;

    [[nodiscard]] explicit operator bool() const noexcept { return data_ != nullptr; }
    [[nodiscard]] bool empty() const noexcept { return data_ == nullptr; }
    [[nodiscard]] BlockView view() const noexcept { return { data_, size_, alignment_ }; }
    [[nodiscard]] std::byte* raw_data() const noexcept { return data_; }
    [[nodiscard]] std::size_t raw_size() const noexcept { return size_; }
    [[nodiscard]] std::size_t raw_alignment() const noexcept { return alignment_; }
    [[nodiscard]] std::uint64_t owner_id() const noexcept { return ownerId_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
};

template <class T>
class MaybeOwned {
private:
    std::optional<T> value_ {};

    explicit MaybeOwned(std::optional<T> value) : value_ { std::move(value) } {}

public:
    static MaybeOwned owned(T value) { return MaybeOwned { std::optional<T> { std::move(value) } }; }
    static MaybeOwned borrowed() { return MaybeOwned { std::nullopt }; }
    [[nodiscard]] bool is_owned() const noexcept { return value_.has_value(); }
    [[nodiscard]] const T& value() const& { return value_.value(); }
};

}
