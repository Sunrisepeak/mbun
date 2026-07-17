export module mbun.sys_bindings.threading;

import std;

namespace mbun::threading {

// Ref: bun-ref/src/threading/{Mutex,Condition,Futex,ThreadPool}.rs and the
// equivalent Zig primitives. Concrete parking-lot/libuv scheduling is deferred.
export using Mutex = std::mutex;
export using Condition = std::condition_variable;

export enum class WaitResult : std::uint8_t { notified, timed_out };

export class WaitGroup {
private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::size_t count_ {};

public:
    void add(std::size_t amount = 1) {
        std::lock_guard lock {mutex_};
        count_ += amount;
    }

    void done() {
        std::lock_guard lock {mutex_};
        if (count_ > 0) {
            --count_;
        }
        if (count_ == 0) {
            condition_.notify_all();
        }
    }

    void wait() {
        std::unique_lock lock {mutex_};
        condition_.wait(lock, [this] { return count_ == 0; });
    }
};

export struct ThreadPoolConfig {
    std::uint32_t max_threads {1};
    std::uint32_t stack_size {};
};

} // namespace mbun::threading
