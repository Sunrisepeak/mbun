// index.cppm — mbun.bundler.index: source-index / symbol-ref newtypes.
//
// Mechanical port of bun's `bun_ast::Index` (source-index newtype) and `Ref`
// (module+innerIndex symbol reference) as used pervasively across the bundler
// graph/linker. ref: .mbun/bun-ref/src/bundler/Graph.rs (IndexInt / Index),
// LinkerGraph.rs (StableRef), ast/*.rs. Pure value types — no allocation.
export module mbun.bundler.index;

import std;

export namespace mbun::bundler {

// `bun.ast.Index.Int` — underlying integer repr of a source index.
using IndexInt = std::uint32_t;

// `bun_ast::Index` — a source-index newtype with an `invalid` niche.
// bun uses u31 with a validity bit; here NONE == UINT32_MAX.
struct Index {
    IndexInt value { INVALID_VALUE };

    static constexpr IndexInt INVALID_VALUE { 0x7FFF'FFFFu };

    Index() = default;
    explicit constexpr Index(IndexInt v) : value { v } {}

    static constexpr Index invalid() { return Index {}; }
    static constexpr Index source(IndexInt v) { return Index { v }; }
    static constexpr Index part(IndexInt v) { return Index { v }; }

    [[nodiscard]] constexpr bool is_valid() const { return value != INVALID_VALUE; }
    [[nodiscard]] constexpr bool is_invalid() const { return value == INVALID_VALUE; }
    [[nodiscard]] constexpr IndexInt get() const { return value; }

    friend constexpr bool operator==(Index, Index) = default;
};

// `bun_ast::Ref` — a reference to a symbol: (source/module index, inner index).
// The runtime tag distinguishes source-local, alloc, and invalid refs; for the
// pure port we keep the two 31-bit fields + a validity flag.
struct Ref {
    std::uint32_t innerIndex { 0 };
    std::uint32_t sourceIndex { INVALID_SOURCE };
    std::uint8_t  tag { 0 };  // 0 = invalid, 1 = symbol, 2 = source-contents-ptr

    static constexpr std::uint32_t INVALID_SOURCE { 0x7FFF'FFFFu };

    Ref() = default;
    constexpr Ref(std::uint32_t src, std::uint32_t inner)
        : innerIndex { inner }, sourceIndex { src }, tag { 1 } {}

    [[nodiscard]] constexpr bool is_valid() const { return tag != 0; }
    [[nodiscard]] constexpr bool is_null() const { return tag == 0; }
    [[nodiscard]] constexpr bool is_source_contents_slice() const { return tag == 2; }

    friend constexpr bool operator==(const Ref&, const Ref&) = default;
};

}  // namespace mbun::bundler
