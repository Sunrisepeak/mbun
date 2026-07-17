// scheduler.cppm — mbun.install.async_http.scheduler
//
// The concurrency budget: how many requests may be in flight on the loop at
// once, and what happens to the rest.
//
// PORT-SOURCE (values copied, not invented):
//   * MAX_SIMULTANEOUS_REQUESTS = 256 — the generic HTTP default
//     (src/http/AsyncHTTP.rs:86).
//   * `bun install` overrides it to 64 at startup —
//     DEFAULT_MAX_SIMULTANEOUS_REQUESTS_FOR_BUN_INSTALL (PackageManager.rs:335),
//     applied at :2198-2213. With a proxy the value is also 64
//     (DEFAULT_..._FOR_PROXIES, :336 — bun keeps the two constants separate to
//     allow future divergence, so we do too).
//   * Priority CLI → proxy → default (PackageManager.rs:2200-2211).
//   * BUN_CONFIG_MAX_HTTP_REQUESTS: u16 in 1..65535; 0 warns and is ignored
//     (AsyncHTTP.rs:229-257).
//   * --network-concurrency <NUM> is the CLI override (field
//     CommandLineArguments.rs:366, parsed at :1074).
//   * On a network error the budget halves toward a floor of 4, once per drain
//     (runTasks.rs:370-378) — reduce_max_simultaneous_requests already exists in
//     network_task.cppm:501 and is called here rather than re-derived.
//
// Admission is the *only* gate on install concurrency: install itself does not
// limit anything — it hands the whole discovered frontier to the HTTP thread at
// once (schedule_tasks, runTasks.rs:1655-1678) and lets this budget meter it.
//
// Ordering follows bun's drain_events (HTTPThread.rs:856): tasks that were
// popped but could not start go to a FIFO `deferred` list (:113) which is
// admitted *before* `queued` on the next tick, so a deferred request cannot be
// starved by newly queued ones.
//
// NOTE: bun's help text says "default 48" (CommandLineArguments.rs:104) while
// the constant is 64. That is a bug in bun's docs, not in bun's behaviour —
// aligning means copying the behaviour (64) and, where the help text is ported,
// copying the text (48) too. Do not "fix" either.
export module mbun.install.async_http.scheduler;

import std;
import mbun.install.network_task;

export namespace mbun::install::async_http {

namespace nt = mbun::install::network_task;

// Generic HTTP default (AsyncHTTP.rs:86). Present for fidelity; install never
// runs at this value because it overrides it at startup.
inline constexpr std::size_t MAX_SIMULTANEOUS_REQUESTS{256};

// What `bun install` actually runs at (PackageManager.rs:335).
inline constexpr std::size_t DEFAULT_MAX_SIMULTANEOUS_REQUESTS_FOR_BUN_INSTALL{64};

// Same value, kept separate exactly as bun keeps it (PackageManager.rs:336).
inline constexpr std::size_t DEFAULT_MAX_SIMULTANEOUS_REQUESTS_FOR_BUN_INSTALL_FOR_PROXIES{64};

// Floor for the halving (PackageManagerOptions.rs:131; mirrored in
// package_manager/options.cppm:413).
inline constexpr std::size_t MIN_SIMULTANEOUS_REQUESTS{4};

// Resolve the budget with bun's precedence: CLI → proxy → default
// (PackageManager.rs:2200-2211). `envValue` is the parsed
// BUN_CONFIG_MAX_HTTP_REQUESTS; 0/absent means unset (AsyncHTTP.rs:229-257 warns
// and ignores 0).
inline std::size_t resolve_max_simultaneous_requests(std::optional<std::size_t> networkConcurrency,
                                                     bool hasProxy,
                                                     std::optional<std::size_t> envValue) {
    if (networkConcurrency) {
        return std::max<std::size_t>(*networkConcurrency, 1);  // max(n, 1), :2202
    }
    if (envValue && *envValue > 0) {
        return *envValue;
    }
    if (hasProxy) {
        return DEFAULT_MAX_SIMULTANEOUS_REQUESTS_FOR_BUN_INSTALL_FOR_PROXIES;
    }
    return DEFAULT_MAX_SIMULTANEOUS_REQUESTS_FOR_BUN_INSTALL;
}

// Parse BUN_CONFIG_MAX_HTTP_REQUESTS (AsyncHTTP.rs:229-257): u16 1..65535.
// std::nullopt means "unset or unusable" — bun warns on 0 and on a non-number
// and carries on with the default rather than failing the install.
inline std::optional<std::size_t> parse_max_http_requests_env(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint32_t value{0};
    const auto [ptr, ec]{std::from_chars(text.data(), text.data() + text.size(), value)};
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    if (value == 0 || value > std::numeric_limits<std::uint16_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(value);
}

// Admission bookkeeping. Deliberately holds no request type: the engine owns the
// requests, this owns only the counting and the ordering, so the policy can be
// tested without a socket.
template <class T>
class AdmissionQueue {
private:
    std::deque<T> queued_{};
    std::deque<T> deferred_{};
    std::size_t active_{0};
    std::size_t max_{DEFAULT_MAX_SIMULTANEOUS_REQUESTS_FOR_BUN_INSTALL};
    std::size_t minRequests_{MIN_SIMULTANEOUS_REQUESTS};
    bool hasNetworkError_{false};

public:
    AdmissionQueue() = default;
    explicit AdmissionQueue(std::size_t max,
                            std::size_t minRequests = MIN_SIMULTANEOUS_REQUESTS)
        : max_{max}, minRequests_{minRequests} {}

    [[nodiscard]] std::size_t max_simultaneous_requests() const { return max_; }
    [[nodiscard]] std::size_t active() const { return active_; }
    [[nodiscard]] std::size_t pending() const { return queued_.size() + deferred_.size(); }
    [[nodiscard]] bool empty() const { return pending() == 0 && active_ == 0; }

    void push(T item) { queued_.push_back(std::move(item)); }

    // Re-enqueue for a retry: immediate, no backoff. bun does exactly this —
    // `task.retried += 1; enqueue_network_task(...)` (runTasks.rs:397-399 via
    // PackageManagerEnqueue.rs:600) with no timer, no jitter, and no Retry-After
    // (install never parses it; only `bun publish` does).
    void requeue(T item) { queued_.push_back(std::move(item)); }

    // Pop the next admissible item, or nullopt when at capacity / nothing to do.
    // Deferred first (HTTPThread.rs:113) so a request that lost an earlier race
    // is not starved by newer arrivals.
    std::optional<T> admit() {
        if (active_ >= max_) {
            // At capacity: park anything queued so it keeps FIFO order ahead of
            // later arrivals on the next tick.
            while (!queued_.empty()) {
                deferred_.push_back(std::move(queued_.front()));
                queued_.pop_front();
            }
            return std::nullopt;
        }
        std::deque<T>& source{!deferred_.empty() ? deferred_ : queued_};
        if (source.empty()) {
            return std::nullopt;
        }
        T item{std::move(source.front())};
        source.pop_front();
        ++active_;
        return item;
    }

    void on_finished() {
        if (active_ > 0) {
            --active_;
        }
    }

    // First network error of a drain halves the budget toward the floor; later
    // errors in the same drain do not compound it (bun gates on a
    // `has_network_error` bool per runTasks pass — runTasks.rs:370-378).
    // This is throttling, not backoff: bun has no backoff anywhere on this path.
    void note_network_error() {
        if (hasNetworkError_) {
            return;
        }
        hasNetworkError_ = true;
        max_ = nt::reduce_max_simultaneous_requests(max_, minRequests_);
    }

    // Call at the end of a drain so the next one may halve again.
    void end_drain() { hasNetworkError_ = false; }
};

}  // namespace mbun::install::async_http
