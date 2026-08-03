import std;
import mbun.dispatch;

namespace {
int checks{0};
int failures{0};
void check(bool condition, std::string_view message) {
    ++checks;
    if (!condition) { ++failures; std::println("FAIL: {}", message); }
}

void test_priority_is_stable() {
    mbun::dispatch::QueueExecutor executor;
    std::vector<int> order;
    executor.submit(mbun::dispatch::Priority::normal, [&] { order.push_back(1); return false; });
    executor.submit(mbun::dispatch::Priority::high, [&] { order.push_back(2); return false; });
    executor.submit(mbun::dispatch::Priority::high, [&] { order.push_back(3); return false; });
    executor.submit(mbun::dispatch::Priority::low, [&] { order.push_back(4); return false; });
    executor.drain();
    check(order == std::vector<int>{2, 3, 1, 4}, "higher priority runs first and ties stay FIFO");
}

void test_cancellation_skips_pending_work() {
    mbun::dispatch::QueueExecutor executor;
    auto source{mbun::dispatch::CancellationSource::create()};
    bool ran{false};
    auto handle{executor.submit(mbun::dispatch::Priority::normal,
                                [&] { ran = true; return false; }, source.token())};
    check(executor.cancel(handle), "executor cancels a pending task");
    check(handle.is_cancelled(), "cancelled handle reports cancellation");
    executor.drain();
    check(!ran, "cancelled task is not invoked");
    check(executor.empty(), "cancelled task is removed while draining");
}

void test_repeating_task_uses_executor_seam() {
    mbun::dispatch::QueueExecutor executor;
    int runs{0};
    executor.submit(mbun::dispatch::Priority::normal, [&] { ++runs; return runs < 3; });
    check(executor.run_one(), "executor reports a task execution");
    check(executor.run_one(), "repeating task is scheduled again");
    check(executor.run_one(), "repeating task eventually completes");
    check(!executor.run_one(), "empty executor has no work");
    check(runs == 3, "callback true keeps a task and false removes it");
}

void test_deferred_post_is_idempotent_and_unregisters() {
    mbun::dispatch::QueueExecutor executor;
    int runs{0};
    auto callback = [&] { ++runs; return false; };
    auto first{executor.post_deferred(42, mbun::dispatch::Priority::normal, callback)};
    auto second{executor.post_deferred(42, mbun::dispatch::Priority::high, callback)};
    check(first.id() == second.id(), "repeated deferred post returns the existing task");
    check(executor.unregister_deferred(42), "deferred task can be unregistered");
    check(!executor.run_one(), "unregistered deferred task never runs");
    check(runs == 0, "unregistered callback is not invoked");
}

void test_source_cancels_all_copies() {
    auto source{mbun::dispatch::CancellationSource::create()};
    auto token{source.token()};
    check(!token.is_cancelled(), "new cancellation token is live");
    source.cancel();
    check(token.is_cancelled(), "source cancellation reaches token copies");
}
} // namespace

int main() {
    test_priority_is_stable();
    test_cancellation_skips_pending_work();
    test_repeating_task_uses_executor_seam();
    test_deferred_post_is_idempotent_and_unregisters();
    test_source_cancels_all_copies();
    std::println("dispatch: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}
