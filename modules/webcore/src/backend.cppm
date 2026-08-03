// mbun.webcore.backend — explicit seam for JSC/native WebCore bindings.
export module mbun.webcore.backend;

import std;
import mbun.webcore.dom;
import mbun.webcore.event;

export namespace mbun::webcore::backend {

enum class Capability : std::uint8_t { dom, events, streams };

class WebCoreBackend {
public:
    virtual ~WebCoreBackend() = default;
    virtual bool supports(Capability capability) const = 0;
    virtual std::expected<void, std::string> bind_document(dom::Document&) = 0;
    virtual std::expected<void, std::string> schedule(event::EventTarget&, event::Event&) = 0;
};

class NullBackend final : public WebCoreBackend {
public:
    bool supports(Capability) const override { return false; }
    std::expected<void, std::string> bind_document(dom::Document&) override {
        return std::unexpected("WebCore backend is not installed");
    }
    std::expected<void, std::string> schedule(event::EventTarget&, event::Event&) override {
        return std::unexpected("event scheduler backend is not installed");
    }
};

} // namespace mbun::webcore::backend
