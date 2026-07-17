#include <cassert>

import std;
import mbun.dns;
import mbun.runtime_dns;

int main() {
    using namespace mbun::runtime_dns;
    int calls{0};
    mbun::runtime_dns::Backend backend{[&calls](const BackendQuery& query) {
        ++calls;
        const auto& lookup{std::get<Query>(query.query)};
        return BackendResult::success(AddressResults{
            AddressResult{mbun::dns::Address::v4({127, 0, 0, 1}), 12},
        }, 12);
    }};
    Resolver resolver{std::move(backend), 30};
    Query query{"localhost", 80, mbun::dns::Options{}};
    auto first{resolver.submit(BackendQuery{query})};
    auto second{resolver.submit(BackendQuery{query})};
    assert(first == second);
    assert(first->state() == RequestState::Succeeded);
    assert(first->waiter_count() == 2);
    assert(calls == 1);
    assert(std::get<AddressResults>(*first->result()->value).size() == 1);

    resolver.advance(31);
    auto third{resolver.submit(BackendQuery{query})};
    assert(third != first);
    assert(calls == 2);
    assert(resolver.cache_size() == 1);
    return 0;
}
