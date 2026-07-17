// state.cppm — platform-neutral watchlist, filtering, de-duplication, and
// deferred eviction state.
// PORT-SOURCE: bun-ref/src/watcher/Watcher.rs::WatchItem and add/remove paths;
//               bun-zig-src/src/watcher/Watcher.zig::WatchItem/indexOf.
export module mbun.watcher.state;

import std;
import mbun.watcher.event;

namespace mbun::watcher {

export using HashType = std::uint32_t;
export inline constexpr std::size_t MAX_EVICTION_COUNT{8096};

export enum class WatchItemKind : std::uint8_t { File, Directory };

// Native fd ownership and PackageJSON/Loader cycle-breaking are intentionally
// opaque here. The runtime binding supplies them in a later pass.
export struct WatchItem {
    std::string file_path;
    HashType hash{};
    std::uint32_t loader_tag{};
    std::int64_t fd{-1};
    std::uint32_t count{};
    HashType parent_hash{};
    WatchItemKind kind{WatchItemKind::File};
    std::uint32_t eventlist_index{};
};

export using HashFunction = std::function<HashType(std::string_view)>;

export inline HashType hash_path(std::string_view path) noexcept {
    // Shape-preserving placeholder for bun_wyhash::hash(path) truncation.
    // Exact wyhash parity is DEFERRED(S-watcher-hash).
    HashType hash{2166136261U};
    for (const auto byte : path) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 16777619U;
    }
    return hash;
}

export class WatchList {
private:
    std::vector<WatchItem> items_;
    std::vector<WatchItemIndex> eviction_indices_;

public:
    [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
    [[nodiscard]] const std::vector<WatchItem>& items() const noexcept { return items_; }
    [[nodiscard]] std::optional<WatchItemIndex> index_of(HashType hash) const noexcept {
        for (std::size_t index{0}; index < items_.size(); ++index) {
            if (items_[index].hash == hash) {
                return static_cast<WatchItemIndex>(index);
            }
        }
        return std::nullopt;
    }

    // Bun treats an existing hash as already watched; the platform fd refresh
    // policy is kept at the caller boundary for the later backend pass.
    [[nodiscard]] bool add(WatchItem item) {
        if (index_of(item.hash).has_value()) {
            return false;
        }
        items_.push_back(std::move(item));
        return true;
    }

    void mark_for_removal(WatchItemIndex index) {
        if (index < items_.size() && eviction_indices_.size() < MAX_EVICTION_COUNT) {
            eviction_indices_.push_back(index);
        }
    }

    // Bun flushes highest indices first because swap-remove changes ordering.
    void flush_evictions() {
        std::ranges::sort(eviction_indices_, std::greater<WatchItemIndex>{});
        eviction_indices_.erase(std::ranges::unique(eviction_indices_).begin(), eviction_indices_.end());
        for (const auto index : eviction_indices_) {
            if (index < items_.size()) {
                items_[index] = std::move(items_.back());
                items_.pop_back();
            }
        }
        eviction_indices_.clear();
    }
};

}  // namespace mbun::watcher
