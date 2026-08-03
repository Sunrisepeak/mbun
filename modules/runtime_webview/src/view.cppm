// view.cppm — runtime-independent WebView configuration and lifecycle.
// Reference intent: Bun runtime/webview view ownership and lifecycle model.
// Local .mbun Bun Rust/Zig sources are unavailable in this worktree; exact
// native window behavior remains DEFERRED until those references are restored.
export module mbun.runtime_webview.view;

import std;

namespace mbun::runtime_webview {

export enum class ViewState : std::uint8_t { created, loading, ready, closed };

export struct ViewOptions {
    std::string title { "mbun WebView" };
    std::uint32_t width { 800 };
    std::uint32_t height { 600 };
    std::string initial_url {};
    bool resizable { true };
    bool visible { true };

    // Explicit member: a defaulted friend operator== on an exported struct
    // ICEs GCC 16 when instantiated across a module import.
    [[nodiscard]] bool operator==(const ViewOptions& other) const {
        return title == other.title && width == other.width && height == other.height
            && initial_url == other.initial_url && resizable == other.resizable
            && visible == other.visible;
    }
};

export class View final {
    std::uint64_t id_ { 0 };
    ViewOptions options_ {};
    ViewState state_ { ViewState::created };
    std::string current_url_ {};

public:
    explicit View(std::uint64_t id, ViewOptions options = {})
        : id_ { id }, options_ { std::move(options) }, current_url_ { options_.initial_url } {}

    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }
    [[nodiscard]] const ViewOptions& options() const noexcept { return options_; }
    [[nodiscard]] ViewState state() const noexcept { return state_; }
    [[nodiscard]] std::string_view current_url() const noexcept { return current_url_; }

    void mark_loading() noexcept {
        if (state_ != ViewState::closed) {
            state_ = ViewState::loading;
        }
    }

    void mark_ready() noexcept {
        if (state_ == ViewState::loading || state_ == ViewState::created) {
            state_ = ViewState::ready;
        }
    }

    void set_current_url(std::string url) {
        if (state_ != ViewState::closed) {
            current_url_ = std::move(url);
        }
    }

    void close() noexcept { state_ = ViewState::closed; }
};

} // namespace mbun::runtime_webview
