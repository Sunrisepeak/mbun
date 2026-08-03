// standalone_graph.cppm — pure path contract for Bun's embedded module graph.
//
// ref: bun-ref/src/standalone_graph/StandaloneModuleGraph.rs and
// bun-zig-src/src/standalone_graph/StandaloneModuleGraph.zig. The serialized
// graph, bytecode cache, and platform executable formats remain DEFERRED; this
// module keeps the resolver-visible virtual-path contract allocation-free.
export module mbun.bundler.standalone_graph;

import std;

export namespace mbun::bundler::standalone_graph {

enum class OperatingSystem : std::uint8_t { Posix, Windows };

inline constexpr std::string_view POSIX_BASE_PATH { "/$bunfs/" };
inline constexpr std::string_view WINDOWS_BASE_PATH { "B:\\~BUN\\" };
inline constexpr std::string_view POSIX_PUBLIC_BASE_PATH { "/$bunfs/" };
inline constexpr std::string_view WINDOWS_PUBLIC_BASE_PATH { "B:/~BUN/" };

constexpr std::string_view base_path(OperatingSystem os) {
    return os == OperatingSystem::Windows ? WINDOWS_BASE_PATH : POSIX_BASE_PATH;
}

constexpr std::string_view public_base_path(OperatingSystem os) {
    return os == OperatingSystem::Windows ? WINDOWS_PUBLIC_BASE_PATH : POSIX_PUBLIC_BASE_PATH;
}

constexpr std::string_view public_base_path_with_root(OperatingSystem os) {
    return os == OperatingSystem::Windows ? "B:/~BUN/root/" : "/$bunfs/root/";
}

constexpr bool is_canonical_path(std::string_view path, OperatingSystem os) {
    return path.starts_with(base_path(os))
        || (os == OperatingSystem::Windows && path.starts_with(public_base_path(os)));
}

constexpr bool is_path(std::string_view path, OperatingSystem os) {
    if (is_canonical_path(path, os)) {
        return true;
    }
    // Windows callers can provide an NT device prefix. Bun strips it before
    // testing the virtual prefix; keep this branch bounded and zero-copy.
    if (os == OperatingSystem::Windows && path.starts_with("\\\\?\\")) {
        return is_canonical_path(path.substr(4), os);
    }
    return false;
}

} // namespace mbun::bundler::standalone_graph
