// package_manager/update_request.cppm — mbun.install.package_manager.update_request
//
// Mechanical port of bun src/install/PackageManager/UpdateRequest.rs: the CLI
// positional parser for `bun add/remove/update/link/unlink <pkg[@version]>`.
//
// Mapping notes:
//   * The reference anchors CLI bytes in a process-lifetime static
//     (`anchor_cli_bytes`) because `version_buf` may later be repointed at
//     lockfile buffers. Here every UpdateRequest owns its input buffer as a
//     heap `std::unique_ptr<std::string>` (stable address across vector
//     moves); `version` holds string_views into that buffer.
//   * `e_string` (AST pointer for package.json editing) is out of scope
//     (PackageJSONEditor is an execution-layer seam).
//   * `bun_core::strings::is_npm_package_name` is ported here verbatim.
export module mbun.install.package_manager.update_request;

import std;
import mbun.install.dependency;
import mbun.install.npm.registry;
import mbun.install.package_manager.options;

namespace mbun::install::package_manager {

namespace dep = mbun::install::dependency;
namespace registry = mbun::install::npm::registry;

export constexpr std::uint32_t INVALID_PACKAGE_ID{0xFFFF'FFFFU};

// Port of bun_core::strings::is_npm_package_name (+ the 214-char cap).
export bool is_npm_package_name(std::string_view target) {
    if (target.size() > 214 || target.empty()) {
        return false;
    }
    bool scoped{};
    char c0{target[0]};
    if ((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z') || (c0 >= '0' && c0 <= '9') ||
        c0 == '$' || c0 == '-') {
        scoped = false;
    } else if (c0 == '@') {
        scoped = true;
    } else {
        return false;
    }
    std::size_t slash_index{0};
    for (std::size_t i{1}; i < target.size(); ++i) {
        char c{target[i]};
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.') {
            continue;
        }
        if (c == '/') {
            if (!scoped || slash_index > 0) {
                return false;
            }
            slash_index = i;
            continue;
        }
        // issue#7045, package "@~3/svelte_mount": encodeURIComponent passes
        // ! ~ * ' ( ) through — allowed in the scope segment only.
        if (c == '!' || c == '~' || c == '*' || c == '\'' || c == '(' || c == ')') {
            if (!scoped || slash_index > 0) {
                return false;
            }
            continue;
        }
        return false;
    }
    return !scoped || (slash_index > 0 && slash_index + 1 < target.size());
}

export struct UpdateRequest {
    std::string name;  // set only when aliased
    std::uint64_t name_hash{0};
    dep::Version version{};
    // Owned backing buffer for `version` views; heap so the address is stable
    // across vector reallocation (the reference uses a leaked 'static slice).
    std::unique_ptr<std::string> version_buf;
    std::uint32_t package_id{INVALID_PACKAGE_ID};
    bool is_aliased{false};
    bool failed{false};

    std::string_view buf() const {
        return version_buf ? std::string_view{*version_buf} : std::string_view{};
    }

    // Port of get_name: alias name, else the raw specifier literal.
    std::string_view get_name() const {
        return is_aliased ? std::string_view{name} : version.literal;
    }

    // Port of matches: an unnamed request (bare specifier) matches on the
    // literal's hash; a named one on the dependency's name hash.
    bool matches(std::uint64_t dependency_name_hash,
                 std::string_view dependency_version_literal) const {
        return name_hash == (name.empty() ? registry::string_hash(dependency_version_literal)
                                          : dependency_name_hash);
    }
};

export enum class UpdateRequestError : std::uint8_t {
    UnrecognizedDependencyFormat,
};

namespace detail {

inline std::string_view trim(std::string_view s) {
    std::size_t b{0};
    std::size_t e{s.size()};
    auto is_ws{[](char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }};
    while (b < e && is_ws(s[b])) {
        ++b;
    }
    while (e > b && is_ws(s[e - 1])) {
        --e;
    }
    return s.substr(b, e - b);
}

}  // namespace detail

// Port of UpdateRequest::parse_with_error. Returns the accumulated requests or
// the first unrecognised-format error (fatal=true in the reference crashes;
// here the caller decides).
export std::expected<std::vector<UpdateRequest>, UpdateRequestError> parse_update_requests(
    std::span<const std::string_view> positionals, Subcommand subcommand) {
    std::vector<UpdateRequest> update_requests;

    for (std::string_view positional : positionals) {
        std::string input{detail::trim(positional)};
        {
            // Replacing "\\\\" (2 bytes) with "/" (1 byte) never grows the
            // string; then normalize remaining backslashes to posix.
            std::string replaced;
            replaced.reserve(input.size());
            for (std::size_t i{0}; i < input.size();) {
                if (i + 1 < input.size() && input[i] == '\\' && input[i + 1] == '\\') {
                    replaced.push_back('/');
                    i += 2;
                } else if (input[i] == '\\') {
                    replaced.push_back('/');
                    ++i;
                } else {
                    replaced.push_back(input[i]);
                    ++i;
                }
            }
            input = std::move(replaced);
        }
        if (subcommand == Subcommand::Link || subcommand == Subcommand::Unlink) {
            if (!input.starts_with("link:")) {
                // "{name}@link:{name}"
                std::string buf;
                buf.reserve(input.size() * 2 + 6);
                buf.append(input).append("@link:").append(input);
                input = std::move(buf);
            }
        }

        // Own the buffer on the heap so Version's views stay valid across
        // vector moves.
        auto anchored{std::make_unique<std::string>(std::move(input))};
        std::string_view in{*anchored};

        std::string_view value{in};
        std::optional<std::string_view> alias;
        if (!dep::is_tarball(in) && is_npm_package_name(in)) {
            alias = in;
            value = in.substr(in.size());  // empty tail
        } else if (in.size() > 1) {
            std::size_t at{in.find('@', 1)};
            if (at != std::string::npos) {
                std::string_view name{in.substr(0, at)};
                if (is_npm_package_name(name)) {
                    alias = name;
                    value = in.substr(at + 1);
                }
            }
        }

        constexpr std::string_view PLACEHOLDER{"@@@"};
        std::optional<dep::Version> version{dep::parse_with_optional_tag(
            alias ? *alias : PLACEHOLDER, value, std::nullopt)};
        if (!version) {
            return std::unexpected(UpdateRequestError::UnrecognizedDependencyFormat);
        }
        // `name@owner/repo` looks aliased but may really be a git shorthand of
        // the whole input; prefer the full-input parse when it yields git.
        if (alias && version->tag == dep::Tag::Git) {
            if (auto ver{dep::parse_with_optional_tag(PLACEHOLDER, in, std::nullopt)}) {
                alias = std::nullopt;
                version = ver;
            }
        }
        // A parse that kept the "@@@" placeholder as the package name never
        // saw a real name — unrecognised.
        bool placeholder_name{[&] {
            switch (version->tag) {
                case dep::Tag::DistTag:
                    return version->dist_tag.name == PLACEHOLDER;
                case dep::Tag::Npm:
                    return version->npm.name == PLACEHOLDER;
                default:
                    return false;
            }
        }()};
        if (placeholder_name) {
            return std::unexpected(UpdateRequestError::UnrecognizedDependencyFormat);
        }

        UpdateRequest request{};
        request.version = *version;
        request.version_buf = std::move(anchored);
        if (alias) {
            request.is_aliased = true;
            request.name = std::string{*alias};
            request.name_hash = registry::string_hash(*alias);
        } else {
            request.name_hash = registry::string_hash(request.version.literal);
        }

        // Dedup: same name hash + same name length as an earlier positional.
        bool duplicate{false};
        for (const UpdateRequest& prev : update_requests) {
            if (prev.name_hash == request.name_hash && request.name.size() == prev.name.size()) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            update_requests.push_back(std::move(request));
        }
    }

    return update_requests;
}

}  // namespace mbun::install::package_manager
