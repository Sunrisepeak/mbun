// library.cppm — dynamic library and symbol descriptors.
// Native dlopen/LoadLibrary and symbol resolution are intentionally deferred.
// PORT-SOURCE: bun runtime/ffi library opening and symbol map construction.
export module mbun.ffi.library;

import std;

namespace mbun::ffi {

export enum class LibraryStatus : std::uint8_t { Deferred, Loaded, Failed };

export struct Symbol {
    std::string name;
    std::uintptr_t address{0};
};

export struct Library {
    std::string path;
    std::vector<Symbol> symbols;
    LibraryStatus status{LibraryStatus::Deferred};

    static Library deferred(std::string_view pathName) {
        return Library{std::string(pathName), {}, LibraryStatus::Deferred};
    }

    std::optional<std::uintptr_t> find_symbol(std::string_view symbolName) const noexcept {
        for (const auto& symbol : symbols) {
            if (symbol.name == symbolName) return symbol.address;
        }
        return std::nullopt;
    }
};

} // namespace mbun::ffi
