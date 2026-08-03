// src/js_printer/op_level.cppm — module mbun.js_printer.op_level
//
// CAP-BUILD-PRINTER shard 3/4 — shared operator-precedence vocabulary.
//
// Blueprint: .mbun/bun-ref/src/ast/op.rs:155 (`pub enum Level`) + :180-200 (its
// comparison helpers).
//
// ── Why this file exists (read before "simplifying" it into expr.cppm) ────────
// `Level` is NOT a print-shard type. In bun it lives in `src/ast/op.rs`, i.e. in
// the AST/op vocabulary, precisely because both the statement printer and the
// expression printer need it: shard 3 cannot spell `self_().print_expr(e,
// Level::Lowest, …)` without it, and shard 2 obviously cannot either.
//
// The shard contract at the top of printer_core.cppm did not home it anywhere,
// and shard 1 did not port it (flags.cppm has ExprFlag / TopLevel but no Level).
// Its natural mbun home is `mbun.js_printer.flags` (the shared-vocabulary
// module) or `mbun.ast` (matching bun's own layering) — but both are owned by
// other shards and were being edited in parallel when this landed, so it is
// parked in its own leaf module instead. It is a pure enum with no dependencies:
// folding it into flags.cppm later is a copy-paste plus a re-export, and the
// module name can stay as an alias if anything imports it directly.
//
// ⚠️ shard 2: do NOT define a second `mbun::js_printer::Level` in expr.cppm.
// Two definitions of the same class name in two modules is an ODR violation the
// moment printer.cppm imports both. Import this module instead.
export module mbun.js_printer.op_level;

import std;

export namespace mbun::js_printer {

// ─────────────────────────────────────────────────────────────────────────────
// Level — ref op.rs:155
//
// Operator precedence, lowest to highest. The printer passes it *down* the
// expression walk: a node whose own precedence is below the level its parent
// demands has to parenthesise itself. Declaration order IS the semantics (bun's
// helpers compare `self as u8`), so do not reorder or assign explicit values.
// ─────────────────────────────────────────────────────────────────────────────
enum class Level : std::uint8_t {
    Lowest,
    Comma,
    Spread,
    Yield,
    Assign,
    Conditional,
    NullishCoalescing,
    LogicalOr,
    LogicalAnd,
    BitwiseOr,
    BitwiseXor,
    BitwiseAnd,
    Equals,
    Compare,
    Shift,
    Add,
    Multiply,
    Exponentiation,
    Prefix,
    Postfix,
    New,
    Call,
    Member,
};

// ref op.rs:181-201 — `lt` / `gt` / `gte` / `lte` / `eql`.
//
// Deliberately NOT ported. bun spells these as methods because Rust refuses to
// order a `#[repr(u8)]` enum for you; C++ scoped enums have no such gap — the
// built-in relational operators already compare two `Level`s by underlying
// value. So `s.lt(Level::Prefix)` is just `s < Level::Prefix`, and adding an
// `operator<=>` here would be a redundant overload competing with the built-in.
// Call sites read the C++ way; the semantics are identical.

}  // namespace mbun::js_printer
