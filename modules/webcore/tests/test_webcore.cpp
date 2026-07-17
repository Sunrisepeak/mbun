import std;
import mbun.webcore;

namespace {
int checks { 0 };
int failures { 0 };

void check(bool value, std::string_view label) {
    ++checks;
    if (!value) { ++failures; std::println("FAIL: {}", label); }
}
}

int main() {
    mbun::webcore::dom::Document document;
    const auto body = document.create_element("body");
    const auto text = document.create_text("hello");
    check(document.append_child(document.root(), body).has_value(), "append body");
    check(document.append_child(body, text).has_value(), "append text");
    check(document.set_attribute(body, "class", "page").has_value(), "set attribute");
    check(!document.append_child(text, document.root()).has_value(), "reject cycle");

    mbun::webcore::event::EventTarget target;
    int calls { 0 };
    const auto first = target.add_event_listener("load", [&](auto&) { ++calls; });
    target.add_event_listener("load", [&](auto& event) { ++calls; event.stop_propagation(); });
    target.add_event_listener("load", [&](auto&) { ++calls; });
    mbun::webcore::event::Event load { "load", false, true };
    check(target.dispatch_event(load), "dispatch not canceled");
    check(calls == 2, "listener snapshot and stop");
    check(target.remove_event_listener(first), "remove listener");

    mbun::webcore::stream::ReadableStream readable {
        [](auto& stream) { stream.enqueue({ "chunk" }); stream.close(); },
        {}
    };
    auto firstRead = readable.read();
    check(firstRead.has_value() && firstRead->value->bytes == "chunk", "pull read");
    auto doneRead = readable.read();
    check(doneRead.has_value() && doneRead->done, "closed read");
    bool wrote { false };
    mbun::webcore::stream::WritableStream writable {
        [&](const auto& chunk) -> std::expected<void, std::string> { wrote = chunk.bytes == "x"; return {}; }
    };
    check(writable.write({ "x" }).has_value() && wrote, "write");
    writable.close();
    check(!writable.write({ "y" }).has_value(), "reject write after close");

    mbun::webcore::backend::NullBackend backend;
    check(!backend.supports(mbun::webcore::backend::Capability::dom), "null backend capability");
    check(!backend.bind_document(document).has_value(), "null backend bind");
    std::println("test_webcore: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}
