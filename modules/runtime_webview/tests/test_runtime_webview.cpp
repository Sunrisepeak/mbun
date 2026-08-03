import std;
import mbun.runtime_webview;

namespace {
int failures { 0 };

void check(bool value, std::string_view label) {
    if (!value) {
        ++failures;
        std::println("FAIL {}", label);
    }
}
} // namespace

int main() {
    using namespace mbun::runtime_webview;

    View view { 7, ViewOptions { .title = "Test", .width = 640, .height = 480,
                                 .initial_url = "https://example.test", .resizable = false } };
    check(view.state() == ViewState::created, "view starts created");
    check(view.current_url() == "https://example.test", "initial URL retained");
    view.mark_loading();
    view.mark_ready();
    check(view.state() == ViewState::ready, "loading reaches ready");

    auto accepted { decide_navigation(NavigationRequest { "https://example.test/next",
                                                           NavigationKind::link, true }, false) };
    check(accepted.accepted(), "valid navigation accepted");
    check(!decide_navigation(NavigationRequest { "https://bad\nurl" }, false).accepted(),
          "control character rejected");
    check(decide_navigation(NavigationRequest { "https://example.test" }, true).result ==
              NavigationResult::rejected_closed,
          "closed view rejects navigation");

    EventQueue queue;
    queue.push(Event { EventKind::created, view.id(), "created" });
    queue.push(Event { EventKind::navigation_finished, view.id(), "ready" });
    check(queue.size() == 2, "events queued");
    check(queue.pop()->kind == EventKind::created, "events FIFO");
    check(queue.pop()->kind == EventKind::navigation_finished, "second event delivered");
    check(!queue.pop().has_value(), "empty queue returns no event");

    DeferredBackend backend;
    check(backend.create(view) == BackendResult::unavailable, "native backend deferred");
    view.close();
    check(view.state() == ViewState::closed, "view closes");
    check(view.current_url() == "https://example.test", "close preserves URL");

    std::println("test_runtime_webview: {} failures", failures);
    return failures == 0 ? 0 : 1;
}
