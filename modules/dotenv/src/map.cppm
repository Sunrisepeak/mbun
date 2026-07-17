// map.cppm — mbun.dotenv.map: the ordered key/value store the .env parser fills.
//
// Port of bun's `Map` (src/dotenv/env_loader.rs). bun backs it with an
// ArrayHashMap so insertion order is preserved and each key maps to a single
// owned value; entries added later can override earlier ones (see the parser's
// per-file override rule). Here we keep it minimal and pure: an ordered vector
// of key/value pairs plus a name->index side table for O(1) lookup. Values are
// owned std::string; keys are stored once. ref: bun src/dotenv/env_loader.rs
// (struct Map, HashTableValue, get / get_or_put / put).
export module mbun.dotenv.map;

import std;

namespace mbun::dotenv {

// Ordered map from env name to owned value, matching bun's insertion-ordered
// ArrayHashMap semantics (later inserts keep their slot; values are replaceable
// in place). Lookups are by byte-slice (std::string_view) key.
export class Map {
public:
    // Result of get_or_put: the entry's stable index and whether it already
    // existed. Mirrors bun's `get_or_put` return (found_existing + index).
    struct GetOrPut {
        std::size_t index;
        bool found_existing;
    };

public:
    Map() = default;

public:
    std::size_t count() const {
        return keys_.size();
    }

    std::string_view key_at(std::size_t idx) const {
        return keys_[idx];
    }

    std::string_view value_at(std::size_t idx) const {
        return values_[idx];
    }

    // Byte-slice lookup. Returns nullopt when the key is absent.
    std::optional<std::string_view> get(std::string_view key) const {
        if (auto it = index_.find(std::string(key)); it != index_.end()) {
            return std::string_view(values_[it->second]);
        }
        return std::nullopt;
    }

    // Returns the entry for `key`, inserting an empty-valued slot when it does
    // not exist yet. `found_existing` reports whether the key was already
    // present (so the caller can apply bun's override rule).
    GetOrPut get_or_put(std::string_view key) {
        std::string k { key };
        if (auto it = index_.find(k); it != index_.end()) {
            return { it->second, true };
        }
        std::size_t idx = keys_.size();
        keys_.push_back(k);
        values_.emplace_back();
        index_.emplace(std::move(k), idx);
        return { idx, false };
    }

    void set_value(std::size_t idx, std::string value) {
        values_[idx] = std::move(value);
    }

    // Convenience insert-or-replace (used by tests / callers that don't need
    // the override bookkeeping). Mirrors bun's `Map::put`.
    void put(std::string_view key, std::string_view value) {
        auto gp = get_or_put(key);
        values_[gp.index] = std::string(value);
    }

private:
    std::vector<std::string> keys_;
    std::vector<std::string> values_;
    std::unordered_map<std::string, std::size_t> index_;
};

} // namespace mbun::dotenv
