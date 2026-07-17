// request.cppm — pending DNS request state and waiter coalescing.
// The intrusive linked list in bun is represented by a small owning vector;
// the externally visible state transitions are the same, without raw JSC ptrs.
export module mbun.runtime_dns.request;

import std;
import mbun.runtime_dns.backend;

namespace mbun::runtime_dns {

export enum class RequestState : std::uint8_t { Pending, Succeeded, Failed, Cancelled };

export class Request {
private:
    BackendQuery query_;
    RequestState state_{RequestState::Pending};
    std::optional<BackendResult> result_;
    std::size_t waiterCount_{1};

public:
    explicit Request(BackendQuery query) : query_{std::move(query)} {}

    const BackendQuery& query() const noexcept { return query_; }
    RequestState state() const noexcept { return state_; }
    const std::optional<BackendResult>& result() const noexcept { return result_; }
    std::size_t waiter_count() const noexcept { return waiterCount_; }
    void add_waiter() noexcept { ++waiterCount_; }

    void complete(BackendResult result) {
        if (state_ != RequestState::Pending) return;
        state_ = result.is_ok() ? RequestState::Succeeded : RequestState::Failed;
        result_ = std::move(result);
    }

    void cancel() {
        if (state_ == RequestState::Pending) state_ = RequestState::Cancelled;
    }
};

}  // namespace mbun::runtime_dns
