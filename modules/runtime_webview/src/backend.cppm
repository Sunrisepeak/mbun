// backend.cppm — injectable seam for platform WebView implementations.
// Cocoa/WebView2/WebKitGTK and message-loop integration are DEFERRED.
export module mbun.runtime_webview.backend;

import std;
import mbun.runtime_webview.navigation;
import mbun.runtime_webview.view;

namespace mbun::runtime_webview {

export enum class BackendResult : std::uint8_t { ok, unavailable, invalid_request, closed };

export class Backend {
public:
    virtual ~Backend() = default;
    [[nodiscard]] virtual BackendResult create(const View&) noexcept = 0;
    [[nodiscard]] virtual BackendResult navigate(const NavigationRequest&) noexcept = 0;
    [[nodiscard]] virtual BackendResult close() noexcept = 0;
};

export class DeferredBackend final : public Backend {
public:
    [[nodiscard]] BackendResult create(const View&) noexcept override {
        return BackendResult::unavailable;
    }
    [[nodiscard]] BackendResult navigate(const NavigationRequest&) noexcept override {
        return BackendResult::unavailable;
    }
    [[nodiscard]] BackendResult close() noexcept override { return BackendResult::unavailable; }
};

} // namespace mbun::runtime_webview
