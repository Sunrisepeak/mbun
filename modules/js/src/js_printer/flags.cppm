// src/js_printer/flags.cppm — module mbun.js_printer.flags
//
// CAP-BUILD-PRINTER shard 1/4 — the small value types threaded through the
// print walk. Every one of these is consumed by shards 2/3/4 (print_expr /
// print_stmt / print_binding), which is why they live in their own leaf module
// rather than inside the Printer.
//
// Blueprint (.mbun/bun-ref/src/js_printer/lib.rs):
//   :1463  `is_identifier_or_numeric_constant_or_property_access`  (see note)
//   :1472  `PrintResult`
//   :1477  `PrintResultSuccess`
//   :1487  `ExprFlag`  + :1495 `ExprFlagSet` + :1497 constructors
//   :1521  `ClauseItemAs`
//   :1529  `IsTopLevel`
//   :1538  `TopLevelAndIsExport`
//   :1544  `TopLevel` + :1548 impl
export module mbun.js_printer.flags;

import std;

export namespace mbun::js_printer {

// ─────────────────────────────────────────────────────────────────────────────
// ExprFlag — ref lib.rs:1487
//
// bun's comment at :1482-1485 is a warning, not trivia: "do not make this a
// packed struct — stage1 compiler bug: optional-chain-with-function.js:
// Evaluation failed: TypeError: (intermediate value) is not a function". That
// was a Zig codegen bug and does not apply to MC++, but the flag set is still
// kept as a plain bitmask over a full-width integer rather than anything
// bit-packed, for the same reason it is cheap: it is passed by value through
// every `print_expr` recursion.
//
// bun uses `enumset::EnumSet<ExprFlag>`; the MC++ equivalent is a scoped enum of
// disjoint bits plus operators, which is the same machine code with no crate.
// ─────────────────────────────────────────────────────────────────────────────
enum class ExprFlag : std::uint8_t {
    None = 0,
    ForbidCall = 1u << 0,
    ForbidIn = 1u << 1,
    HasNonOptionalChainParent = 1u << 2,
    ExprResultIsUnused = 1u << 3,
    IsFollowedByOf = 1u << 4,
};

using ExprFlagSet = ExprFlag;  // ref :1495 — `EnumSet<ExprFlag>`

[[nodiscard]] constexpr ExprFlagSet operator|(ExprFlagSet a, ExprFlagSet b) {
    return static_cast<ExprFlagSet>(std::to_underlying(a) | std::to_underlying(b));
}
[[nodiscard]] constexpr ExprFlagSet operator&(ExprFlagSet a, ExprFlagSet b) {
    return static_cast<ExprFlagSet>(std::to_underlying(a) & std::to_underlying(b));
}
[[nodiscard]] constexpr ExprFlagSet operator~(ExprFlagSet a) {
    return static_cast<ExprFlagSet>(~std::to_underlying(a));
}
constexpr ExprFlagSet& operator|=(ExprFlagSet& a, ExprFlagSet b) { return a = a | b; }
constexpr ExprFlagSet& operator&=(ExprFlagSet& a, ExprFlagSet b) { return a = a & b; }

// `set.contains(flag)` — ref enumset's `EnumSet::contains`.
[[nodiscard]] constexpr bool has_flag(ExprFlagSet set, ExprFlag flag) {
    return (std::to_underlying(set) & std::to_underlying(flag)) != 0;
}

// ref lib.rs:1497-1518 — the named constructors. Kept as functions (not just
// enumerators) so call sites transcribed from the Rust read identically.
namespace expr_flag {
[[nodiscard]] constexpr ExprFlagSet none() { return ExprFlag::None; }
[[nodiscard]] constexpr ExprFlagSet forbid_call() { return ExprFlag::ForbidCall; }
[[nodiscard]] constexpr ExprFlagSet has_non_optional_chain_parent() {
    return ExprFlag::HasNonOptionalChainParent;
}
[[nodiscard]] constexpr ExprFlagSet expr_result_is_unused() { return ExprFlag::ExprResultIsUnused; }
[[nodiscard]] constexpr ExprFlagSet is_followed_by_of() { return ExprFlag::IsFollowedByOf; }
}  // namespace expr_flag

// ─────────────────────────────────────────────────────────────────────────────
// ClauseItemAs — ref lib.rs:1521. Which side of an import/export clause an
// item is being printed on; drives `print_clause_item_as` (:3077).
// ─────────────────────────────────────────────────────────────────────────────
enum class ClauseItemAs : std::uint8_t {
    Import,
    Var,
    Export,
    ExportFrom,
};

// ─────────────────────────────────────────────────────────────────────────────
// IsTopLevel / TopLevel — ref lib.rs:1529 / :1544 / :1548
//
// `VarOnly` is the middle state: still top level for the purposes of a `var`
// (which hoists out of blocks) but not for a `let`/`const`. `sub_var()` is the
// descent: entering a nested scope demotes Yes → VarOnly and keeps No at No.
// ─────────────────────────────────────────────────────────────────────────────
enum class IsTopLevel : std::uint8_t {
    Yes,
    VarOnly,
    No,
};

struct TopLevel {
    IsTopLevel isTopLevel { IsTopLevel::No };

    // ref :1549
    [[nodiscard]] static constexpr TopLevel init(IsTopLevel isTopLevel) { return TopLevel { isTopLevel }; }

    // ref :1553
    [[nodiscard]] constexpr TopLevel sub_var() const {
        if (isTopLevel == IsTopLevel::No) {
            return init(IsTopLevel::No);
        }
        return init(IsTopLevel::VarOnly);
    }

    // ref :1560
    [[nodiscard]] constexpr bool is_top_level() const { return isTopLevel != IsTopLevel::No; }
};

// ─────────────────────────────────────────────────────────────────────────────
// TopLevelAndIsExport — ref lib.rs:1538
//
// bun's comment at :1535-1536: "One shape; dead-code elimination removes the
// unused fields when MAY_HAVE_MODULE_INFO is false."
//
// DEFERRED(shard-3): `is_top_level` is `Option<analyze_transpiled_module
// ::VarKind>` in bun (lib.rs:83 `RecordKind`, the ModuleInfo record stream).
// mbun has no ModuleInfo producer yet, so the field is modelled as the optional
// it is but with the printer-local `ModuleInfoVarKind` below. Whoever ports
// `analyze_transpiled_module` should replace that alias, not this struct.
// ─────────────────────────────────────────────────────────────────────────────
enum class ModuleInfoVarKind : std::uint8_t {
    DeclaredVariable,  // ref lib.rs:85  RecordKind::DeclaredVariable
    LexicalVariable,   // ref lib.rs:87  RecordKind::LexicalVariable
};

struct TopLevelAndIsExport {
    bool isExport { false };
    std::optional<ModuleInfoVarKind> isTopLevel {};
};

// ─────────────────────────────────────────────────────────────────────────────
// PrintResult — ref lib.rs:1472 / :1477
//
// bun's `PrintResult` is `Result(PrintResultSuccess) | Err(bun_core::Error)`.
// mbun's error flow is `std::expected` (AGENTS.md 核心原则 3 — 无异常 expected
// 错误流), so the sum type is spelled as one.
//
// DEFERRED(shard-3): `source_map` is `Option<SourceMap::Chunk>`; mbun has no
// sourcemap chunk type wired into modules/js yet. Modelled as the optional it
// is, over a placeholder that the sourcemap port replaces.
// ─────────────────────────────────────────────────────────────────────────────
struct SourceMapChunkRef {
    // DEFERRED(sourcemap): stands in for bun_sourcemap::Chunk (lib.rs:1479).
    // Intentionally empty — an empty placeholder is honest; a fabricated shape
    // would be a lie the shards would build on.
};

struct PrintResultSuccess {
    std::string code;
    std::optional<SourceMapChunkRef> sourceMap {};
};

using PrintResult = std::expected<PrintResultSuccess, std::errc>;

// ─────────────────────────────────────────────────────────────────────────────
// is_identifier_or_numeric_constant_or_property_access — ref lib.rs:1463
//
// NOT ported here. It is the one function in this shard's assigned range that
// matches on `js_ast::ExprData` (`EIdentifier | EDot | EIndex`, plus `ENumber`
// when the value is infinite/NaN), so it belongs to shard 2 (print_expr)
// alongside the rest of the ExprData dispatch — porting it here would force this
// leaf module to depend on the AST and would put the AST mapping in the hands of
// the one shard that is not doing the mapping. Shard 2: the mbun.ast equivalents
// are NodeKind::{Identifier, Member, Index} and NodeKind::NumberLiteral.
// ─────────────────────────────────────────────────────────────────────────────

}  // namespace mbun::js_printer
