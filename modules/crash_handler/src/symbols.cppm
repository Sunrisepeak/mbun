export module mbun.crash_handler.symbols;

import std;

namespace mbun::crash_handler {

export struct Symbol {
    std::string name;
    std::string module;
    std::string file;
    std::size_t line { 0 };
};

// Seam for dladdr/DWARF/Mach-O/llvm-symbolizer backends. The platform
// implementation is intentionally DEFERRED; the report layer remains usable
// with an injected backend and has a deterministic unknown-address fallback.
export struct SymbolBackend {
    virtual ~SymbolBackend() = default;
    virtual std::optional<Symbol> resolve(std::uintptr_t address) const = 0;
};

export struct NullSymbolBackend final : SymbolBackend {
    std::optional<Symbol> resolve(std::uintptr_t) const override { return std::nullopt; }
};

} // namespace mbun::crash_handler
