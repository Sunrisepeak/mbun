// Request routing seam derived from bun runtime/server/ServerConfig and
// runtime/server/RequestContext. The JSC callback and uWS transport stay out
// of this pure layer.
export module mbun.runtime_server.request_dispatch;

import std;

namespace mbun::runtime_server {

export enum class HttpMethod : std::uint8_t {
    any,
    get,
    head,
    post,
    put,
    patch,
    del,
    options,
    connect,
    trace,
};

export struct RequestView {
    std::string_view method{};
    std::string_view path{};
};

export struct RouteMatch {
    std::size_t routeIndex{};
    bool found{};
};

export constexpr std::optional<HttpMethod> method_from(std::string_view method) {
    if (method == "GET") return HttpMethod::get;
    if (method == "HEAD") return HttpMethod::head;
    if (method == "POST") return HttpMethod::post;
    if (method == "PUT") return HttpMethod::put;
    if (method == "PATCH") return HttpMethod::patch;
    if (method == "DELETE") return HttpMethod::del;
    if (method == "OPTIONS") return HttpMethod::options;
    if (method == "CONNECT") return HttpMethod::connect;
    if (method == "TRACE") return HttpMethod::trace;
    return std::nullopt;
}

export struct Route {
    std::string path;
    HttpMethod method{HttpMethod::any};
    std::size_t handlerId{};
};

export class RequestDispatcher {
private:
    std::vector<Route> routes_;

    static bool method_matches_(HttpMethod routeMethod, std::string_view requestMethod) {
        return routeMethod == HttpMethod::any || method_from(requestMethod) == routeMethod;
    }

public:
    void add_route(std::string path, HttpMethod method, std::size_t handlerId) {
        routes_.push_back(Route{std::move(path), method, handlerId});
    }

    RouteMatch dispatch(RequestView request) const {
        // Bun's static route setup lets later declarations override earlier
        // ones. Reverse lookup preserves that rule without rebuilding a map.
        for (std::size_t i{routes_.size()}; i > 0; --i) {
            const auto& route{routes_[i - 1]};
            if (route.path == request.path && method_matches_(route.method, request.method)) {
                return RouteMatch{i - 1, true};
            }
        }
        return RouteMatch{};
    }

    const Route* route_at(std::size_t index) const {
        return index < routes_.size() ? &routes_[index] : nullptr;
    }

    std::size_t size() const { return routes_.size(); }
};

}
