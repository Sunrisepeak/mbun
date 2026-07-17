// resolver.cppm — runtime DNS facade. JSC/promise and native adapters plug in
// at Backend; this module owns only request deduplication and cache policy.
export module mbun.runtime_dns.resolver;

import std;
import mbun.runtime_dns.backend;
import mbun.runtime_dns.cache;
import mbun.runtime_dns.record;
import mbun.runtime_dns.request;

namespace mbun::runtime_dns {

export class Resolver {
private:
    Backend backend_;
    Cache cache_;
    std::uint64_t clock_{0};

    static CacheKey key_for_(const BackendQuery& query) {
        return std::visit([](const auto& item) {
            CacheKey key;
            if constexpr (std::same_as<std::decay_t<decltype(item)>, Query>) {
                key.name = item.name;
                key.port = item.port;
                auto packed{item.options.to_packed_bytes()};
                key.options.assign(packed.begin(), packed.end());
            } else {
                key.name = item.name;
                key.record = item;
            }
            return key;
        }, query.query);
    }

public:
    explicit Resolver(Backend backend, std::uint64_t ttlSeconds = 30)
        : backend_{std::move(backend)}, cache_{ttlSeconds} {}

    std::shared_ptr<Request> submit(BackendQuery query) {
        auto key{key_for_(query)};
        if (auto hit{cache_.find(key, clock_)}) return hit;
        auto request{cache_.insert(key, query, clock_)};
        request->complete(backend_.submit(query));
        return request;
    }

    void advance(std::uint64_t seconds = 1) noexcept { clock_ += seconds; }
    std::size_t cache_size() const noexcept { return cache_.size(); }
};

}  // namespace mbun::runtime_dns
