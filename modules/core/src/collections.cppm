// collections.cppm — mbun.core.collections: allocation-conscious primitives.
//
// Re-expressed in MC++ from bun's real collection implementations (MIT):
//   - src/collections/{bit_set.rs,bit_set.zig,lib.rs}
//   - src/css/small_list.zig
//   - src/{semver/lib.rs,install_types/SemverString.zig}
//
// Design:
//   - StringPool copies each distinct byte string once into non-moving chunks;
//     StringId and returned views remain stable across growth and pool moves.
//   - StaticBitSet/DynamicBitSet keep padding bits zero and scan whole words
//     with popcount/countr_zero hot paths. Iterators allocate nothing.
//   - SmallVec<T,N> constructs the first N values in correctly aligned inline
//     storage, then spills with bun SmallList's 1.5x+8 growth policy.
module;

#include <cassert>

export module mbun.core.collections;

import std;

export namespace mbun::core::collections {

enum class CollectionError : std::uint8_t {
    capacity_overflow,
    out_of_memory,
    element_operation_failed,
};

// ---------------------------------------------------------------------------
// StringPool
// ---------------------------------------------------------------------------

struct StringId {
    std::uint32_t value{std::numeric_limits<std::uint32_t>::max()};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return value != std::numeric_limits<std::uint32_t>::max();
    }

    [[nodiscard]] constexpr bool operator==(const StringId& other) const noexcept {
        return value == other.value;
    }
};

class StringPool {
private:
    struct Entry {
        const char* data;
        std::size_t size;
    };

    struct Block {
        std::unique_ptr<char[]> data;
        std::size_t capacity;
        std::size_t used;
    };

    struct HashedString {
        std::size_t hash;
        std::string_view value;
    };

    struct PooledString {
        std::size_t hash;
        std::string_view value;
    };

    struct StringHash {
        using is_transparent = void;

        [[nodiscard]] static std::size_t hash_value(std::string_view value) noexcept {
            return std::hash<std::string_view>{}(value);
        }

        [[nodiscard]] std::size_t operator()(const HashedString& value) const noexcept {
            return value.hash;
        }

        [[nodiscard]] std::size_t operator()(const PooledString& value) const noexcept {
            return value.hash;
        }
    };

    struct StringEqual {
        using is_transparent = void;

        [[nodiscard]] bool operator()(const PooledString& left,
                                      const PooledString& right) const noexcept {
            return left.value == right.value;
        }

        [[nodiscard]] bool operator()(const PooledString& left,
                                      const HashedString& right) const noexcept {
            return left.value == right.value;
        }

        [[nodiscard]] bool operator()(const HashedString& left,
                                      const PooledString& right) const noexcept {
            return left.value == right.value;
        }
    };

    static constexpr std::size_t DEFAULT_BLOCK_SIZE{4096};
    static constexpr char EMPTY_STRING{'\0'};

    std::vector<Entry> entries_;
    std::vector<Block> blocks_;
    std::unordered_map<PooledString, StringId, StringHash, StringEqual> index_;
    std::size_t bytesSize_{0};

    [[nodiscard]] static constexpr std::size_t max_entries_() noexcept {
        return static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
    }

    [[nodiscard]] static HashedString hash_string_(std::string_view value) noexcept {
        return {StringHash::hash_value(value), value};
    }

    [[nodiscard]] auto find_hashed_(const HashedString& value) const noexcept {
        return index_.find(value);
    }
public:
    StringPool() = default;
    StringPool(const StringPool&) = delete;
    StringPool& operator=(const StringPool&) = delete;
    StringPool(StringPool&& other) noexcept {
        // Move the map before its backing blocks so the source never has live
        // string_view keys pointing at blocks it no longer owns.
        index_ = std::move(other.index_);
        entries_ = std::move(other.entries_);
        blocks_ = std::move(other.blocks_);
        bytesSize_ = std::exchange(other.bytesSize_, 0);
        other.index_.clear();
        other.entries_.clear();
        other.blocks_.clear();
    }

    StringPool& operator=(StringPool&& other) noexcept {
        if (this != &other) {
            index_ = std::move(other.index_);
            entries_ = std::move(other.entries_);
            blocks_ = std::move(other.blocks_);
            bytesSize_ = std::exchange(other.bytesSize_, 0);
            other.index_.clear();
            other.entries_.clear();
            other.blocks_.clear();
        }
        return *this;
    }
    ~StringPool() = default;

    [[nodiscard]] std::size_t size() const noexcept {
        return entries_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return entries_.empty();
    }

    [[nodiscard]] std::size_t bytes_size() const noexcept {
        return bytesSize_;
    }

    [[nodiscard]] std::optional<StringId> find(std::string_view value) const noexcept {
        const auto iterator{find_hashed_(hash_string_(value))};
        if (iterator == index_.end()) {
            return std::nullopt;
        }
        return iterator->second;
    }

    [[nodiscard]] bool contains(std::string_view value) const noexcept {
        return find_hashed_(hash_string_(value)) != index_.end();
    }

    [[nodiscard]] std::string_view get(StringId id) const noexcept {
        assert(id.valid() && static_cast<std::size_t>(id.value) < entries_.size());
        const Entry& entry{entries_[id.value]};
        return {entry.data, entry.size};
    }

    [[nodiscard]] std::expected<void, CollectionError> reserve(std::size_t count) noexcept {
        if (count > max_entries_()) {
            return std::unexpected{CollectionError::capacity_overflow};
        }
        try {
            entries_.reserve(count);
            index_.reserve(count);
        } catch (const std::length_error&) {
            return std::unexpected{CollectionError::capacity_overflow};
        } catch (const std::bad_alloc&) {
            return std::unexpected{CollectionError::out_of_memory};
        }
        return {};
    }

    [[nodiscard]] std::expected<StringId, CollectionError> intern(std::string_view value) noexcept {
        // Compute the byte hash once. The stored key carries this precomputed
        // value, so insertion and later rehashes do not rescan pooled bytes
        // (same optimization intent as bun commit 2e97ce744).
        const HashedString hashed{hash_string_(value)};
        if (const auto existing{find_hashed_(hashed)}; existing != index_.end()) {
            return existing->second;
        }
        if (entries_.size() >= max_entries_()) {
            return std::unexpected{CollectionError::capacity_overflow};
        }
        if (value.size() > std::numeric_limits<std::size_t>::max() - bytesSize_) {
            return std::unexpected{CollectionError::capacity_overflow};
        }

        const StringId id{static_cast<std::uint32_t>(entries_.size())};
        bool addedBlock{false};
        std::size_t previousUsed{0};
        try {
            // Reserve the flat metadata before obtaining storage so failures
            // leave the pool unchanged.
            entries_.reserve(entries_.size() + 1);
            index_.reserve(index_.size() + 1);

            const char* stored{&EMPTY_STRING};
            if (!value.empty()) {
                if (blocks_.empty() ||
                    blocks_.back().capacity - blocks_.back().used < value.size()) {
                    const std::size_t capacity{std::max(DEFAULT_BLOCK_SIZE, value.size())};
                    auto data{std::make_unique_for_overwrite<char[]>(capacity)};
                    blocks_.push_back(Block{std::move(data), capacity, 0});
                    addedBlock = true;
                }
                Block& block{blocks_.back()};
                previousUsed = block.used;
                char* destination{block.data.get() + block.used};
                std::memcpy(destination, value.data(), value.size());
                block.used += value.size();
                stored = destination;
            }

            const std::string_view stableView{stored, value.size()};
            entries_.push_back(Entry{stored, value.size()});
            try {
                index_.emplace(PooledString{hashed.hash, stableView}, id);
            } catch (...) {
                entries_.pop_back();
                if (!value.empty()) {
                    blocks_.back().used = previousUsed;
                    if (addedBlock) {
                        blocks_.pop_back();
                    }
                }
                throw;
            }
            bytesSize_ += value.size();
            return id;
        } catch (const std::length_error&) {
            return std::unexpected{CollectionError::capacity_overflow};
        } catch (const std::bad_alloc&) {
            return std::unexpected{CollectionError::out_of_memory};
        }
    }

    void clear() noexcept {
        index_.clear();
        entries_.clear();
        blocks_.clear();
        bytesSize_ = 0;
    }
};

// ---------------------------------------------------------------------------
// Bit sets
// ---------------------------------------------------------------------------

namespace detail {

using BitWord = std::uint64_t;
constexpr std::size_t WORD_BITS{std::numeric_limits<BitWord>::digits};

[[nodiscard]] constexpr std::size_t word_count(std::size_t bitLength) noexcept {
    return bitLength / WORD_BITS + static_cast<std::size_t>((bitLength % WORD_BITS) != 0);
}

[[nodiscard]] constexpr BitWord last_word_mask(std::size_t bitLength) noexcept {
    const std::size_t remainder{bitLength % WORD_BITS};
    return remainder == 0 ? std::numeric_limits<BitWord>::max()
                          : (BitWord{1} << remainder) - BitWord{1};
}

constexpr void clear_padding(std::span<BitWord> words, std::size_t bitLength) noexcept {
    if (!words.empty()) {
        words.back() &= last_word_mask(bitLength);
    }
}

inline void set_range(std::span<BitWord> words, std::size_t bitLength, std::size_t start,
                      std::size_t end, bool value) noexcept {
    assert(start <= end && end <= bitLength);
    if (start == end) {
        return;
    }

    const std::size_t firstWord{start / WORD_BITS};
    const std::size_t lastWord{(end - 1) / WORD_BITS};
    const std::size_t firstBit{start % WORD_BITS};
    const std::size_t endBit{end % WORD_BITS};
    const BitWord lowMask{std::numeric_limits<BitWord>::max() << firstBit};
    const BitWord highMask{endBit == 0 ? std::numeric_limits<BitWord>::max()
                                       : (BitWord{1} << endBit) - BitWord{1}};

    if (firstWord == lastWord) {
        const BitWord mask{lowMask & highMask};
        words[firstWord] = value ? (words[firstWord] | mask) : (words[firstWord] & ~mask);
        return;
    }

    words[firstWord] = value ? (words[firstWord] | lowMask) : (words[firstWord] & ~lowMask);
    for (std::size_t word{firstWord + 1}; word < lastWord; ++word) {
        words[word] = value ? std::numeric_limits<BitWord>::max() : BitWord{0};
    }
    words[lastWord] = value ? (words[lastWord] | highMask) : (words[lastWord] & ~highMask);
    clear_padding(words, bitLength);
}

template <bool SetBits, bool Forward>
class BitIterator {
private:
    std::span<const BitWord> words_;
    std::size_t bitLength_{0};
    std::size_t wordIndex_{0};
    BitWord bits_{0};

    [[nodiscard]] BitWord load_word_(std::size_t index) const noexcept {
        BitWord word{words_[index]};
        if constexpr (!SetBits) {
            word = ~word;
        }
        if (index + 1 == words_.size()) {
            word &= last_word_mask(bitLength_);
        }
        return word;
    }
public:
    BitIterator(std::span<const BitWord> words, std::size_t bitLength) noexcept
        : words_{words}, bitLength_{bitLength} {
        if (!words_.empty()) {
            wordIndex_ = Forward ? 0 : words_.size() - 1;
            bits_ = load_word_(wordIndex_);
        }
    }

    [[nodiscard]] std::optional<std::size_t> next() noexcept {
        while (bits_ == 0) {
            if (words_.empty()) {
                return std::nullopt;
            }
            if constexpr (Forward) {
                if (wordIndex_ + 1 >= words_.size()) {
                    return std::nullopt;
                }
                ++wordIndex_;
            } else {
                if (wordIndex_ == 0) {
                    return std::nullopt;
                }
                --wordIndex_;
            }
            bits_ = load_word_(wordIndex_);
        }

        if constexpr (Forward) {
            const std::size_t bit{static_cast<std::size_t>(std::countr_zero(bits_))};
            bits_ &= bits_ - 1;
            return wordIndex_ * WORD_BITS + bit;
        } else {
            const std::size_t bit{WORD_BITS - 1 -
                                  static_cast<std::size_t>(std::countl_zero(bits_))};
            bits_ &= bit == 0 ? BitWord{0} : (BitWord{1} << bit) - BitWord{1};
            return wordIndex_ * WORD_BITS + bit;
        }
    }
};

inline std::size_t count_words(std::span<const BitWord> words) noexcept {
    std::size_t count{0};
    for (BitWord word : words) {
        count += static_cast<std::size_t>(std::popcount(word));
    }
    return count;
}

inline std::optional<std::size_t> find_first_set(std::span<const BitWord> words) noexcept {
    for (std::size_t index{0}; index < words.size(); ++index) {
        if (words[index] != 0) {
            return index * WORD_BITS + static_cast<std::size_t>(std::countr_zero(words[index]));
        }
    }
    return std::nullopt;
}

}  // namespace detail

template <std::size_t Size>
class StaticBitSet {
private:
    static constexpr std::size_t WORD_COUNT{detail::word_count(Size)};
    std::array<detail::BitWord, WORD_COUNT> words_{};
public:
    static constexpr std::size_t BIT_LENGTH{Size};

    constexpr StaticBitSet() noexcept = default;

    [[nodiscard]] static constexpr StaticBitSet init_empty() noexcept {
        return {};
    }

    [[nodiscard]] static constexpr StaticBitSet init_full() noexcept {
        StaticBitSet result;
        result.words_.fill(std::numeric_limits<detail::BitWord>::max());
        detail::clear_padding(result.words_, Size);
        return result;
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return Size;
    }

    [[nodiscard]] bool is_set(std::size_t index) const noexcept {
        assert(index < Size);
        return (words_[index / detail::WORD_BITS] &
                (detail::BitWord{1} << (index % detail::WORD_BITS))) != 0;
    }

    [[nodiscard]] bool is_set_allow_out_of_bound(std::size_t index,
                                                 bool outOfBounds) const noexcept {
        return index >= Size ? outOfBounds : is_set(index);
    }

    [[nodiscard]] std::size_t count() const noexcept {
        return detail::count_words(words_);
    }

    void set_value(std::size_t index, bool value) noexcept {
        assert(index < Size);
        const detail::BitWord bit{detail::BitWord{1} << (index % detail::WORD_BITS)};
        detail::BitWord& word{words_[index / detail::WORD_BITS]};
        word = value ? (word | bit) : (word & ~bit);
    }

    void set(std::size_t index) noexcept {
        set_value(index, true);
    }

    void unset(std::size_t index) noexcept {
        set_value(index, false);
    }

    void toggle(std::size_t index) noexcept {
        assert(index < Size);
        words_[index / detail::WORD_BITS] ^= detail::BitWord{1} << (index % detail::WORD_BITS);
    }

    void set_range_value(std::size_t start, std::size_t end, bool value) noexcept {
        detail::set_range(words_, Size, start, end, value);
    }

    void set_all(bool value) noexcept {
        words_.fill(value ? std::numeric_limits<detail::BitWord>::max() : detail::BitWord{0});
        detail::clear_padding(words_, Size);
    }

    void clear() noexcept {
        set_all(false);
    }

    void toggle_all() noexcept {
        for (detail::BitWord& word : words_) {
            word = ~word;
        }
        detail::clear_padding(words_, Size);
    }

    void toggle_set(const StaticBitSet& other) noexcept {
        for (std::size_t index{0}; index < WORD_COUNT; ++index) {
            words_[index] ^= other.words_[index];
        }
        detail::clear_padding(words_, Size);
    }

    void set_union(const StaticBitSet& other) noexcept {
        for (std::size_t index{0}; index < WORD_COUNT; ++index) {
            words_[index] |= other.words_[index];
        }
    }

    void set_intersection(const StaticBitSet& other) noexcept {
        for (std::size_t index{0}; index < WORD_COUNT; ++index) {
            words_[index] &= other.words_[index];
        }
    }

    void set_exclude(const StaticBitSet& other) noexcept {
        for (std::size_t index{0}; index < WORD_COUNT; ++index) {
            words_[index] &= ~other.words_[index];
        }
        detail::clear_padding(words_, Size);
    }

    void set_exclude_two(const StaticBitSet& other, const StaticBitSet& third) noexcept {
        for (std::size_t index{0}; index < WORD_COUNT; ++index) {
            words_[index] &= ~other.words_[index] & ~third.words_[index];
        }
        detail::clear_padding(words_, Size);
    }

    [[nodiscard]] bool has_intersection(const StaticBitSet& other) const noexcept {
        for (std::size_t index{0}; index < WORD_COUNT; ++index) {
            if ((words_[index] & other.words_[index]) != 0) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::optional<std::size_t> find_first_set() const noexcept {
        return detail::find_first_set(words_);
    }

    [[nodiscard]] std::optional<std::size_t> find_first_unset() const noexcept {
        return detail::BitIterator<false, true>{words_, Size}.next();
    }

    [[nodiscard]] std::optional<std::size_t> toggle_first_set() noexcept {
        for (std::size_t index{0}; index < WORD_COUNT; ++index) {
            detail::BitWord& word{words_[index]};
            if (word != 0) {
                const std::size_t bit{static_cast<std::size_t>(std::countr_zero(word))};
                word &= word - 1;
                return index * detail::WORD_BITS + bit;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] bool eql(const StaticBitSet& other) const noexcept {
        return words_ == other.words_;
    }

    [[nodiscard]] bool subset_of(const StaticBitSet& other) const noexcept {
        for (std::size_t index{0}; index < WORD_COUNT; ++index) {
            if ((words_[index] & other.words_[index]) != words_[index]) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool superset_of(const StaticBitSet& other) const noexcept {
        return other.subset_of(*this);
    }

    [[nodiscard]] StaticBitSet complement() const noexcept {
        StaticBitSet result{*this};
        result.toggle_all();
        return result;
    }

    [[nodiscard]] StaticBitSet union_with(const StaticBitSet& other) const noexcept {
        StaticBitSet result{*this};
        result.set_union(other);
        return result;
    }

    [[nodiscard]] StaticBitSet intersect_with(const StaticBitSet& other) const noexcept {
        StaticBitSet result{*this};
        result.set_intersection(other);
        return result;
    }

    [[nodiscard]] StaticBitSet xor_with(const StaticBitSet& other) const noexcept {
        StaticBitSet result{*this};
        result.toggle_set(other);
        return result;
    }

    [[nodiscard]] StaticBitSet difference_with(const StaticBitSet& other) const noexcept {
        StaticBitSet result{*this};
        result.set_exclude(other);
        return result;
    }

    template <bool SetBits = true, bool Forward = true>
    [[nodiscard]] auto iterator() const noexcept {
        return detail::BitIterator<SetBits, Forward>{words_, Size};
    }
};

class DynamicBitSet {
private:
    std::size_t bitLength_{0};
    std::vector<detail::BitWord> words_;

    void assert_same_size_(const DynamicBitSet& other) const noexcept {
        assert(bitLength_ == other.bitLength_);
    }
public:
    DynamicBitSet() = default;

    explicit DynamicBitSet(std::size_t bitLength, bool fill = false)
        : bitLength_{bitLength},
          words_(detail::word_count(bitLength),
                 fill ? std::numeric_limits<detail::BitWord>::max() : detail::BitWord{0}) {
        detail::clear_padding(words_, bitLength_);
    }

    DynamicBitSet(const DynamicBitSet&) = default;
    DynamicBitSet& operator=(const DynamicBitSet&) = default;

    DynamicBitSet(DynamicBitSet&& other) noexcept
        : bitLength_{std::exchange(other.bitLength_, 0)}, words_{std::move(other.words_)} {}

    DynamicBitSet& operator=(DynamicBitSet&& other) noexcept {
        if (this != &other) {
            bitLength_ = std::exchange(other.bitLength_, 0);
            words_ = std::move(other.words_);
        }
        return *this;
    }

    ~DynamicBitSet() = default;

    [[nodiscard]] std::size_t capacity() const noexcept {
        return bitLength_;
    }

    [[nodiscard]] std::size_t bit_length() const noexcept {
        return bitLength_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return bitLength_ == 0;
    }

    [[nodiscard]] bool is_set(std::size_t index) const noexcept {
        assert(index < bitLength_);
        return (words_[index / detail::WORD_BITS] &
                (detail::BitWord{1} << (index % detail::WORD_BITS))) != 0;
    }

    [[nodiscard]] bool is_set_allow_out_of_bound(std::size_t index,
                                                 bool outOfBounds) const noexcept {
        return index >= bitLength_ ? outOfBounds : is_set(index);
    }

    [[nodiscard]] std::size_t count() const noexcept {
        return detail::count_words(words_);
    }

    void set_value(std::size_t index, bool value) noexcept {
        assert(index < bitLength_);
        const detail::BitWord bit{detail::BitWord{1} << (index % detail::WORD_BITS)};
        detail::BitWord& word{words_[index / detail::WORD_BITS]};
        word = value ? (word | bit) : (word & ~bit);
    }

    void set(std::size_t index) noexcept {
        set_value(index, true);
    }

    void unset(std::size_t index) noexcept {
        set_value(index, false);
    }

    void toggle(std::size_t index) noexcept {
        assert(index < bitLength_);
        words_[index / detail::WORD_BITS] ^= detail::BitWord{1} << (index % detail::WORD_BITS);
    }

    void set_range_value(std::size_t start, std::size_t end, bool value) noexcept {
        detail::set_range(words_, bitLength_, start, end, value);
    }

    void set_all(bool value) noexcept {
        std::ranges::fill(words_,
                          value ? std::numeric_limits<detail::BitWord>::max() : detail::BitWord{0});
        detail::clear_padding(words_, bitLength_);
    }

    void clear() noexcept {
        set_all(false);
    }

    void toggle_all() noexcept {
        for (detail::BitWord& word : words_) {
            word = ~word;
        }
        detail::clear_padding(words_, bitLength_);
    }

    void resize(std::size_t newLength, bool fill) {
        const std::size_t oldLength{bitLength_};
        words_.resize(detail::word_count(newLength),
                      fill ? std::numeric_limits<detail::BitWord>::max() : detail::BitWord{0});
        bitLength_ = newLength;
        if (newLength > oldLength) {
            detail::set_range(words_, bitLength_, oldLength, newLength, fill);
        }
        detail::clear_padding(words_, bitLength_);
    }

    [[nodiscard]] std::expected<void, CollectionError> try_resize(std::size_t newLength,
                                                                  bool fill) noexcept {
        if (detail::word_count(newLength) > words_.max_size()) {
            return std::unexpected{CollectionError::capacity_overflow};
        }
        try {
            resize(newLength, fill);
        } catch (const std::length_error&) {
            return std::unexpected{CollectionError::capacity_overflow};
        } catch (const std::bad_alloc&) {
            return std::unexpected{CollectionError::out_of_memory};
        }
        return {};
    }

    void toggle_set(const DynamicBitSet& other) noexcept {
        assert_same_size_(other);
        for (std::size_t index{0}; index < words_.size(); ++index) {
            words_[index] ^= other.words_[index];
        }
        detail::clear_padding(words_, bitLength_);
    }

    void set_union(const DynamicBitSet& other) noexcept {
        assert_same_size_(other);
        for (std::size_t index{0}; index < words_.size(); ++index) {
            words_[index] |= other.words_[index];
        }
    }

    void set_intersection(const DynamicBitSet& other) noexcept {
        assert_same_size_(other);
        for (std::size_t index{0}; index < words_.size(); ++index) {
            words_[index] &= other.words_[index];
        }
    }

    void set_exclude(const DynamicBitSet& other) noexcept {
        assert_same_size_(other);
        for (std::size_t index{0}; index < words_.size(); ++index) {
            words_[index] &= ~other.words_[index];
        }
        detail::clear_padding(words_, bitLength_);
    }

    void set_exclude_two(const DynamicBitSet& other, const DynamicBitSet& third) noexcept {
        assert_same_size_(other);
        assert_same_size_(third);
        for (std::size_t index{0}; index < words_.size(); ++index) {
            words_[index] &= ~other.words_[index] & ~third.words_[index];
        }
        detail::clear_padding(words_, bitLength_);
    }

    [[nodiscard]] bool has_intersection(const DynamicBitSet& other) const noexcept {
        assert_same_size_(other);
        for (std::size_t index{0}; index < words_.size(); ++index) {
            if ((words_[index] & other.words_[index]) != 0) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::optional<std::size_t> find_first_set() const noexcept {
        return detail::find_first_set(words_);
    }

    [[nodiscard]] std::optional<std::size_t> find_first_unset() const noexcept {
        return detail::BitIterator<false, true>{words_, bitLength_}.next();
    }

    [[nodiscard]] std::optional<std::size_t> toggle_first_set() noexcept {
        for (std::size_t index{0}; index < words_.size(); ++index) {
            detail::BitWord& word{words_[index]};
            if (word != 0) {
                const std::size_t bit{static_cast<std::size_t>(std::countr_zero(word))};
                word &= word - 1;
                return index * detail::WORD_BITS + bit;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] bool eql(const DynamicBitSet& other) const noexcept {
        return bitLength_ == other.bitLength_ && words_ == other.words_;
    }

    [[nodiscard]] bool subset_of(const DynamicBitSet& other) const noexcept {
        if (bitLength_ != other.bitLength_) {
            return false;
        }
        for (std::size_t index{0}; index < words_.size(); ++index) {
            if ((words_[index] & other.words_[index]) != words_[index]) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool superset_of(const DynamicBitSet& other) const noexcept {
        return other.subset_of(*this);
    }

    [[nodiscard]] DynamicBitSet complement() const {
        DynamicBitSet result{*this};
        result.toggle_all();
        return result;
    }

    [[nodiscard]] DynamicBitSet union_with(const DynamicBitSet& other) const {
        DynamicBitSet result{*this};
        result.set_union(other);
        return result;
    }

    [[nodiscard]] DynamicBitSet intersect_with(const DynamicBitSet& other) const {
        DynamicBitSet result{*this};
        result.set_intersection(other);
        return result;
    }

    [[nodiscard]] DynamicBitSet xor_with(const DynamicBitSet& other) const {
        DynamicBitSet result{*this};
        result.toggle_set(other);
        return result;
    }

    [[nodiscard]] DynamicBitSet difference_with(const DynamicBitSet& other) const {
        DynamicBitSet result{*this};
        result.set_exclude(other);
        return result;
    }

    template <bool SetBits = true, bool Forward = true>
    [[nodiscard]] auto iterator() const noexcept {
        return detail::BitIterator<SetBits, Forward>{words_, bitLength_};
    }
};

// Runtime-sized bitset with bun's load-bearing 127-bit inline threshold.
// `bitLength_` plus a 16-byte union keeps the type at three machine words on
// 64-bit targets, matching rewrite commit 04d93937d's AutoBitSet footprint.
class AutoBitSet {
private:
    static constexpr std::size_t INLINE_BITS{127};
    static constexpr std::size_t INLINE_WORDS{detail::word_count(INLINE_BITS)};

    union Storage {
        std::array<detail::BitWord, INLINE_WORDS> inlineWords;
        detail::BitWord* dynamicWords;

        constexpr Storage() noexcept : inlineWords{} {}
        ~Storage() {}
    };

    // The inline arm is always the complete StaticBitSet<127>. The requested
    // length only selects inline vs dynamic storage, exactly like bun's
    // AutoBitSet::init_empty forwarding to AutoBitSetStatic::init_empty().
    std::size_t bitLength_{INLINE_BITS};
    Storage storage_{};

    [[nodiscard]] bool is_dynamic_() const noexcept {
        return bitLength_ > INLINE_BITS;
    }

    void activate_inline_() noexcept {
        std::construct_at(std::addressof(storage_.inlineWords));
    }

    [[nodiscard]] std::span<detail::BitWord> mutable_words_() noexcept {
        const std::size_t count{detail::word_count(bitLength_)};
        if (is_dynamic_()) {
            return {storage_.dynamicWords, count};
        }
        return {storage_.inlineWords.data(), count};
    }

    [[nodiscard]] std::span<const detail::BitWord> words_() const noexcept {
        const std::size_t count{detail::word_count(bitLength_)};
        if (is_dynamic_()) {
            return {storage_.dynamicWords, count};
        }
        return {storage_.inlineWords.data(), count};
    }

    void release_() noexcept {
        if (is_dynamic_()) {
            std::allocator<detail::BitWord> allocator;
            allocator.deallocate(storage_.dynamicWords, detail::word_count(bitLength_));
            activate_inline_();
        }
        storage_.inlineWords.fill(0);
        bitLength_ = INLINE_BITS;
    }

public:
    AutoBitSet() noexcept = default;

    explicit AutoBitSet(std::size_t bitLength)
        : bitLength_{needs_dynamic(bitLength) ? bitLength : INLINE_BITS} {
        if (is_dynamic_()) {
            const std::size_t count{detail::word_count(bitLength_)};
            std::allocator<detail::BitWord> allocator;
            detail::BitWord* words{allocator.allocate(count)};
            storage_.dynamicWords = words;
            std::fill_n(words, count, detail::BitWord{0});
        } else {
            storage_.inlineWords.fill(0);
        }
    }

    AutoBitSet(const AutoBitSet& other) : AutoBitSet{other.bitLength_} {
        std::ranges::copy(other.words_(), mutable_words_().begin());
    }

    AutoBitSet& operator=(const AutoBitSet& other) {
        if (this != &other) {
            AutoBitSet copy{other};
            *this = std::move(copy);
        }
        return *this;
    }

    AutoBitSet(AutoBitSet&& other) noexcept : bitLength_{other.bitLength_} {
        if (other.is_dynamic_()) {
            storage_.dynamicWords = other.storage_.dynamicWords;
            other.activate_inline_();
            other.storage_.inlineWords.fill(0);
        } else {
            storage_.inlineWords = other.storage_.inlineWords;
            other.storage_.inlineWords = {};
        }
        other.bitLength_ = INLINE_BITS;
    }

    AutoBitSet& operator=(AutoBitSet&& other) noexcept {
        if (this != &other) {
            release_();
            bitLength_ = other.bitLength_;
            if (other.is_dynamic_()) {
                storage_.dynamicWords = other.storage_.dynamicWords;
                other.activate_inline_();
                other.storage_.inlineWords.fill(0);
            } else {
                storage_.inlineWords = other.storage_.inlineWords;
                other.storage_.inlineWords = {};
            }
            other.bitLength_ = INLINE_BITS;
        }
        return *this;
    }

    ~AutoBitSet() {
        release_();
    }

    [[nodiscard]] static constexpr bool needs_dynamic(std::size_t bitLength) noexcept {
        return bitLength > INLINE_BITS;
    }

    [[nodiscard]] bool is_set(std::size_t index) const noexcept {
        assert(index < bitLength_);
        return (words_()[index / detail::WORD_BITS] &
                (detail::BitWord{1} << (index % detail::WORD_BITS))) != 0;
    }

    [[nodiscard]] std::size_t count() const noexcept {
        return detail::count_words(words_());
    }

    void set(std::size_t index) noexcept {
        assert(index < bitLength_);
        const detail::BitWord bit{detail::BitWord{1} << (index % detail::WORD_BITS)};
        mutable_words_()[index / detail::WORD_BITS] |= bit;
    }

    void unset(std::size_t index) noexcept {
        assert(index < bitLength_);
        const detail::BitWord bit{detail::BitWord{1} << (index % detail::WORD_BITS)};
        mutable_words_()[index / detail::WORD_BITS] &= ~bit;
    }

    void set_all(bool value) noexcept {
        std::ranges::fill(mutable_words_(),
                          value ? std::numeric_limits<detail::BitWord>::max() : detail::BitWord{0});
        detail::clear_padding(mutable_words_(), bitLength_);
    }

    [[nodiscard]] bool has_intersection(const AutoBitSet& other) const noexcept {
        if (is_dynamic_() != other.is_dynamic_()) {
            return false;
        }
        const auto words{words_()};
        const auto otherWords{other.words_()};
        // Bun's dynamic implementation iterates zip. Preserve that safe
        // common-prefix behavior when release callers provide unequal sizes.
        const std::size_t commonWords{std::min(words.size(), otherWords.size())};
        for (std::size_t index{0}; index < commonWords; ++index) {
            if ((words[index] & otherWords[index]) != 0) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::optional<std::size_t> find_first_set() const noexcept {
        return detail::find_first_set(words_());
    }

    [[nodiscard]] bool eql(const AutoBitSet& other) const noexcept {
        // bun compares raw_bytes() here. This deliberately ignores the exact
        // bit length when both representations occupy the same number of
        // words (including static-127 vs dynamic-128).
        return std::ranges::equal(words_(), other.words_());
    }

    template <bool SetBits = true, bool Forward = true>
    [[nodiscard]] auto iterator() const noexcept {
        return detail::BitIterator<SetBits, Forward>{words_(), bitLength_};
    }
};

static_assert(sizeof(void*) != 8 || sizeof(AutoBitSet) == 3 * sizeof(std::size_t));

// ---------------------------------------------------------------------------
// SmallVec
// ---------------------------------------------------------------------------

template <class T, std::size_t InlineCapacity>
class SmallVec {
private:
    using Allocator = std::allocator<T>;
    using Traits = std::allocator_traits<Allocator>;
    static constexpr std::size_t INLINE_BYTES{sizeof(T) *
                                              (InlineCapacity == 0 ? 1 : InlineCapacity)};

    std::size_t size_{0};
    std::size_t capacity_{InlineCapacity};
    T* heap_{nullptr};
    [[no_unique_address]] Allocator allocator_{};
    alignas(T) std::array<std::byte, INLINE_BYTES> inlineStorage_{};

    [[nodiscard]] T* inline_data_() noexcept {
        return std::launder(reinterpret_cast<T*>(inlineStorage_.data()));
    }

    [[nodiscard]] const T* inline_data_() const noexcept {
        return std::launder(reinterpret_cast<const T*>(inlineStorage_.data()));
    }

    void destroy_elements_() noexcept {
        for (std::size_t index{size_}; index > 0; --index) {
            Traits::destroy(allocator_, data() + (index - 1));
        }
        size_ = 0;
    }

    void release_heap_() noexcept {
        if (heap_ != nullptr) {
            Traits::deallocate(allocator_, heap_, capacity_);
            heap_ = nullptr;
            capacity_ = InlineCapacity;
        }
    }

    [[nodiscard]] static constexpr std::size_t max_size_() noexcept {
        return static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) / sizeof(T);
    }

    [[nodiscard]] std::size_t growth_capacity_(std::size_t minimum) const noexcept {
        std::size_t candidate{capacity_};
        while (candidate < minimum) {
            const std::size_t increment{candidate / 2 + 8};
            if (candidate > max_size_() - increment) {
                return minimum;
            }
            candidate += increment;
        }
        return candidate;
    }

    void reallocate_(std::size_t newCapacity) {
        T* newData{Traits::allocate(allocator_, newCapacity)};
        std::size_t constructed{0};
        try {
            for (; constructed < size_; ++constructed) {
                Traits::construct(allocator_, newData + constructed,
                                  std::move_if_noexcept(data()[constructed]));
            }
        } catch (...) {
            for (std::size_t index{constructed}; index > 0; --index) {
                Traits::destroy(allocator_, newData + (index - 1));
            }
            Traits::deallocate(allocator_, newData, newCapacity);
            throw;
        }

        T* oldData{data()};
        const bool oldWasHeap{heap_ != nullptr};
        const std::size_t oldCapacity{capacity_};
        for (std::size_t index{size_}; index > 0; --index) {
            Traits::destroy(allocator_, oldData + (index - 1));
        }
        if (oldWasHeap) {
            Traits::deallocate(allocator_, oldData, oldCapacity);
        }
        heap_ = newData;
        capacity_ = newCapacity;
    }

    template <class... Args>
    T& reallocate_emplace_(std::size_t newCapacity, Args&&... args) {
        T* newData{Traits::allocate(allocator_, newCapacity)};
        const std::size_t oldSize{size_};
        bool appended{false};
        std::size_t relocated{0};
        try {
            // Construct the appended element while references into the old
            // buffer are still valid. This is load-bearing for
            // push_back(v[i]) / emplace_back(v[i]) when growth spills.
            Traits::construct(allocator_, newData + oldSize, std::forward<Args>(args)...);
            appended = true;
            for (; relocated < oldSize; ++relocated) {
                Traits::construct(allocator_, newData + relocated,
                                  std::move_if_noexcept(data()[relocated]));
            }
        } catch (...) {
            for (std::size_t index{relocated}; index > 0; --index) {
                Traits::destroy(allocator_, newData + (index - 1));
            }
            if (appended) {
                Traits::destroy(allocator_, newData + oldSize);
            }
            Traits::deallocate(allocator_, newData, newCapacity);
            throw;
        }

        T* oldData{data()};
        const bool oldWasHeap{heap_ != nullptr};
        const std::size_t oldCapacity{capacity_};
        for (std::size_t index{oldSize}; index > 0; --index) {
            Traits::destroy(allocator_, oldData + (index - 1));
        }
        if (oldWasHeap) {
            Traits::deallocate(allocator_, oldData, oldCapacity);
        }
        heap_ = newData;
        capacity_ = newCapacity;
        size_ = oldSize + 1;
        return newData[oldSize];
    }

    void move_from_(SmallVec&& other) {
        if (!other.is_inline()) {
            heap_ = std::exchange(other.heap_, nullptr);
            size_ = std::exchange(other.size_, 0);
            capacity_ = std::exchange(other.capacity_, InlineCapacity);
            return;
        }

        std::size_t constructed{0};
        try {
            for (; constructed < other.size_; ++constructed) {
                Traits::construct(allocator_, inline_data_() + constructed,
                                  std::move(other[constructed]));
            }
        } catch (...) {
            for (std::size_t index{constructed}; index > 0; --index) {
                Traits::destroy(allocator_, inline_data_() + (index - 1));
            }
            throw;
        }
        size_ = other.size_;
        other.clear();
    }
public:
    using value_type = T;
    using size_type = std::size_t;
    using iterator = T*;
    using const_iterator = const T*;

    SmallVec() noexcept = default;

    SmallVec(const SmallVec& other)
    requires(std::is_copy_constructible_v<T>)
    {
        reserve(other.size_);
        std::size_t constructed{0};
        try {
            for (; constructed < other.size_; ++constructed) {
                Traits::construct(allocator_, data() + constructed, other[constructed]);
            }
        } catch (...) {
            size_ = constructed;
            destroy_elements_();
            release_heap_();
            throw;
        }
        size_ = other.size_;
    }

    SmallVec(SmallVec&& other) noexcept(std::is_nothrow_move_constructible_v<T>)
    requires(std::is_move_constructible_v<T>)
    {
        move_from_(std::move(other));
    }

    SmallVec& operator=(const SmallVec& other)
    requires(std::is_copy_constructible_v<T> && std::is_copy_assignable_v<T>)
    {
        if (this == &other) {
            return *this;
        }

        if (other.size_ > capacity_) {
            reserve(other.size_);
        }

        const std::size_t common{std::min(size_, other.size_)};
        for (std::size_t index{0}; index < common; ++index) {
            data()[index] = other[index];
        }
        while (size_ > other.size_) {
            pop_back();
        }
        while (size_ < other.size_) {
            Traits::construct(allocator_, data() + size_, other[size_]);
            ++size_;
        }
        return *this;
    }

    SmallVec& operator=(SmallVec&& other) noexcept(std::is_nothrow_move_constructible_v<T>)
    requires(std::is_move_constructible_v<T>)
    {
        if (this != &other) {
            destroy_elements_();
            release_heap_();
            move_from_(std::move(other));
        }
        return *this;
    }

    ~SmallVec() {
        destroy_elements_();
        release_heap_();
    }

    [[nodiscard]] T* data() noexcept {
        return heap_ != nullptr ? heap_ : inline_data_();
    }

    [[nodiscard]] const T* data() const noexcept {
        return heap_ != nullptr ? heap_ : inline_data_();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0;
    }

    [[nodiscard]] bool is_inline() const noexcept {
        return heap_ == nullptr;
    }

    [[nodiscard]] static constexpr std::size_t inline_capacity() noexcept {
        return InlineCapacity;
    }

    [[nodiscard]] static constexpr std::size_t max_size() noexcept {
        return max_size_();
    }

    [[nodiscard]] T& operator[](std::size_t index) noexcept {
        assert(index < size_);
        return data()[index];
    }

    [[nodiscard]] const T& operator[](std::size_t index) const noexcept {
        assert(index < size_);
        return data()[index];
    }

    [[nodiscard]] T& front() noexcept {
        assert(size_ > 0);
        return data()[0];
    }

    [[nodiscard]] const T& front() const noexcept {
        assert(size_ > 0);
        return data()[0];
    }

    [[nodiscard]] T& back() noexcept {
        assert(size_ > 0);
        return data()[size_ - 1];
    }

    [[nodiscard]] const T& back() const noexcept {
        assert(size_ > 0);
        return data()[size_ - 1];
    }

    [[nodiscard]] iterator begin() noexcept {
        return data();
    }

    [[nodiscard]] const_iterator begin() const noexcept {
        return data();
    }

    [[nodiscard]] const_iterator cbegin() const noexcept {
        return data();
    }

    [[nodiscard]] iterator end() noexcept {
        return data() + size_;
    }

    [[nodiscard]] const_iterator end() const noexcept {
        return data() + size_;
    }

    [[nodiscard]] const_iterator cend() const noexcept {
        return data() + size_;
    }

    [[nodiscard]] std::span<T> span() noexcept {
        return {data(), size_};
    }

    [[nodiscard]] std::span<const T> span() const noexcept {
        return {data(), size_};
    }

    /// Grow without propagating allocation/element-relocation exceptions.
    ///
    /// `move_if_noexcept` copies when `T` is copyable and moving may throw; a
    /// failed copy therefore leaves the original elements unchanged (strong
    /// guarantee). For move-only `T` with a throwing move constructor, already
    /// moved source elements may remain moved-from after failure; size,
    /// capacity, ownership and destruction stay valid (basic guarantee).
    [[nodiscard]] std::expected<void, CollectionError>
    try_reserve(std::size_t newCapacity) noexcept {
        if (newCapacity <= capacity_) {
            return {};
        }
        if (newCapacity > max_size_()) {
            return std::unexpected{CollectionError::capacity_overflow};
        }
        try {
            reallocate_(newCapacity);
        } catch (const std::length_error&) {
            return std::unexpected{CollectionError::capacity_overflow};
        } catch (const std::bad_alloc&) {
            return std::unexpected{CollectionError::out_of_memory};
        } catch (...) {
            return std::unexpected{CollectionError::element_operation_failed};
        }
        return {};
    }

    void reserve(std::size_t newCapacity) {
        const auto result{try_reserve(newCapacity)};
        if (!result) {
            if (result.error() == CollectionError::capacity_overflow) {
                throw std::length_error{"SmallVec capacity overflow"};
            }
            if (result.error() == CollectionError::out_of_memory) {
                throw std::bad_alloc{};
            }
            throw std::runtime_error{"SmallVec element relocation failed"};
        }
    }

    template <class... Args>
    T& emplace_back(Args&&... args) {
        if (size_ == max_size_()) {
            throw std::length_error{"SmallVec capacity overflow"};
        }
        if (size_ == capacity_) {
            return reallocate_emplace_(growth_capacity_(size_ + 1), std::forward<Args>(args)...);
        }
        T* location{data() + size_};
        Traits::construct(allocator_, location, std::forward<Args>(args)...);
        ++size_;
        return *location;
    }

    void push_back(const T& value) {
        emplace_back(value);
    }

    void push_back(T&& value)
    requires(std::is_move_constructible_v<T>)
    {
        emplace_back(std::move(value));
    }

    void pop_back() noexcept {
        assert(size_ > 0);
        --size_;
        Traits::destroy(allocator_, data() + size_);
    }

    void insert(std::size_t index, T value) {
        assert(index <= size_);
        if (index == size_) {
            push_back(std::move(value));
            return;
        }
        if (size_ == capacity_) {
            reserve(growth_capacity_(size_ + 1));
        }
        T* values{data()};
        Traits::construct(allocator_, values + size_, std::move(values[size_ - 1]));
        ++size_;
        for (std::size_t cursor{size_ - 2}; cursor > index; --cursor) {
            values[cursor] = std::move(values[cursor - 1]);
        }
        values[index] = std::move(value);
    }

    [[nodiscard]] T ordered_remove(std::size_t index) {
        assert(index < size_);
        T result{std::move(data()[index])};
        for (std::size_t cursor{index}; cursor + 1 < size_; ++cursor) {
            data()[cursor] = std::move(data()[cursor + 1]);
        }
        pop_back();
        return result;
    }

    [[nodiscard]] T swap_remove(std::size_t index) {
        assert(index < size_);
        T result{std::move(data()[index])};
        if (index + 1 < size_) {
            data()[index] = std::move(data()[size_ - 1]);
        }
        pop_back();
        return result;
    }

    void clear() noexcept {
        destroy_elements_();
    }
};

}  // namespace mbun::core::collections
