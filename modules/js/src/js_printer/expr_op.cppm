// src/js_printer/expr_op.cppm — module mbun.js_printer.expr_op
//
// CAP-BUILD-PRINTER shard 2/4 (part 1 of 2) — the operator/precedence model.
//
// Blueprint: .mbun/bun-ref/src/ast/op.rs (374 lines), ported 1:1.
//   :155  `pub enum Level`            :180-245 its `lt/gt/gte/lte/eql/sub/add_f`
//   :10   `pub enum Code`             (the operator universe)
//   :113  `unary_assign_target`       :119 `is_left_associative`
//   :125  `is_right_associative`      :131 `binary_assign_target`
//   :146  `is_prefix`
//   :248  `pub struct Op`             (text / level / is_keyword)
//   :301  `pub static TABLE`          — `.rodata` [Op; Code::COUNT]
//
// This is split out of expr.cppm for two reasons: the 2000-line cap (AGENTS.md
// 规则 10), and because it is pure data + pure functions with no dependency on
// the Printer, the AST walk, or the CRTP chain. It is the leaf every other part
// of shard 2 leans on.
//
// ═════════════════════════════════════════════════════════════════════════════
// ⚠️ BLUEPRINT DIVERGENCE — the `aux` seam (read before touching `to_op_code`)
// ═════════════════════════════════════════════════════════════════════════════
// bun's AST stores a resolved `Op::Code` on `E::Binary`/`E::Unary` (ast/e.rs) —
// the parser has already collapsed `Token` → `Code`. mbun's parser does NOT: it
// stores the *raw lexer Token* in `Node::aux`:
//
//     js_parser.cppm:3232  arena_.at(n).aux = static_cast<std::uint32_t>(op);  // Binary/Logical
//     js_parser.cppm:3317  arena_.at(n).aux = static_cast<std::uint32_t>(k);   // Unary
//     js_parser.cppm:3332  arena_.at(n).aux = static_cast<std::uint32_t>(k);   // Update (prefix)
//     js_parser.cppm:3351  arena_.at(n).aux = static_cast<std::uint32_t>(op);  // Update (postfix)
//     js_parser.cppm:3071  arena_.at(n).aux = static_cast<std::uint32_t>(op);  // Assignment
//
// So the `Code` universe is ported verbatim (it is the blueprint's asset: the
// precedence table, the associativity ranges, and the `print_space_before_
// operator` adjacency rules are all expressed in terms of it), and `to_op_code`
// is the mbun-side seam that recovers what bun's parser would have handed us.
// Porting the table in Token-space instead would have thrown away the ordering
// invariants that `is_left_associative` etc. depend on (they are *range checks*
// over the `Code` discriminant — see :119/:125 — and the Token enum's order is
// unrelated).
//
// ⚠️ `await` IS LOSSY IN THE mbun AST. js_parser.cppm:3294 builds
// `NodeKind::Unary` for `await x` and never assigns `aux`, so it defaults to 0 ==
// `Token::EndOfFile`. bun models this as a *separate node* (`ExprData::EAwait`,
// lib.rs:4348) and never as a unary op. `Token::EndOfFile` is not reachable as a
// real unary operator, so the encoding is unambiguous in practice, and
// `Code::UnAwait` below is mbun-only: it exists so the Unary arm has something
// to switch on. It is NOT in the blueprint's `Code` enum and is deliberately
// placed AFTER every blueprint discriminant so that none of the range checks
// (:113/:119/:125/:131/:146) change meaning.
// ═════════════════════════════════════════════════════════════════════════════
export module mbun.js_printer.expr_op;

import std;
import mbun.js_lexer;

// `Level` is shard 3's `op_level.cppm`, NOT this file's.
//
// It was ported there first, with an explicit note (op_level.cppm:23-25) telling
// shard 2 not to define a second one: two definitions of `mbun::js_printer::
// Level` in two modules is an ODR violation the instant printer.cppm imports
// both. That call is correct and this file defers to it — bun homes `Level` in
// `src/ast/op.rs` alongside `Code` precisely because BOTH printers need it, so a
// shared leaf module is the right shape. Re-exported so importing `expr_op`
// still gets the whole operator vocabulary in one go.
export import mbun.js_printer.op_level;

export namespace mbun::js_printer {

// ── Level helpers ────────────────────────────────────────────────────────────
// op_level.cppm deliberately does not port `lt`/`gt`/`gte`/`lte`/`eql` (its note
// at :66-73: C++ scoped enums already have built-in relational operators, so
// `level.gte(X)` is just `level >= X`). Agreed — call sites here use the
// built-ins.
//
// `sub` / `add_f` (op.rs:204/:209) are a different matter: they are ARITHMETIC,
// not comparison, and C++ gives scoped enums no built-in for that. They are
// needed by `binary_check_and_prepare` (`e_level.sub(1)`, ref :1740) and the
// unary arm (`Level::Prefix.sub(1)`, ref :4419), so they are ported here.
// ⚠️ If shard 3 ever needs them too, MOVE them to op_level.cppm rather than
// duplicating — same ODR hazard `Level` itself just had.

// ref op.rs:214 `from_raw`. bun decodes by exhaustive match specifically so an
// out-of-range shift traps in release rather than fabricating an invalid
// discriminant (its comment at :215-218). The MC++ equivalent of that intent is
// a range check: `Level` has no invalid-value UB the way a Rust enum does, but a
// silently-clamped level would corrupt paren placement, which is exactly the
// class of bug that comment is guarding.
[[nodiscard]] constexpr Level level_from_raw(int n) {
    if (n < std::to_underlying(Level::Lowest) || n > std::to_underlying(Level::Member)) {
        // ref op.rs:243 `_ => panic!("invalid Op.Level")`.
        throw std::logic_error { "invalid Op.Level" };
    }
    return static_cast<Level>(static_cast<std::uint8_t>(n));
}

// ref op.rs:204 `sub` / :209 `add_f`
[[nodiscard]] constexpr Level sub(Level l, int i) { return level_from_raw(std::to_underlying(l) - i); }
[[nodiscard]] constexpr Level add_f(Level l, int i) { return level_from_raw(std::to_underlying(l) + i); }

// ─────────────────────────────────────────────────────────────────────────────
// AssignTarget — ref ast/lib.rs (`crate::AssignTarget`, imported by op.rs:4)
// ─────────────────────────────────────────────────────────────────────────────
enum class AssignTarget : std::uint8_t {
    None = 0,
    Replace = 1,
    Update = 2,
};

// ─────────────────────────────────────────────────────────────────────────────
// Code — ref op.rs:10
//
// ⚠️ ORDER IS THE API. Four of the five classifier functions below are range
// checks over this discriminant (op.rs:113/:119/:125/:131/:146), e.g.
// `is_right_associative` is literally `code >= BinAssign || code == BinPow`.
// Reordering or inserting into the middle silently changes associativity for
// unrelated operators. New entries go at the end (see `UnAwait`).
// ─────────────────────────────────────────────────────────────────────────────
enum class Code : std::uint8_t {
    // Prefix — ref op.rs:12-18
    UnPos,     // +expr
    UnNeg,     // -expr
    UnCpl,     // ~expr
    UnNot,     // !expr
    UnVoid,
    UnTypeof,
    UnDelete,

    // Prefix update — ref op.rs:21-22
    UnPreDec,
    UnPreInc,

    // Postfix update — ref op.rs:25-26
    UnPostDec,
    UnPostInc,

    // Left-associative — ref op.rs:28-75
    BinAdd,
    BinSub,
    BinMul,
    BinDiv,
    BinRem,
    BinPow,
    BinLt,
    BinLe,
    BinGt,
    BinGe,
    BinIn,
    BinInstanceof,
    BinShl,
    BinShr,
    BinUShr,
    BinLooseEq,
    BinLooseNe,
    BinStrictEq,
    BinStrictNe,
    BinNullishCoalescing,
    BinLogicalOr,
    BinLogicalAnd,
    BinBitwiseOr,
    BinBitwiseAnd,
    BinBitwiseXor,

    // Non-associative — ref op.rs:77-78
    BinComma,

    // Right-associative — ref op.rs:80-108
    BinAssign,
    BinAddAssign,
    BinSubAssign,
    BinMulAssign,
    BinDivAssign,
    BinRemAssign,
    BinPowAssign,
    BinShlAssign,
    BinShrAssign,
    BinUShrAssign,
    BinBitwiseOrAssign,
    BinBitwiseAndAssign,
    BinBitwiseXorAssign,
    BinNullishCoalescingAssign,
    BinLogicalOrAssign,
    BinLogicalAndAssign,

    // ── mbun-only, NOT in the blueprint's `Code` ──────────────────────────────
    // See the divergence note at the top of this file. bun has `ExprData::EAwait`
    // (lib.rs:4348) as its own node; mbun folds `await` into `NodeKind::Unary`
    // with `aux` left at 0. Placed last so every range check above is unaffected:
    // `is_right_associative` (`>= BinAssign`) would otherwise claim it.
    UnAwait,

    // Sentinel — the `Code::COUNT` of op.rs:280 (`<Code as Enum>::LENGTH`).
    COUNT,
};

// ─────────────────────────────────────────────────────────────────────────────
// Classifiers — ref op.rs:112-150
//
// ⚠️ `UnAwait` is excluded from all of them by construction: it sorts after
// every blueprint discriminant, so `< BinComma`, `>= BinAssign`, `< UnPostDec`
// need explicit guarding only where the "greater-than" direction is used.
// ─────────────────────────────────────────────────────────────────────────────

// ref op.rs:113
[[nodiscard]] constexpr AssignTarget unary_assign_target(Code code) {
    if (std::to_underlying(code) >= std::to_underlying(Code::UnPreDec)
        && std::to_underlying(code) <= std::to_underlying(Code::UnPostInc)) {
        return AssignTarget::Update;
    }
    return AssignTarget::None;
}

// ref op.rs:119
[[nodiscard]] constexpr bool is_left_associative(Code code) {
    return std::to_underlying(code) >= std::to_underlying(Code::BinAdd)
        && std::to_underlying(code) < std::to_underlying(Code::BinComma)
        && code != Code::BinPow;
}

// ref op.rs:125. The `>= BinAssign` half would swallow the mbun-only `UnAwait`
// (which sorts after the assignment block), so it is excluded explicitly —
// `await` is a prefix operator and is neither left- nor right-associative in the
// sense this predicate means.
[[nodiscard]] constexpr bool is_right_associative(Code code) {
    if (code == Code::UnAwait || code == Code::COUNT) {
        return false;
    }
    return std::to_underlying(code) >= std::to_underlying(Code::BinAssign) || code == Code::BinPow;
}

// ref op.rs:131
[[nodiscard]] constexpr AssignTarget binary_assign_target(Code code) {
    if (code == Code::BinAssign) {
        return AssignTarget::Replace;
    }
    if (code == Code::UnAwait || code == Code::COUNT) {
        return AssignTarget::None;
    }
    if (std::to_underlying(code) > std::to_underlying(Code::BinAssign)) {
        return AssignTarget::Update;
    }
    return AssignTarget::None;
}

// ref op.rs:146. `await` is a prefix operator, hence the explicit inclusion.
[[nodiscard]] constexpr bool is_prefix(Code code) {
    return code == Code::UnAwait || std::to_underlying(code) < std::to_underlying(Code::UnPostDec);
}

// ─────────────────────────────────────────────────────────────────────────────
// Op / TABLE — ref op.rs:248 / :301
//
// bun builds TABLE at const-eval time so it lands in `.rodata` with zero startup
// cost (its comment at :299-300). `constexpr` + `inline constexpr` is the exact
// MC++ equivalent. Indexed by the `Code` discriminant.
// ─────────────────────────────────────────────────────────────────────────────
struct Op {
    std::string_view text {};                // ref op.rs:249
    Level level { Level::Lowest };           // ref op.rs:250
    bool isKeyword { false };                // ref op.rs:251

    // ref op.rs:266
    [[nodiscard]] static constexpr Op init(std::string_view text, Level level, bool isKeyword) {
        return Op { text, level, isKeyword };
    }
};

// ref op.rs:301. Built with an immediately-invoked constexpr lambda so the array
// is initialised by index the way bun writes it (`t[Code::X as usize] = ...`)
// rather than by a positional initialiser list, where a single misplaced comma
// would silently shift every operator's text by one.
inline constexpr std::array<Op, std::to_underlying(Code::COUNT)> TABLE { [] constexpr {
    constexpr Op NIL { Op::init("", Level::Lowest, false) };  // ref op.rs:302
    std::array<Op, std::to_underlying(Code::COUNT)> t {};
    t.fill(NIL);

    auto set { [&t](Code c, std::string_view text, Level level, bool isKeyword) constexpr {
        t[std::to_underlying(c)] = Op::init(text, level, isKeyword);
    } };

    // Prefix — ref op.rs:306-312
    set(Code::UnPos, "+", Level::Prefix, false);
    set(Code::UnNeg, "-", Level::Prefix, false);
    set(Code::UnCpl, "~", Level::Prefix, false);
    set(Code::UnNot, "!", Level::Prefix, false);
    set(Code::UnVoid, "void", Level::Prefix, true);
    set(Code::UnTypeof, "typeof", Level::Prefix, true);
    set(Code::UnDelete, "delete", Level::Prefix, true);

    // Prefix update — ref op.rs:315-316
    set(Code::UnPreDec, "--", Level::Prefix, false);
    set(Code::UnPreInc, "++", Level::Prefix, false);

    // Postfix update — ref op.rs:319-320
    set(Code::UnPostDec, "--", Level::Postfix, false);
    set(Code::UnPostInc, "++", Level::Postfix, false);

    // Left-associative — ref op.rs:323-347
    set(Code::BinAdd, "+", Level::Add, false);
    set(Code::BinSub, "-", Level::Add, false);
    set(Code::BinMul, "*", Level::Multiply, false);
    set(Code::BinDiv, "/", Level::Multiply, false);
    set(Code::BinRem, "%", Level::Multiply, false);
    set(Code::BinPow, "**", Level::Exponentiation, false);
    set(Code::BinLt, "<", Level::Compare, false);
    set(Code::BinLe, "<=", Level::Compare, false);
    set(Code::BinGt, ">", Level::Compare, false);
    set(Code::BinGe, ">=", Level::Compare, false);
    set(Code::BinIn, "in", Level::Compare, true);
    set(Code::BinInstanceof, "instanceof", Level::Compare, true);
    set(Code::BinShl, "<<", Level::Shift, false);
    set(Code::BinShr, ">>", Level::Shift, false);
    set(Code::BinUShr, ">>>", Level::Shift, false);
    set(Code::BinLooseEq, "==", Level::Equals, false);
    set(Code::BinLooseNe, "!=", Level::Equals, false);
    set(Code::BinStrictEq, "===", Level::Equals, false);
    set(Code::BinStrictNe, "!==", Level::Equals, false);
    set(Code::BinNullishCoalescing, "??", Level::NullishCoalescing, false);
    set(Code::BinLogicalOr, "||", Level::LogicalOr, false);
    set(Code::BinLogicalAnd, "&&", Level::LogicalAnd, false);
    set(Code::BinBitwiseOr, "|", Level::BitwiseOr, false);
    set(Code::BinBitwiseAnd, "&", Level::BitwiseAnd, false);
    set(Code::BinBitwiseXor, "^", Level::BitwiseXor, false);

    // Non-associative — ref op.rs:350
    set(Code::BinComma, ",", Level::Comma, false);

    // Right-associative — ref op.rs:353-368
    set(Code::BinAssign, "=", Level::Assign, false);
    set(Code::BinAddAssign, "+=", Level::Assign, false);
    set(Code::BinSubAssign, "-=", Level::Assign, false);
    set(Code::BinMulAssign, "*=", Level::Assign, false);
    set(Code::BinDivAssign, "/=", Level::Assign, false);
    set(Code::BinRemAssign, "%=", Level::Assign, false);
    set(Code::BinPowAssign, "**=", Level::Assign, false);
    set(Code::BinShlAssign, "<<=", Level::Assign, false);
    set(Code::BinShrAssign, ">>=", Level::Assign, false);
    set(Code::BinUShrAssign, ">>>=", Level::Assign, false);
    set(Code::BinBitwiseOrAssign, "|=", Level::Assign, false);
    set(Code::BinBitwiseAndAssign, "&=", Level::Assign, false);
    set(Code::BinBitwiseXorAssign, "^=", Level::Assign, false);
    set(Code::BinNullishCoalescingAssign, "??=", Level::Assign, false);
    set(Code::BinLogicalOrAssign, "||=", Level::Assign, false);
    set(Code::BinLogicalAndAssign, "&&=", Level::Assign, false);

    // mbun-only. `await` binds like a prefix operator (Level::Prefix) and is a
    // keyword, so it takes the `is_keyword` path in the EUnary arm (lib.rs:4398)
    // — which is what produces `await x` rather than `awaitx`.
    set(Code::UnAwait, "await", Level::Prefix, true);

    return t;
}() };

// ref op.rs:285 `Table::get`
[[nodiscard]] constexpr Op op_info(Code code) { return TABLE[std::to_underlying(code)]; }

// ─────────────────────────────────────────────────────────────────────────────
// to_op_code — the mbun seam. NOT in the blueprint (bun's parser resolves this).
//
// Recovers the `Op::Code` that bun's parser would have stored, from the raw
// `js_lexer::Token` that mbun's parser stores in `Node::aux`. `kind` disambiguates
// the tokens that mean different operators in different node positions:
//   * `+` / `-` are `BinAdd`/`BinSub` on Binary but `UnPos`/`UnNeg` on Unary.
//   * `++` / `--` are prefix or postfix depending on `Node::flags` bit 0
//     (js_parser.cppm:3334 sets it for prefix; the postfix site at :3351 does not).
// ─────────────────────────────────────────────────────────────────────────────
enum class OpPosition : std::uint8_t {
    Binary,       // NodeKind::Binary / Logical / Assignment
    Unary,        // NodeKind::Unary
    UpdatePre,    // NodeKind::Update with flags bit0 set
    UpdatePost,   // NodeKind::Update without flags bit0
};

[[nodiscard]] constexpr std::optional<Code> to_op_code(mbun::js_lexer::Token t, OpPosition pos) {
    using T = mbun::js_lexer::Token;

    switch (pos) {
    case OpPosition::Unary:
        switch (t) {
        // ⚠️ `aux` defaults to 0 == Token::EndOfFile for `await` — the parser
        // never assigns it (js_parser.cppm:3294). See the file-header note.
        case T::EndOfFile: return Code::UnAwait;
        case T::Plus: return Code::UnPos;
        case T::Minus: return Code::UnNeg;
        case T::Tilde: return Code::UnCpl;
        case T::Exclamation: return Code::UnNot;
        case T::Void: return Code::UnVoid;
        case T::Typeof: return Code::UnTypeof;
        case T::Delete: return Code::UnDelete;
        default: return std::nullopt;
        }

    case OpPosition::UpdatePre:
        switch (t) {
        case T::PlusPlus: return Code::UnPreInc;
        case T::MinusMinus: return Code::UnPreDec;
        default: return std::nullopt;
        }

    case OpPosition::UpdatePost:
        switch (t) {
        case T::PlusPlus: return Code::UnPostInc;
        case T::MinusMinus: return Code::UnPostDec;
        default: return std::nullopt;
        }

    case OpPosition::Binary:
        switch (t) {
        case T::Plus: return Code::BinAdd;
        case T::Minus: return Code::BinSub;
        case T::Asterisk: return Code::BinMul;
        case T::Slash: return Code::BinDiv;
        case T::Percent: return Code::BinRem;
        case T::AsteriskAsterisk: return Code::BinPow;
        case T::LessThan: return Code::BinLt;
        case T::LessThanEquals: return Code::BinLe;
        case T::GreaterThan: return Code::BinGt;
        case T::GreaterThanEquals: return Code::BinGe;
        case T::In: return Code::BinIn;
        case T::Instanceof: return Code::BinInstanceof;
        case T::LessThanLessThan: return Code::BinShl;
        case T::GreaterThanGreaterThan: return Code::BinShr;
        case T::GreaterThanGreaterThanGreaterThan: return Code::BinUShr;
        case T::EqualsEquals: return Code::BinLooseEq;
        case T::ExclamationEquals: return Code::BinLooseNe;
        case T::EqualsEqualsEquals: return Code::BinStrictEq;
        case T::ExclamationEqualsEquals: return Code::BinStrictNe;
        case T::QuestionQuestion: return Code::BinNullishCoalescing;
        case T::BarBar: return Code::BinLogicalOr;
        case T::AmpersandAmpersand: return Code::BinLogicalAnd;
        case T::Bar: return Code::BinBitwiseOr;
        case T::Ampersand: return Code::BinBitwiseAnd;
        case T::Caret: return Code::BinBitwiseXor;
        case T::Comma: return Code::BinComma;
        case T::Equals: return Code::BinAssign;
        case T::PlusEquals: return Code::BinAddAssign;
        case T::MinusEquals: return Code::BinSubAssign;
        case T::AsteriskEquals: return Code::BinMulAssign;
        case T::SlashEquals: return Code::BinDivAssign;
        case T::PercentEquals: return Code::BinRemAssign;
        case T::AsteriskAsteriskEquals: return Code::BinPowAssign;
        case T::LessThanLessThanEquals: return Code::BinShlAssign;
        case T::GreaterThanGreaterThanEquals: return Code::BinShrAssign;
        case T::GreaterThanGreaterThanGreaterThanEquals: return Code::BinUShrAssign;
        case T::BarEquals: return Code::BinBitwiseOrAssign;
        case T::AmpersandEquals: return Code::BinBitwiseAndAssign;
        case T::CaretEquals: return Code::BinBitwiseXorAssign;
        case T::QuestionQuestionEquals: return Code::BinNullishCoalescingAssign;
        case T::BarBarEquals: return Code::BinLogicalOrAssign;
        case T::AmpersandAmpersandEquals: return Code::BinLogicalAndAssign;
        default: return std::nullopt;
        }
    }
    return std::nullopt;
}

// ─────────────────────────────────────────────────────────────────────────────
// Table integrity — these are the invariants the range-check classifiers above
// silently depend on. If someone reorders `Code`, this is what fails first, at
// compile time, instead of a paren going missing at run time.
// ─────────────────────────────────────────────────────────────────────────────
static_assert(std::to_underlying(Code::UnPos) == 0, "op.rs:12 — UnPos leads the enum");
static_assert(is_left_associative(Code::BinAdd) && is_left_associative(Code::BinBitwiseXor),
    "op.rs:119 — the left-associative range is [BinAdd, BinComma)");
static_assert(!is_left_associative(Code::BinPow), "op.rs:122 — ** is right-associative");
static_assert(is_right_associative(Code::BinPow) && is_right_associative(Code::BinAssign),
    "op.rs:125");
static_assert(!is_left_associative(Code::BinComma), "op.rs:119 — comma is non-associative");
static_assert(!is_right_associative(Code::UnAwait), "mbun-only UnAwait must escape the >= BinAssign range");
static_assert(is_prefix(Code::UnPos) && is_prefix(Code::UnPreInc) && is_prefix(Code::UnAwait),
    "op.rs:146");
static_assert(!is_prefix(Code::UnPostInc) && !is_prefix(Code::BinAdd), "op.rs:146");
static_assert(unary_assign_target(Code::UnPreDec) == AssignTarget::Update, "op.rs:113");
static_assert(unary_assign_target(Code::UnNot) == AssignTarget::None, "op.rs:113");
static_assert(binary_assign_target(Code::BinAssign) == AssignTarget::Replace, "op.rs:131");
static_assert(binary_assign_target(Code::BinAddAssign) == AssignTarget::Update, "op.rs:131");
static_assert(binary_assign_target(Code::BinAdd) == AssignTarget::None, "op.rs:131");
static_assert(op_info(Code::BinPow).text == "**" && op_info(Code::BinPow).level == Level::Exponentiation,
    "op.rs:328 — table indexed by discriminant");
static_assert(op_info(Code::BinUShrAssign).text == ">>>=", "op.rs:362 — the longest operator");
static_assert(op_info(Code::BinInstanceof).isKeyword && !op_info(Code::BinLt).isKeyword, "op.rs:334");

}  // namespace mbun::js_printer
