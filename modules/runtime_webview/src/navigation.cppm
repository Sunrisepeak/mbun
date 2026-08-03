// navigation.cppm — URL navigation request/result value types.
// The native URL loader and policy enforcement are DEFERRED.
export module mbun.runtime_webview.navigation;

import std;

namespace mbun::runtime_webview {

export enum class NavigationKind : std::uint8_t { initial, link, script, reload };

export struct NavigationRequest {
    std::string url {};
    NavigationKind kind { NavigationKind::initial };
    bool user_initiated { false };

    [[nodiscard]] bool is_valid() const noexcept {
        return !url.empty() && url.find_first_of("\r\n") == std::string::npos;
    }
};

export enum class NavigationResult : std::uint8_t { accepted, rejected_invalid_url, rejected_closed };

export struct NavigationDecision {
    NavigationResult result { NavigationResult::rejected_invalid_url };
    std::string url {};

    [[nodiscard]] bool accepted() const noexcept { return result == NavigationResult::accepted; }
};

export [[nodiscard]] inline NavigationDecision decide_navigation(const NavigationRequest& request,
                                                                 bool view_closed) {
    if (view_closed) {
        return { NavigationResult::rejected_closed, request.url };
    }
    if (!request.is_valid()) {
        return { NavigationResult::rejected_invalid_url, request.url };
    }
    return { NavigationResult::accepted, request.url };
}

} // namespace mbun::runtime_webview
