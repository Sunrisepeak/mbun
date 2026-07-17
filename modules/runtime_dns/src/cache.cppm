// cache.cppm — bounded completed/inflight DNS cache.
// PORT-SOURCE: bun-ref/src/runtime/dns_jsc/dns.rs GlobalCache/RequestKeyOwned;
//              bun-zig-src/src/runtime/dns_jsc/dns.zig pending cache paths.
export module mbun.runtime_dns.cache;

import std;
import mbun.runtime_dns.request;
import mbun.runtime_dns.backend;
import mbun.runtime_dns.record;

namespace mbun::runtime_dns {

export struct CacheKey {
    std::string name;
    std::uint16_t port{0};
    std::vector<std::uint8_t> options;
    std::optional<RecordQuery> record;

    friend bool operator==(const CacheKey&, const CacheKey&) = default;
};

export struct CacheKeyHash {
    std::size_t operator()(const CacheKey& key) const noexcept {
        std::size_t h{1469598103934665603ull};
        auto add = [&h](std::uint8_t byte) { h = (h ^ byte) * 1099511628211ull; };
        for (auto c : key.name) add(static_cast<std::uint8_t>(c));
        add(static_cast<std::uint8_t>(key.port));
        add(static_cast<std::uint8_t>(key.port >> 8));
        for (auto byte : key.options) add(byte);
        if (key.record) {
            add(static_cast<std::uint8_t>(key.record->type));
            add(static_cast<std::uint8_t>(key.record->kind));
        }
        return h;
    }
};

export class Cache {
public:
    static constexpr std::size_t MAX_ENTRIES{256};

private:
    struct Entry {
        std::shared_ptr<Request> request;
        std::uint64_t completedAt{0};
    };
    std::unordered_map<CacheKey, Entry, CacheKeyHash> entries_;
    std::uint64_t ttlSeconds_{30};

public:
    explicit Cache(std::uint64_t ttlSeconds = 30) : ttlSeconds_{ttlSeconds} {}

    std::shared_ptr<Request> find(const CacheKey& key, std::uint64_t now) {
        auto it{entries_.find(key)};
        if (it == entries_.end()) return {};
        if (it->second.request->state() != RequestState::Pending
            && now - it->second.completedAt > ttlSeconds_) {
            entries_.erase(it);
            return {};
        }
        it->second.request->add_waiter();
        return it->second.request;
    }

    std::shared_ptr<Request> insert(CacheKey key, BackendQuery query, std::uint64_t now) {
        if (entries_.size() >= MAX_ENTRIES) evict_one_();
        auto request{std::make_shared<Request>(std::move(query))};
        entries_.insert_or_assign(std::move(key), Entry{request, now});
        return request;
    }

    void complete(const CacheKey& key, const BackendResult& result, std::uint64_t now) {
        auto it{entries_.find(key)};
        if (it == entries_.end()) return;
        it->second.request->complete(result);
        it->second.completedAt = now;
    }

    std::size_t size() const noexcept { return entries_.size(); }

private:
    void evict_one_() {
        auto it{std::ranges::find_if(entries_, [](const auto& item) {
            return item.second.request->state() != RequestState::Pending;
        })};
        if (it != entries_.end()) entries_.erase(it);
    }
};

}  // namespace mbun::runtime_dns
