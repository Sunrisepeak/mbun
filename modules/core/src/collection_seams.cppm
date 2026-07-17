// collection_seams.cppm — first-stage collection seams not covered by
// mbun.core.collections.
//
// References (MIT, read-only):
//   bun Rust: src/collections/{lib.rs,hive_array.rs,array_hash_map.rs}
//   bun Zig:  src/collections/{StaticHashMap.zig,hive_array.zig}
//   bun Zig:  src/bundler/PathToSourceIndexMap.zig
//
// This file deliberately does not extend collections.cppm. It provides small,
// independently consumable seams so downstream ports can select a data
// structure without making the already-covered bitset/StringPool/SmallVec
// module a conflict hotspot. The implementations are value-safe C++ scaffolds;
// allocator injection, intrusive storage, and hash-table probing remain
// explicitly deferred until a real consumer supplies those contracts.
export module mbun.core.collection_seams;

import std;
import mbun.core.collections;

export namespace mbun::core::collection_seams {

// Rust SmallList keeps the smallvec layout while preserving a CSS-facing API.
// Reuse the existing inline storage implementation and expose the missing
// list-shaped operations under a separate module.
template <class T, std::size_t InlineCapacity>
class SmallList {
private:
    collections::SmallVec<T, InlineCapacity> values_;

public:
    using value_type = T;

    [[nodiscard]] std::size_t size() const noexcept { return values_.size(); }
    [[nodiscard]] bool empty() const noexcept { return values_.empty(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return values_.capacity(); }
    [[nodiscard]] T* data() noexcept { return values_.data(); }
    [[nodiscard]] const T* data() const noexcept { return values_.data(); }
    [[nodiscard]] T& at(std::size_t index) noexcept { return values_[index]; }
    [[nodiscard]] const T& at(std::size_t index) const noexcept { return values_[index]; }
    [[nodiscard]] std::span<T> slice() noexcept { return values_.span(); }
    [[nodiscard]] std::span<const T> slice() const noexcept { return values_.span(); }

    void append(const T& value) { values_.push_back(value); }
    void append(T&& value) { values_.push_back(std::move(value)); }

    template <class... Args>
    T& emplace(Args&&... args) {
        return values_.emplace_back(std::forward<Args>(args)...);
    }

    void clear() noexcept { values_.clear(); }
    [[nodiscard]] T ordered_remove(std::size_t index) { return values_.ordered_remove(index); }
    [[nodiscard]] T swap_remove(std::size_t index) { return values_.swap_remove(index); }
};

// Bun's ArrayHashMap preserves insertion order. This bounded front-end is a
// small-map seam for consumers whose cardinality is known to stay small; it
// intentionally uses linear lookup until the hash/probe contract is needed.
template <class K, class V, std::size_t InlineCapacity, class Equal = std::equal_to<K>>
class SmallMap {
public:
    struct Entry {
        K key;
        V value;
    };

private:
    SmallList<Entry, InlineCapacity> entries_;
    Equal equal_{};

public:
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
    [[nodiscard]] std::span<const Entry> entries() const noexcept { return entries_.slice(); }

    [[nodiscard]] V* find(const K& key) noexcept {
        for (Entry& entry : entries_.slice()) {
            if (equal_(entry.key, key)) {
                return std::addressof(entry.value);
            }
        }
        return nullptr;
    }

    [[nodiscard]] const V* find(const K& key) const noexcept {
        for (const Entry& entry : entries_.slice()) {
            if (equal_(entry.key, key)) {
                return std::addressof(entry.value);
            }
        }
        return nullptr;
    }

    template <class KeyArg, class... Args>
    V& get_or_put(KeyArg&& key, Args&&... args) {
        if (V* existing{find(key)}) {
            return *existing;
        }
        Entry& entry{entries_.emplace(Entry{std::forward<KeyArg>(key),
                                            V(std::forward<Args>(args)...)} )};
        return entry.value;
    }

    bool erase(const K& key) {
        for (std::size_t index{0}; index < entries_.size(); ++index) {
            if (equal_(entries_.at(index).key, key)) {
                entries_.ordered_remove(index);
                return true;
            }
        }
        return false;
    }

    void clear() noexcept { entries_.clear(); }
};

// Stable slot/index storage distilled from HiveArray and PathToSourceIndexMap.
// A generation makes stale handles fail after a slot is recycled.
struct ArenaIndex {
    std::uint32_t slot{std::numeric_limits<std::uint32_t>::max()};
    std::uint32_t generation{0};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return slot != std::numeric_limits<std::uint32_t>::max();
    }
    friend constexpr bool operator==(ArenaIndex, ArenaIndex) = default;
};

template <class T>
class IndexArena {
private:
    struct Slot {
        std::optional<T> value;
        std::uint32_t generation{1};
        std::uint32_t nextFree{std::numeric_limits<std::uint32_t>::max()};
    };

    std::vector<Slot> slots_;
    std::uint32_t firstFree_{std::numeric_limits<std::uint32_t>::max()};

    [[nodiscard]] Slot* slot_(ArenaIndex index) noexcept {
        if (!index.valid() || index.slot >= slots_.size()) {
            return nullptr;
        }
        Slot& slot{slots_[index.slot]};
        return slot.value && slot.generation == index.generation ? std::addressof(slot) : nullptr;
    }

public:
    [[nodiscard]] std::size_t size() const noexcept {
        return static_cast<std::size_t>(std::ranges::count_if(
            slots_, [](const Slot& slot) { return slot.value.has_value(); }));
    }

    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    template <class... Args>
    [[nodiscard]] ArenaIndex emplace(Args&&... args) {
        std::uint32_t slotIndex;
        if (firstFree_ != std::numeric_limits<std::uint32_t>::max()) {
            slotIndex = firstFree_;
            Slot& slot{slots_[slotIndex]};
            firstFree_ = slot.nextFree;
            slot.nextFree = std::numeric_limits<std::uint32_t>::max();
            slot.value.emplace(std::forward<Args>(args)...);
            return {slotIndex, slot.generation};
        }
        if (slots_.size() >= std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error{"IndexArena index overflow"};
        }
        slots_.push_back(Slot{std::optional<T>{std::in_place, std::forward<Args>(args)...}});
        slotIndex = static_cast<std::uint32_t>(slots_.size() - 1);
        return {slotIndex, slots_.back().generation};
    }

    [[nodiscard]] T* get(ArenaIndex index) noexcept {
        Slot* slot{slot_(index)};
        return slot == nullptr ? nullptr : std::addressof(*slot->value);
    }

    [[nodiscard]] const T* get(ArenaIndex index) const noexcept {
        if (!index.valid() || index.slot >= slots_.size()) {
            return nullptr;
        }
        const Slot& slot{slots_[index.slot]};
        return slot.value && slot.generation == index.generation
                   ? std::addressof(*slot.value)
                   : nullptr;
    }

    bool erase(ArenaIndex index) noexcept {
        Slot* slot{slot_(index)};
        if (slot == nullptr) {
            return false;
        }
        slot->value.reset();
        ++slot->generation;
        if (slot->generation == 0) {
            slot->generation = 1;
        }
        slot->nextFree = firstFree_;
        firstFree_ = index.slot;
        return true;
    }

    void clear() noexcept {
        slots_.clear();
        firstFree_ = std::numeric_limits<std::uint32_t>::max();
    }
};

// Rust PriorityQueue is a comparator-context min heap. C++'s comparator
// convention is retained: `Compare(a, b)` means a has higher priority than b.
template <class T, class Compare = std::less<T>>
class PriorityQueue {
private:
    std::vector<T> items_;
    Compare compare_{};

    void sift_up_(std::size_t child) {
        while (child > 0) {
            const std::size_t parent{(child - 1) / 2};
            if (!compare_(items_[child], items_[parent])) {
                break;
            }
            std::swap(items_[child], items_[parent]);
            child = parent;
        }
    }

    void sift_down_() {
        std::size_t parent{0};
        while (true) {
            const std::size_t left{parent * 2 + 1};
            if (left >= items_.size()) {
                return;
            }
            std::size_t best{left};
            const std::size_t right{left + 1};
            if (right < items_.size() && compare_(items_[right], items_[left])) {
                best = right;
            }
            if (!compare_(items_[best], items_[parent])) {
                return;
            }
            std::swap(items_[parent], items_[best]);
            parent = best;
        }
    }

public:
    [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
    [[nodiscard]] bool empty() const noexcept { return items_.empty(); }

    void push(T value) {
        items_.push_back(std::move(value));
        sift_up_(items_.size() - 1);
    }

    [[nodiscard]] std::optional<T> pop() {
        if (items_.empty()) {
            return std::nullopt;
        }
        std::swap(items_.front(), items_.back());
        T result{std::move(items_.back())};
        items_.pop_back();
        if (!items_.empty()) {
            sift_down_();
        }
        return result;
    }

    void clear() noexcept { items_.clear(); }
};

}  // namespace mbun::core::collection_seams
