// Compact symbol table seam. The link field preserves bun's union-find model.
// ref: bun-ref/src/ast/symbol.rs Symbol and bun-zig-src/src/js_parser/ast/symbols.zig
export module mbun.codegen.symbol;

import std;

export namespace mbun::codegen {

struct SymbolId {
    static constexpr std::uint32_t INVALID { std::numeric_limits<std::uint32_t>::max() };
    std::uint32_t value { INVALID };
    [[nodiscard]] bool valid() const noexcept { return value != INVALID; }
    friend bool operator==(SymbolId, SymbolId) = default;
};

enum class SymbolKind : std::uint8_t { unbound, local, imported, exported, label, private_name };

enum class SymbolFlag : std::uint8_t {
    none = 0,
    keep_name = 1 << 0,
    must_not_rename = 1 << 1,
    assigned = 1 << 2,
};

constexpr SymbolFlag operator|(SymbolFlag left, SymbolFlag right) {
    return static_cast<SymbolFlag>(static_cast<std::uint8_t>(left) |
                                   static_cast<std::uint8_t>(right));
}

struct Symbol {
    std::string original_name;
    SymbolId link;
    std::uint32_t use_count { 0 };
    std::uint32_t chunk_index { SymbolId::INVALID };
    std::uint32_t nested_scope_slot { SymbolId::INVALID };
    SymbolKind kind { SymbolKind::unbound };
    SymbolFlag flags { SymbolFlag::none };
};

class SymbolTable {
private:
    std::vector<Symbol> symbols_;

public:
    SymbolId add(std::string_view originalName, SymbolKind kind = SymbolKind::unbound) {
        SymbolId id { static_cast<std::uint32_t>(symbols_.size()) };
        symbols_.push_back(Symbol { std::string(originalName), id, 0, SymbolId::INVALID,
                                    SymbolId::INVALID, kind, SymbolFlag::none });
        return id;
    }

    [[nodiscard]] const Symbol* get(SymbolId id) const noexcept {
        return id.valid() && id.value < symbols_.size() ? &symbols_[id.value] : nullptr;
    }

    [[nodiscard]] Symbol* get(SymbolId id) noexcept {
        return id.valid() && id.value < symbols_.size() ? &symbols_[id.value] : nullptr;
    }

    SymbolId follow(SymbolId id) const noexcept {
        const Symbol* symbol { get(id) };
        while (symbol != nullptr && symbol->link != id) {
            id = symbol->link;
            symbol = get(id);
        }
        return id;
    }

    void merge(SymbolId from, SymbolId into) noexcept {
        from = follow(from);
        into = follow(into);
        if (from.valid() && into.valid() && from != into) {
            if (Symbol* symbol = get(from)) {
                symbol->link = into;
            }
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return symbols_.size(); }
};

}  // namespace mbun::codegen
