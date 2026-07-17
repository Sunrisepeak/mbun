export module mbun.router;

import std;
import mbun.core.paths;
import mbun.resolver;

namespace mbun::router {

export struct FileSystem {
    std::function<std::vector<std::string>()> files;
};

export struct Options {
    std::string dir;
    std::vector<std::string> file_extensions;
    // Only Next.js-style routing is supported by bun's FileSystemRouter.
    std::string style{"nextjs"};
};

export struct RouteMatch {
    std::string name;
    std::string file_path;
    std::string pathname;
    std::map<std::string, std::string, std::less<>> params;
    std::map<std::string, std::string, std::less<>> query;
};

export class FileSystemRouter {
public:
    FileSystemRouter(FileSystem fileSystem, Options options)
        : fileSystem_{std::move(fileSystem)}, options_{std::move(options)} {
        reload();
    }

    void reload() {
        routes_.clear();
        routeList_.clear();
        std::vector<std::string> extensions{options_.file_extensions};
        if (extensions.empty()) {
            extensions = {".tsx", ".jsx", ".ts", ".mjs", ".cjs", ".js"};
        }
        for (auto& extension : extensions) {
            if (!extension.empty() && extension.front() != '.') {
                extension.insert(extension.begin(), '.');
            }
        }

        for (const auto& file : fileSystem_.files()) {
            if (file.empty() || file.front() == '.' || is_hidden_component(file)
                || contains_node_modules(file)) {
                continue;
            }
            const auto extension{file_extension(file)};
            if (!std::ranges::contains(extensions, extension)) {
                continue;
            }
            auto route{make_route(file, extension)};
            if (!route) {
                continue;
            }
            routes_[route->name] = join_path(options_.dir, file);
            routeList_.push_back(std::move(*route));
        }
        std::ranges::sort(routeList_, [](const Route& lhs, const Route& rhs) {
            if (lhs.kind != rhs.kind) {
                return lhs.kind < rhs.kind;
            }
            if (lhs.kind >= RouteKind::CatchAll && lhs.segment_count != rhs.segment_count) {
                return lhs.segment_count < rhs.segment_count;
            }
            return lhs.name < rhs.name;
        });
    }

    const std::map<std::string, std::string, std::less<>>& routes() const { return routes_; }
    std::optional<RouteMatch> match(std::string_view path) const {
        const auto [rawPath, rawQuery]{split_query(path)};
        const std::string decodedPath{percent_decode(rawPath)};
        const std::string normalized{normalize_path(decodedPath)};
        // Next.js treats "/foo/index", "/foo/index/" and repeated "/index"
        // suffixes as the parent route; "/index" collapses to "/".
        const std::string matchPath{strip_index_suffix(normalized)};
        const auto requestSegments{split_segments(matchPath)};
        for (const auto& route : routeList_) {
            std::map<std::string, std::string, std::less<>> params;
            if (!match_route(route, requestSegments, params)) {
                continue;
            }
            RouteMatch result{
                .name = route.name,
                .file_path = join_path(options_.dir, route.file),
                .pathname = normalized,
                .params = std::move(params),
                .query = parse_query(rawQuery),
            };
            return result;
        }
        return std::nullopt;
    }

private:
    enum class RouteKind : unsigned char { Static, Dynamic, CatchAll, OptionalCatchAll };

    struct Route {
        std::string name;
        std::string file;
        std::vector<std::string> segments;
        RouteKind kind{RouteKind::Static};
        std::size_t segment_count{0};
    };

    FileSystem fileSystem_;
    Options options_;
    std::map<std::string, std::string, std::less<>> routes_;
    std::vector<Route> routeList_;

    static bool is_hidden_component(std::string_view file) {
        std::size_t start{0};
        while (start < file.size()) {
            const auto end{file.find('/', start)};
            const auto length{end == std::string_view::npos ? file.size() : end};
            if (length > start && file[start] == '.') {
                return true;
            }
            if (end == std::string_view::npos) {
                break;
            }
            start = end + 1;
        }
        return false;
    }

    static bool contains_node_modules(std::string_view file) {
        constexpr std::string_view banned{"node_modules"};
        std::size_t start{0};
        while (start < file.size()) {
            const auto end{file.find('/', start)};
            const auto component{file.substr(start, end == std::string_view::npos ? file.size() - start : end - start)};
            if (component == banned) {
                return true;
            }
            if (end == std::string_view::npos) {
                break;
            }
            start = end + 1;
        }
        return false;
    }

    static std::string_view file_extension(std::string_view file) {
        const auto slash{file.find_last_of('/')};
        const auto dot{file.find_last_of('.')};
        if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) {
            return {};
        }
        return file.substr(dot);
    }

    static std::string join_path(std::string_view dir, std::string_view file) {
        if (dir.empty()) {
            return std::string{file};
        }
        std::string result{dir};
        if (result.back() != '/') {
            result.push_back('/');
        }
        result.append(file);
        return result;
    }

    static std::string strip_extension(std::string_view file, std::string_view extension) {
        return std::string{file.substr(0, file.size() - extension.size())};
    }

    static std::optional<Route> make_route(std::string_view file, std::string_view extension) {
        std::string routePath{strip_extension(file, extension)};
        const auto segmentViews{split_segments(routePath)};
        std::vector<std::string> segments;
        segments.reserve(segmentViews.size());
        for (const auto& view : segmentViews) {
            segments.emplace_back(view);
        }
        if (!segments.empty() && segments.back() == "index") {
            segments.pop_back();
        }
        std::string name{"/"};
        for (std::size_t i{0}; i < segments.size(); ++i) {
            if (i != 0) {
                name.push_back('/');
            }
            name.append(segments[i]);
        }
        Route route{
            .name = std::move(name),
            .file = std::string{file},
            .segments = std::move(segments),
            .kind = RouteKind::Static,
            .segment_count = 0,
        };
        for (const auto& segment : route.segments) {
            if (segment.starts_with("[[...") && segment.ends_with("]]")) {
                if (route.kind < RouteKind::OptionalCatchAll) {
                    route.kind = RouteKind::OptionalCatchAll;
                }
            } else if (segment.starts_with("[...") && segment.ends_with(']')) {
                if (route.kind < RouteKind::CatchAll) {
                    route.kind = RouteKind::CatchAll;
                }
            } else if (segment.starts_with('[') && segment.ends_with(']')) {
                if (segment.size() <= 2) {
                    return std::nullopt;
                }
                if (route.kind < RouteKind::Dynamic) {
                    route.kind = RouteKind::Dynamic;
                }
            } else if (segment.find('[') != std::string::npos
                       || segment.find(']') != std::string::npos) {
                // Malformed bracket, e.g. "[foo" is missing its closing bracket.
                return std::nullopt;
            }
            ++route.segment_count;
        }
        return route;
    }

    static std::vector<std::string_view> split_segments(std::string_view path) {
        std::vector<std::string_view> result;
        std::size_t start{path.starts_with('/') ? 1U : 0U};
        while (start <= path.size()) {
            const auto end{path.find('/', start)};
            const auto stop{end == std::string_view::npos ? path.size() : end};
            if (stop > start) {
                result.push_back(path.substr(start, stop - start));
            }
            if (end == std::string_view::npos) {
                break;
            }
            start = end + 1;
        }
        return result;
    }

    static std::string normalize_path(std::string_view path) {
        if (path.empty()) {
            return "/";
        }
        std::string result{path};
        if (!result.starts_with('/')) {
            result.insert(result.begin(), '/');
        }
        while (result.size() > 1 && result.ends_with('/')) {
            result.pop_back();
        }
        return result;
    }

    static std::string strip_index_suffix(std::string_view normalized) {
        std::string path{normalized};
        while (path.ends_with("/index")) {
            path.resize(path.size() - std::string_view{"/index"}.size());
            if (path.empty()) {
                path = "/";
                break;
            }
        }
        return path;
    }

    static std::pair<std::string_view, std::string_view> split_query(std::string_view path) {
        const auto question{path.find('?')};
        if (question == std::string_view::npos) {
            return {path, {}};
        }
        return {path.substr(0, question), path.substr(question + 1)};
    }

    static int hex_value(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    static std::string percent_decode(std::string_view input) {
        std::string result;
        result.reserve(input.size());
        for (std::size_t i{0}; i < input.size(); ++i) {
            if (input[i] == '%' && i + 2 < input.size()) {
                const int high{hex_value(input[i + 1])};
                const int low{hex_value(input[i + 2])};
                if (high >= 0 && low >= 0) {
                    result.push_back(static_cast<char>((high << 4) | low));
                    i += 2;
                    continue;
                }
            }
            result.push_back(input[i]);
        }
        return result;
    }

    static std::map<std::string, std::string, std::less<>> parse_query(std::string_view query) {
        std::map<std::string, std::string, std::less<>> result;
        std::size_t start{0};
        while (start <= query.size()) {
            const auto end{query.find('&', start)};
            const auto part{query.substr(start, end == std::string_view::npos ? query.size() - start : end - start)};
            if (!part.empty()) {
                const auto equal{part.find('=')};
                const auto key{part.substr(0, equal)};
                const auto value{equal == std::string_view::npos ? std::string_view{} : part.substr(equal + 1)};
                result[percent_decode(key)] = percent_decode(value);
            }
            if (end == std::string_view::npos) break;
            start = end + 1;
        }
        return result;
    }

    static bool match_route(const Route& route, const std::vector<std::string_view>& request,
                            std::map<std::string, std::string, std::less<>>& params) {
        std::size_t requestIndex{0};
        for (const auto& pattern : route.segments) {
            if (pattern.starts_with("[[...") && pattern.ends_with("]]")) {
                const auto key{pattern.substr(5, pattern.size() - 7)};
                std::string value;
                for (std::size_t i{requestIndex}; i < request.size(); ++i) {
                    if (!value.empty()) value.push_back('/');
                    value.append(request[i]);
                }
                if (!value.empty()) params[std::string{key}] = std::move(value);
                requestIndex = request.size();
                continue;
            }
            if (pattern.starts_with("[...") && pattern.ends_with(']')) {
                if (requestIndex == request.size()) return false;
                const auto key{pattern.substr(4, pattern.size() - 5)};
                std::string value;
                for (std::size_t i{requestIndex}; i < request.size(); ++i) {
                    if (!value.empty()) value.push_back('/');
                    value.append(request[i]);
                }
                params[std::string{key}] = std::move(value);
                requestIndex = request.size();
                continue;
            }
            if (requestIndex == request.size()) return false;
            if (pattern.size() >= 2 && pattern.front() == '[' && pattern.back() == ']') {
                params[pattern.substr(1, pattern.size() - 2)] = std::string{request[requestIndex++]};
            } else if (pattern != request[requestIndex++]) {
                return false;
            }
        }
        return requestIndex == request.size();
    }
};

}
