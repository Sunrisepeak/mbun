// src/js_printer/expr.cppm — module mbun.js_printer.expr
//
// CAP-BUILD-PRINTER shard 2/4 (part 2 of 2) — `print_expr` and its helpers.
//
// Blueprint (.mbun/bun-ref/src/js_printer/lib.rs):
//   :3195  `print_expr`                        — the ExprData dispatch (1330 lines)
//   :1659  `struct BinaryExpressionVisitor`    :1716 `binary_check_and_prepare`
//   :1814  `binary_visit_right_and_finish`
//   :1463  `is_identifier_or_numeric_constant_or_property_access`
//   :2130  `print_space_before_identifier`     :2143 `maybe_print_space`
//   :2156  `print_undefined`                   :2612 `print_non_negative_float`
//   :3009  `print_pure`                        :3022 `print_string_literal_utf8`
//   :4524  `print_space_before_operator`       :6838 `print_identifier`
//   :6926  `print_number`
//
// The operator/precedence half lives in `expr_op.cppm` (ref ast/op.rs) — the
// 2000-line cap plus it being pure data made the split natural. Read the shard
// contract at the top of `printer_core.cppm` before editing this file.
//
// ═════════════════════════════════════════════════════════════════════════════
// ⚠️ THE THREE SEAMS THIS SHARD SITS ON (all mbun-vs-bun AST divergences)
// ═════════════════════════════════════════════════════════════════════════════
//
// (1) OPERATORS ARE RAW LEXER TOKENS, not resolved `Op::Code`.
//     Handled by `to_op_code` in expr_op.cppm — see its header note. `await` is
//     lossy (`Unary` with `aux == 0`); `Code::UnAwait` recovers it.
//
// (2) LITERALS CARRY NO VALUE. This is the big one.
//     bun's `E::Number` holds an `f64` (:4222 calls `print_number(e.value, ..)`);
//     bun's `E::String` holds decoded UTF-8/UTF-16 (:4016). mbun's parser stores
//     NEITHER — `NodeKind::{Number,String,BigInt,RegExp,Boolean}Literal` are
//     built with `arena_.make(kind, t.start, t.end)` and nothing else
//     (js_parser.cppm:3661-3680). All that survives is the source byte range.
//
//     That is not an oversight, it is the erasure design: mbun's transpile path
//     copies original source bytes and applies `Arena::erase_slice` edits, so the
//     AST never needed decoded literal values. An AST-REBUILD printer does.
//
//     What this shard does about it: `ExprPrinter` holds a `source` view (see
//     seam 3) and re-derives what it needs from the byte range.
//       * Numbers: decoded to f64 here (`parse_number_literal_`) and fed through
//         a faithful `print_number`/`print_non_negative_float` port, so bun's
//         NORMALISATION is reproduced (`0x10` → `16`, `1000000` → `1e6`).
//       * Strings: re-quoted through `best_quote_char_for_string` + shard 1's
//         escaper, but from the RAW source bytes — so escape sequences already
//         in the source (`'\x41'`) are NOT decoded and re-encoded the way bun
//         does. ⚠️ THIS IS A KNOWN FIDELITY GAP, see `print_string_literal_raw_`.
//       * RegExp/BigInt: echoed verbatim, which is what bun does anyway
//         (:4212 `print_reg_exp_literal` prints the source text).
//     The real fix is for the parser to record decoded values on the literal
//     nodes. That is a js_parser change and is explicitly out of this shard's
//     blast radius (铁律 1).
//
// (3) THE PRINTER HAS NO SOURCE REFERENCE. bun does not need one (its AST is
//     self-contained). Because of seam (2), this one does. `source` is therefore
//     genuine mixin-owned state — the one exception the shard contract allows
//     ("your mixin has no state of its own unless it genuinely owns some",
//     printer_core.cppm:74). ⚠️ COORDINATOR: shard 3 (`print_stmt`) will need the
//     same view for the same reason; when it lands, `source` should probably be
//     hoisted into `PrinterCore` and this member deleted. Left here rather than
//     pre-emptively editing printer_core.cppm, which 铁律 1 puts off-limits.
// ═════════════════════════════════════════════════════════════════════════════
export module mbun.js_printer.expr;

import std;
import mbun.ast;
import mbun.js_lexer;
import mbun.js_printer.quote;
import mbun.js_printer.flags;
import mbun.js_printer.options;
import mbun.js_printer.expr_op;

export namespace mbun::js_printer {

// ─────────────────────────────────────────────────────────────────────────────
// is_identifier_or_numeric_constant_or_property_access — ref lib.rs:1463
//
// Assigned to this shard by flags.cppm:165-175: it is the one function in shard
// 1's range that matches on `ExprData`. bun's arms map to mbun `NodeKind` as:
//   EIdentifier → Identifier | EDot → Member | EIndex → Index
//   ENumber     → NumberLiteral, but ONLY when the value is infinite or NaN
//
// The ENumber arm is the subtle one and it is NOT arbitrary: this predicate
// guards `delete (0, x)` insertion (:4417). `Infinity`/`NaN` print as bare
// IDENTIFIERS (:6928/:6947), so `delete Infinity` would parse as deleting a
// property access — hence they must be treated as identifier-ish. Every other
// number prints as a numeric literal, which `delete` cannot meaningfully target.
//
// ⚠️ Because of seam (2), the number's value is not on the node; it is re-derived
// from source. Only `Infinity`/`NaN` matter here and neither is a NumberLiteral
// in mbun's parser (both lex as `Token::Identifier`), so the ENumber arm is
// effectively dead for mbun input today — kept for blueprint fidelity and
// because it becomes live the moment the parser constant-folds (`1e999` → inf).
// ─────────────────────────────────────────────────────────────────────────────
[[nodiscard]] inline bool is_identifier_or_numeric_constant_or_property_access(
    const mbun::ast::Arena& arena, mbun::ast::NodeIndex e, std::string_view source) {
    if (e == mbun::ast::NONE) {
        return false;
    }
    const mbun::ast::Node& n { arena.at(e) };
    switch (n.kind) {
    // ref :1466
    case mbun::ast::NodeKind::Identifier:
    case mbun::ast::NodeKind::Member:
    case mbun::ast::NodeKind::Index:
        return true;
    // ref :1467 — `e.value().is_infinite() || e.value().is_nan()`
    case mbun::ast::NodeKind::NumberLiteral: {
        const std::string_view raw { source.substr(n.start, n.end - n.start) };
        double v { 0.0 };
        const char* first { raw.data() };
        const char* last { raw.data() + raw.size() };
        const std::from_chars_result r { std::from_chars(first, last, v) };
        if (r.ec != std::errc {}) {
            return false;
        }
        return std::isinf(v) || std::isnan(v);
    }
    // ref :1468 — `_ => false`
    default:
        return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ASCII identifier classification — ref bun's `lexer::is_identifier{,_continue}`
//
// mbun's lexer HAS this logic but keeps it private (`is_ascii_id_start` /
// `is_ascii_id_continue` are private statics of `js_lexer::Lexer`, js_lexer.cppm:
// 456/459), and js_lexer.cppm is outside this shard's blast radius (铁律 1). So
// it is re-derived here from the spec rather than by reaching into the lexer.
//
// ⚠️ NON-ASCII: bun consults full Unicode ID_Start/ID_Continue tables. This
// treats every byte >= 0x80 as identifier-continue. That is deliberately
// CONSERVATIVE in the one place it is used on raw bytes
// (`print_space_before_identifier`, which only ever inspects `prev_char()`): a
// false positive emits one extra space, which is always syntactically harmless.
// A false negative would glue two tokens together and change meaning. The
// asymmetry is why the safe direction is the one taken.
// ─────────────────────────────────────────────────────────────────────────────
[[nodiscard]] constexpr bool is_ascii_id_start(std::uint8_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
}
[[nodiscard]] constexpr bool is_ascii_id_continue(std::uint8_t c) {
    return is_ascii_id_start(c) || (c >= '0' && c <= '9');
}
[[nodiscard]] constexpr bool is_identifier_continue_byte(std::uint8_t c) {
    return is_ascii_id_continue(c) || c >= 0x80;  // see the conservatism note above
}

// ref bun `lexer::is_identifier(&e.name)` as used at :3687 — "can this property
// name be printed as `.name` rather than `["name"]`".
[[nodiscard]] constexpr bool is_identifier_name(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    if (!is_ascii_id_start(static_cast<std::uint8_t>(name[0]))
        && static_cast<std::uint8_t>(name[0]) < 0x80) {
        return false;
    }
    for (const char ch : name.substr(1)) {
        const std::uint8_t c { static_cast<std::uint8_t>(ch) };
        if (!is_ascii_id_continue(c) && c < 0x80) {
            return false;
        }
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// TrackedSink — the sink capability `print_space_before_identifier` needs.
//
// bun's writer exposes `written()` / `prev_char()` / `prev_prev_char()`
// (lib.rs:7313-7315) and the Printer leans on them for token adjacency. Shard
// 1's `ByteSink` concept (quote.cppm:68) only requires `write_all`, and
// `StringSink` only adds `buffer()` — so those three are re-derived here from
// the buffer rather than by extending a concept in a file 铁律 1 puts off-limits.
//
// ⚠️ COORDINATOR: this is the natural companion to seam (3). If `source` gets
// hoisted into PrinterCore, these belong there too (bun keeps `written` as a
// counter on the writer; reading `buffer().size()` is equivalent for StringSink
// but assumes the sink is buffer-backed, which a streaming sink would not be).
// ─────────────────────────────────────────────────────────────────────────────
template <typename W>
concept TrackedSink = ByteSink<W> && requires(W& w) {
    { w.buffer() } -> std::convertible_to<const std::string&>;
};

// ─────────────────────────────────────────────────────────────────────────────
// ExprPrinter — shard 2's CRTP mixin. ref lib.rs:3195 and its helper cluster.
//
// `D` is the assembled `Printer`. Everything reached through `self_()` is either
// PrinterCore's (shard 1, landed) or another shard's (3/4, may not exist yet —
// see the `requires`-guards on the arms that need them).
// ─────────────────────────────────────────────────────────────────────────────
template <typename D>
class ExprPrinter {
private:
    [[nodiscard]] D& self_() { return static_cast<D&>(*this); }
    [[nodiscard]] const D& self_() const { return static_cast<const D&>(*this); }

public:
    // ── seam (3): mixin-owned state ──────────────────────────────────────────
    // The source text the AST's offsets index into. See seam (2)/(3) above.
    // ⚠️ The ARENA is deliberately NOT here: stmt.cppm:31-34 owns `bind_arena`/
    // `arena()`/`node()` and warns "do NOT add a second arena pointer to your own
    // mixin, or the two will drift out of sync". Reached via `self_().node(i)`.
    // `source` has no such owner yet, so it stays — see seam (3).
    std::string_view source {};

    // ref :1625 `prev_op: Op::Code`. printer_core.cppm's DEFERRED list at :274
    // hands this to shard 2 explicitly ("prev_op: Op::Code (:1625) — shard 2
    // (needs Op::Code)"), which is exactly why it lives here and not in Core:
    // Core cannot name `Code` without importing this shard's module.
    //
    // ⚠️ THE INITIALISER IS `BinAdd` AND IT IS OBSERVABLE — ref :7027
    // (`prev_op: Op::Code::BinAdd`). It is not a "no previous operator" sentinel;
    // combined with `written`/`prev_op_end` both starting at -1 (:7367/:7023) it
    // makes `print_space_before_operator` FIRE at position 0, so real bun emits a
    // LEADING SPACE for a program that starts with `+` or `++`. Verified on bun
    // 1.3.14 (transformSync, loader:"ts"):
    //     "+(+a);"  =>  " + +a;\n"     (next=UnPos,    prev=BinAdd → space)
    //     "++a;"    =>  " ++a;\n"      (next=UnPreInc, prev=BinAdd → space)
    //     "-(-a);"  =>  "- -a;\n"      (next=UnNeg,    prev=BinAdd → NO space)
    // The third case is the control: seeding `UnNeg` instead flips it to " - -a"
    // and breaks 4 vectors (verified by mutation).
    //
    // ⚠️ `UnPos` is an EQUIVALENT seed, not a wrong one: `print_space_before_
    // operator` only ever reads `prev` through `(prev == BinAdd || prev ==
    // UnPos)` (:4536), so the two are indistinguishable by construction and a
    // mutation between them is undetectable — do not add a test trying to pin
    // that down. `BinAdd` is kept because it is what the blueprint writes.
    Code prevOp { Code::BinAdd };

private:
    // ── writer introspection — ref lib.rs:7313-7315, see TrackedSink ──────────

    // ── writer introspection: DEFERRED to shard 3 (stmt.cppm) ────────────────
    // `written()` / `prev_char()` / `print_space_before_identifier()` /
    // `add_source_mapping()` / `arena()` / `node()` are all PUBLIC members of
    // `StmtPrinter`, which claims them explicitly at stmt.cppm:23-47 ("Shards
    // 2/4: call `self_().print_space_before_identifier()`. Do NOT redefine it —
    // two definitions in two mixins is an ambiguous base at assembly").
    //
    // That claim is correct and this shard defers to it. Both shards had ported
    // the same helpers independently and arrived at the same non-obvious answer
    // — `written()` is `len - 1` because bun seeds it to -1 (:7367) — so there is
    // no disagreement to resolve, only a duplicate to delete. Keeping a second
    // copy here would make every `self_().written()` ambiguous the moment
    // printer.cppm mixes both bases.
    //
    // `prev_prev_char` is the ONE piece shard 3 does not have (it has no reader),
    // so it stays here, private, until someone hoists the trio into PrinterCore.

    // ref :7405 — `self.ctx.get_last_last_byte()`. Needed ONLY by
    // `print_space_before_operator`'s `<!--` guard (:4547).
    [[nodiscard]] std::uint8_t prev_prev_char_() {
        const std::string& b { self_().writer().buffer() };
        return b.size() < 2 ? std::uint8_t { 0 } : static_cast<std::uint8_t>(b[b.size() - 2]);
    }

public:
    // ═════════════════════════════════════════════════════════════════════════
    // Spacing / adjacency helpers
    // ═════════════════════════════════════════════════════════════════════════

    // ref :2143
    void maybe_print_space() {
        switch (self_().prev_char()) {
        case 0:
        case ' ':
        case '\n':
            break;
        default:
            self_().print(" ");
            break;
        }
    }

    // ref :4524. The comment block there is the specification; it is reproduced
    // because each clause is a distinct real-world hazard:
    //   "+ + y"   => "+ +y"      "+ ++ y"  => "+ ++y"
    //   "x + + y" => "x+ +y"     "x ++ + y" => "x+++y"
    //   "-- >"    => "-- >"      (else `-->` opens an HTML-comment token)
    //   "< ! --"  => "<! --"     (else `<!--` opens an HTML-comment token)
    void print_space_before_operator(Code next) {
        if (self_().prevOpEnd == self_().written()) {
            const Code prev { prevOp };
            if (((prev == Code::BinAdd || prev == Code::UnPos)
                    && (next == Code::BinAdd || next == Code::UnPos || next == Code::UnPreInc))
                || ((prev == Code::BinSub || prev == Code::UnNeg)
                    && (next == Code::BinSub || next == Code::UnNeg || next == Code::UnPreDec))
                || (prev == Code::UnPostDec && next == Code::BinGt)
                || (prev == Code::UnNot && next == Code::UnPreDec && self_().written() > 1
                    && prev_prev_char_() == '<')) {
                self_().print(" ");
            }
        }
    }

    // ref :2150 / :4554 — the `require(..)` → `import(..).then(..)` lowering pair.
    [[nodiscard]] Level print_dot_then_prefix() {
        self_().print(".then(() => ");
        return Level::Comma;
    }
    void print_dot_then_suffix() { self_().print(")"); }

    // ref :3009
    void print_pure() { self_().print("/* @__PURE__ */ "); }

    // ═════════════════════════════════════════════════════════════════════════
    // Identifiers — ref :6838 / :6846
    // ═════════════════════════════════════════════════════════════════════════

    void print_identifier(std::string_view identifier) {
        if constexpr (D::flags().asciiOnly) {
            print_identifier_ascii_only_(identifier);
        } else {
            self_().print(identifier);
        }
    }

private:
    // ref :6846. bun's fast path is a SIMD all-ASCII scan followed by one
    // `print()`, because ~all identifiers are pure ASCII and the per-codepoint
    // loop below is only for the rare non-ASCII tail (its comment at :6847-6850).
    // `std::ranges::all_of` over the bytes is the same branch-predictable scan and
    // vectorises; going wider (SWAR/intrinsics) is not justified until a profile
    // says identifier printing is hot, and quote.cppm already owns the project's
    // one hand-rolled SWAR loop.
    void print_identifier_ascii_only_(std::string_view identifier) {
        const auto isAscii { [](char ch) { return static_cast<std::uint8_t>(ch) < 0x80; } };
        if (std::ranges::all_of(identifier, isAscii)) {
            self_().print(identifier);
            return;
        }

        // ref :6852-6879 — emit maximal ASCII runs verbatim, escape the rest as
        // `\u{...}`. Tracking a run start beats printing byte-by-byte.
        std::size_t asciiStart { 0 };
        bool isAsciiRun { false };
        std::size_t i { 0 };
        while (i < identifier.size()) {
            const std::uint8_t c { static_cast<std::uint8_t>(identifier[i]) };
            if (c < 0x80) {
                if (!isAsciiRun) {
                    asciiStart = i;
                    isAsciiRun = true;
                }
                i += 1;
                continue;
            }
            if (isAsciiRun) {
                self_().print(identifier.substr(asciiStart, i - asciiStart));
                isAsciiRun = false;
            }
            const auto [cp, len] { decode_utf8_(identifier, i) };
            self_().print("\\u{");
            // ⚠️ The cast is REQUIRED, not cosmetic — this is a GCC/libc++ split.
            // `std::format("{:x}", cp)` with a `char32_t` compiles under GCC 16 /
            // libstdc++ but is REJECTED by LLVM 22 / libc++: the standard provides
            // no `formatter<char32_t, char>` specialisation, so libc++ is right and
            // libstdc++ is the lenient outlier. AGENTS.md 规则 7 pins the language
            // to the GCC-16 ∩ latest-LLVM intersection, so the portable spelling
            // wins. Formatting the code point as a plain integer is what bun does
            // anyway (`format_args!("{:x}", cursor.c)`, :6875).
            self_().print(std::format("{:x}", static_cast<std::uint32_t>(cp)));
            self_().print("}");
            i += len;
        }
        if (isAsciiRun) {
            self_().print(identifier.substr(asciiStart));
        }
    }

    // Minimal UTF-8 decode for the escape path above. Returns (codepoint, length).
    // Malformed input yields the raw byte as a 1-byte codepoint, which round-trips
    // it as `\u{xx}` instead of throwing — the printer must never fail on input the
    // parser already accepted.
    [[nodiscard]] static std::pair<char32_t, std::size_t> decode_utf8_(
        std::string_view s, std::size_t i) {
        const std::uint8_t c0 { static_cast<std::uint8_t>(s[i]) };
        const auto cont { [&](std::size_t k) -> bool {
            return i + k < s.size() && (static_cast<std::uint8_t>(s[i + k]) & 0xC0) == 0x80;
        } };
        const auto bits { [&](std::size_t k) -> char32_t {
            return static_cast<char32_t>(static_cast<std::uint8_t>(s[i + k]) & 0x3F);
        } };
        if ((c0 & 0xE0) == 0xC0 && cont(1)) {
            return { (static_cast<char32_t>(c0 & 0x1F) << 6) | bits(1), 2 };
        }
        if ((c0 & 0xF0) == 0xE0 && cont(1) && cont(2)) {
            return { (static_cast<char32_t>(c0 & 0x0F) << 12) | (bits(1) << 6) | bits(2), 3 };
        }
        if ((c0 & 0xF8) == 0xF0 && cont(1) && cont(2) && cont(3)) {
            return { (static_cast<char32_t>(c0 & 0x07) << 18) | (bits(1) << 12) | (bits(2) << 6)
                         | bits(3),
                4 };
        }
        return { static_cast<char32_t>(c0), 1 };
    }

public:
    // ═════════════════════════════════════════════════════════════════════════
    // Numbers — ref :2612 / :6926
    //
    // These are ported faithfully even though seam (2) means the value has to be
    // re-derived from source first: they are pure `f64 -> text` and are where
    // bun's number NORMALISATION lives (`0x10` → `16`, `1000000` → `1e6`,
    // `-1` at Level::Prefix → `(-1)`). Reproducing that is most of the point of
    // an AST-rebuild printer.
    // ═════════════════════════════════════════════════════════════════════════

    // ref :2612
    void print_non_negative_float(double value) {
        // ref :2614-2617 — integer check via floor, not a cast (a cast would be UB
        // for values outside the integer range, which is exactly the case the
        // `< 2^52` guard below is filtering for).
        const double floored { std::floor(value) };
        const bool isInteger { (value - floored) == 0.0 };

        // ref :2618 — `float < (u64::MAX >> 12) as f64` i.e. maxInt(u52). Below
        // this every integer is exactly representable in an f64, so the u64 cast
        // is lossless and `itoa` is both shorter and faster than the float
        // formatter.
        constexpr double MAX_INT_U52 { static_cast<double>(std::numeric_limits<std::uint64_t>::max() >> 12) };
        if (value < MAX_INT_U52 && isInteger) {
            const std::uint64_t val { static_cast<std::uint64_t>(value) };
            // ref :2624 — `pow10_exp_1e4_to_1e9`: prefer `1e6` over `1000000`.
            if (const std::optional<int> e { pow10_exp_1e4_to_1e9_(val) }; e.has_value()) {
                self_().print("1e");
                self_().print(static_cast<char>('0' + *e));
                return;
            }
            // ref :2630 — `itoa`.
            self_().print(std::format("{}", val));
            return;
        }

        // ref :2635-2637 — Rust's `Display for f64` emits the shortest string that
        // round-trips and never uses scientific notation. `std::format("{}", d)`
        // is specified to produce the shortest round-tripping representation
        // (same Grisu/Ryu family), which is the closest MC++ equivalent.
        //
        // ⚠️ NOT byte-identical to Rust in every case: Rust's Display prints
        // `1.0` for an integral f64 that took this branch, while std::format
        // prints `1`. Unreachable for integral values (they take the branch
        // above), but a real difference if that guard ever changes.
        self_().print(std::format("{}", value));
    }

    // ref :6926
    void print_number(double value, Level level) {
        const double absValue { std::abs(value) };

        if (std::isnan(value)) {
            // ref :6928 — NaN prints as the bare identifier `NaN`.
            self_().print_space_before_identifier();
            self_().print("NaN");
            return;
        }

        if (std::isinf(value)) {
            // ref :6931-6959
            const bool isNegInf { std::signbit(value) };
            const bool wrap { ((!self_().options.hasRunSymbolRenamer || self_().options.minifySyntax)
                                  && level >= Level::Multiply)
                || (isNegInf && level >= Level::Prefix) };

            if (wrap) {
                self_().print("(");
            }
            if (isNegInf) {
                print_space_before_operator(Code::UnNeg);
                self_().print("-");
            } else {
                self_().print_space_before_identifier();
            }

            // ref :6952-6958. The comment at :6951 is the reason for the
            // renamer condition: "If we are not running the symbol renamer, we
            // must not print Infinity" — an unrenamed scope could have shadowed
            // `Infinity`, so `1 / 0` is the only safe spelling.
            if constexpr (D::flags().isJson) {
                self_().print("Infinity");
            } else if (!self_().options.minifySyntax && self_().options.hasRunSymbolRenamer) {
                self_().print("Infinity");
            } else if (self_().options.minifyWhitespace) {
                self_().print("1/0");
            } else {
                self_().print("1 / 0");
            }

            if (wrap) {
                self_().print(")");
            }
            return;
        }

        if (!std::signbit(value)) {
            // ref :6961
            self_().print_space_before_identifier();
            print_non_negative_float(absValue);
            self_().prevNumEnd = self_().written();  // ref :6964
            return;
        }

        if (level >= Level::Prefix) {
            // ref :6966-6973. The comment at :6966-6969 is load-bearing: the test
            // is `signbit` and not `value < 0` SPECIFICALLY so that `-0` wraps —
            // `-0 < 0` is false, but `(-0).toString` still needs the parens.
            self_().print("(-");
            print_non_negative_float(absValue);
            self_().print(")");
            return;
        }

        // ref :6975
        print_space_before_operator(Code::UnNeg);
        self_().print("-");
        print_non_negative_float(absValue);
        self_().prevNumEnd = self_().written();
    }

    // ref :2156
    void print_undefined(mbun::ast::NodeIndex loc, Level level) {
        if (self_().options.minifySyntax) {
            if (level >= Level::Prefix) {
                self_().add_source_mapping(loc);
                self_().print("(void 0)");
            } else {
                self_().print_space_before_identifier();
                self_().add_source_mapping(loc);
                self_().print("void 0");
            }
        } else {
            self_().print_space_before_identifier();
            self_().add_source_mapping(loc);
            self_().print("undefined");
        }
    }

private:
    // ref bun_core::fmt::pow10_exp_1e4_to_1e9 (called at :2624). Returns `e` when
    // `val == 10^e` for e in 4..=9, so `print_non_negative_float` can emit `1e6`
    // instead of `1000000`. Range starts at 1e4 because `1e3` and `1000` are both
    // 4 bytes — there is nothing to win below that.
    [[nodiscard]] static constexpr std::optional<int> pow10_exp_1e4_to_1e9_(std::uint64_t val) {
        std::uint64_t p { 10'000 };
        for (int e { 4 }; e <= 9; ++e) {
            if (val == p) {
                return e;
            }
            p *= 10;
        }
        return std::nullopt;
    }

    // ── seam (2): literal value recovery ─────────────────────────────────────

    // Decode a JS numeric literal's SOURCE TEXT to the f64 that bun's parser
    // would have stored on `E::Number`. Handles the four bases plus the `_`
    // separators the lexer accepts. Not in the blueprint — bun's parser did this
    // long before the printer ran.
    [[nodiscard]] static std::optional<double> parse_number_literal_(std::string_view raw) {
        std::string buf {};
        buf.reserve(raw.size());
        for (const char c : raw) {
            if (c != '_') {  // numeric separators: `1_000_000`
                buf.push_back(c);
            }
        }
        if (buf.empty()) {
            return std::nullopt;
        }

        // Radix-prefixed forms. bun's parser folds these to their f64 value, which
        // is why `0x10` reprints as `16`.
        int base { 10 };
        std::size_t off { 0 };
        if (buf.size() > 2 && buf[0] == '0') {
            switch (buf[1]) {
            case 'x': case 'X': base = 16; off = 2; break;
            case 'o': case 'O': base = 8;  off = 2; break;
            case 'b': case 'B': base = 2;  off = 2; break;
            default: break;
            }
        }

        if (base != 10) {
            std::uint64_t v { 0 };
            const char* first { buf.data() + off };
            const char* last { buf.data() + buf.size() };
            const std::from_chars_result r { std::from_chars(first, last, v, base) };
            if (r.ec != std::errc {} || r.ptr != last) {
                return std::nullopt;
            }
            return static_cast<double>(v);
        }

        // Legacy octal (`0755`) — no prefix, leading 0, all digits < 8. Sloppy
        // mode only; the lexer accepts it, so the printer must not choke.
        if (buf.size() > 1 && buf[0] == '0'
            && std::ranges::all_of(buf, [](char c) { return c >= '0' && c <= '7'; })) {
            std::uint64_t v { 0 };
            const char* first { buf.data() + 1 };
            const char* last { buf.data() + buf.size() };
            const std::from_chars_result r { std::from_chars(first, last, v, 8) };
            if (r.ec == std::errc {} && r.ptr == last) {
                return static_cast<double>(v);
            }
        }

        double v { 0.0 };
        const char* first { buf.data() };
        const char* last { buf.data() + buf.size() };
        const std::from_chars_result r { std::from_chars(first, last, v) };
        if (r.ec != std::errc {} || r.ptr != last) {
            return std::nullopt;
        }
        return v;
    }

    // ⚠️ FIDELITY GAP — see seam (2) in the file header.
    //
    // bun re-quotes from the DECODED string value (:4016 `best_quote_char_for_
    // e_string(e)` then `print_string_characters_*`), so `'\x41'` reprints as
    // `"A"` and the quote choice reflects the decoded content. mbun's AST has
    // only the source range, so this re-quotes from the RAW bytes BETWEEN the
    // original quotes: escape sequences are passed through untouched and the
    // quote choice is made over their source spelling.
    //
    // Consequences, stated precisely so nobody has to rediscover them:
    //   * `'a'`      → `"a"`     ✔ matches bun (quote normalisation works)
    //   * `'\x41'`   → `"\x41"`  ✘ bun prints `"A"`
    //   * `'a"b'`    → `'a"b'`   ✔ matches bun (quote choice works)
    //   * `'\n'`     → `` `\n` `` ✘ bun sees a real newline in the decoded value
    //                              and picks a backtick; we see the two bytes
    //                              `\` `n`, which need no escaping in any quote,
    //                              so we keep `"`.
    // The last one is the skeleton agent's差分 finding #3 and it is NOT reproduced
    // here — that finding is about a decoded newline. Fixing this properly needs
    // the parser to record the decoded value; it is not fixable from the printer.
    void print_string_literal_raw_(std::string_view rawWithQuotes) {
        // Strip the original delimiters. Template literals do not reach here.
        std::string_view inner { rawWithQuotes };
        if (inner.size() >= 2 && (inner.front() == '\'' || inner.front() == '"')
            && inner.back() == inner.front()) {
            inner = inner.substr(1, inner.size() - 2);
        }

        // ref :2593 `best_quote_char_for_e_string` — JSON always double-quotes.
        std::uint8_t quote { '"' };
        if constexpr (!D::flags().isJson) {
            quote = best_quote_(inner, /*allowBacktick=*/false);
        }
        self_().print(static_cast<char>(quote));
        self_().print_string_characters_utf8(inner, quote);
        self_().print(static_cast<char>(quote));
    }

    // ref :3015 `print_string_literal_e_string`. This is the arm bun actually
    // takes for `E::String`, and it is now reachable because the parser records
    // the DECODED value on the node (ast.cppm `Arena::add_string`). mbun's
    // decoded value is always UTF-16, so bun's `print_string_characters_e_string`
    // (:4559) — which picks utf16-vs-utf8 at runtime — resolves statically to its
    // utf16 branch here. `best_quote_char_for_string` costs the DECODED units, so
    // `'a\nb'` correctly prefers a backtick the way bun does.
    // ⚠️ `allow_backtick` is `true` at the call site (:4029), unlike the raw path.
    // NOT ported: bun's `prefer_template` short-circuit (:4022) — that flag marks a
    // string FOLDED from a template literal, which mbun's parser never produces
    // (templates keep their own node and print from source, seam 2).
    void print_string_literal_utf16_(std::u16string_view text, bool allowBacktick) {
        std::uint8_t quote { '"' };
        if constexpr (!D::flags().isJson) {
            quote = best_quote_char_for_string(
                std::span<const char16_t> { text.data(), text.size() }, allowBacktick);
        }
        self_().print(static_cast<char>(quote));
        self_().print_string_characters_utf16(text, quote);
        self_().print(static_cast<char>(quote));
    }

    // `best_quote_char_for_string` takes `std::span<const T>` (quote.cppm:221).
    // ⚠️ It MUST be instantiated over `std::uint8_t`, not `char`: it costs each
    // unit via `static_cast<std::uint32_t>(str[i])`, and on a platform where
    // `char` is signed every byte >= 0x80 would sign-extend to ~4 billion and
    // miss every arm of the switch. bun passes u8 slices (`slice8()`, :2597) for
    // exactly this reason.
    [[nodiscard]] static std::uint8_t best_quote_(std::string_view s, bool allowBacktick) {
        return best_quote_char_for_string(
            std::span<const std::uint8_t> {
                reinterpret_cast<const std::uint8_t*>(s.data()), s.size() },
            allowBacktick);
    }

    [[nodiscard]] std::string_view raw_(const mbun::ast::Node& n) const {
        return source.substr(n.start, n.end - n.start);
    }


public:
    // ref :3022 `print_string_literal_utf8` — used for computed property names
    // that are not valid identifiers (:3707).
    //
    // ⚠️ PUBLIC, and unsuffixed, on purpose — it is a cross-shard SEAM, not a
    // private helper. bun declares it `pub fn` (:3022) and calls it from
    // `print_binding` (:5180) as well as from `print_expr`'s `EIndex` arm
    // (:3707). binding.cppm:366 is mbun's image of the first of those.
    //
    // It was private and `_`-suffixed until now, which made
    // `Printer::print_binding` un-INSTANTIABLE: the name it calls did not exist
    // on the assembled Printer. That went unnoticed because
    // tests/test_js_printer_binding.cpp assembles its own Printer-shaped type
    // that defines a `print_string_literal_utf8` stub of its own — the tests were
    // green against the stub, never against the real class. See
    // tests/test_js_printer_real.cpp, which instantiates the REAL Printer so this
    // cannot silently regress again.
    void print_string_literal_utf8(std::string_view text, bool allowBacktick) {
        std::uint8_t quote { '"' };
        if constexpr (!D::flags().isJson) {
            quote = best_quote_(text, allowBacktick);
        }
        self_().print(static_cast<char>(quote));
        self_().print_string_characters_utf8(text, quote);
        self_().print(static_cast<char>(quote));
    }

    // ═════════════════════════════════════════════════════════════════════════
    // print_expr — ref :3195
    //
    // ⚠️ SCOPE. This lands the self-contained expression core. Arms whose bodies
    // belong to shards 3/4 (`print_stmt` for arrow/function bodies, `print_class`,
    // `print_property`) are guarded with a `requires`-expression so this module
    // COMPILES AND TESTS TODAY and lights up automatically the moment those
    // shards land — no edit to this file needed. That is the CRTP contract's
    // "a shard can call a method of a shard that does not exist yet"
    // (printer_core.cppm:56-58) made explicit; without the guard the call is only
    // an error at instantiation, which would block this shard's own tests.
    //
    // bun arms with NO mbun AST counterpart, deliberately not ported (they are
    // bundler/runtime lowering, and mbun's parser never builds them):
    //   EUndefined(:3205) ENewTarget(:3229) EImportMetaMain(:3260) ESpecial(:3312)
    //   ECommonjsExportIdentifier(:3379) ERequireMain(:3542) ERequireCallTarget(:3555)
    //   ERequireResolveCallTarget(:3568) ERequireString(:3582) ERequireResolveString(:3594)
    //   EImportIdentifier(:4247) EInlinedEnum(:4493) ENameOfSymbol(:4501)
    //   EObjectJSON(:3982) EArrayJSON(:3995) EBranchBoolean(:3999)
    // `print_undefined` IS ported (it is reachable from shard 3's `S::Return`).
    // ═════════════════════════════════════════════════════════════════════════
    void print_expr(mbun::ast::NodeIndex e, Level level, ExprFlagSet inFlags) {
        // ref :3196-3199 — bun bails on stack exhaustion rather than crashing.
        // printer_core.cppm:283 defers `stack_check`/`stack_overflowed`; the
        // binary chain (the deepest realistic nesting) is already heap-iterated
        // below, which is the specific hazard that guard exists for (:1653-1656).
        if (e == mbun::ast::NONE) {
            return;
        }

        ExprFlagSet flags { inFlags };
        const mbun::ast::Node& n { self_().node(e) };

        switch (n.kind) {
        // ref :3204 — `EMissing => {}`
        case mbun::ast::NodeKind::Missing:
            break;

        // ref :3209
        case mbun::ast::NodeKind::SuperExpr:
            self_().print_space_before_identifier();
            self_().add_source_mapping(e);
            self_().print("super");
            break;

        // ref :3214
        case mbun::ast::NodeKind::NullLiteral:
            self_().print_space_before_identifier();
            self_().add_source_mapping(e);
            self_().print("null");
            break;

        // ref :3219
        case mbun::ast::NodeKind::ThisExpr:
            self_().print_space_before_identifier();
            self_().add_source_mapping(e);
            self_().print("this");
            break;

        // ref :3224
        case mbun::ast::NodeKind::SpreadElement:
            self_().add_source_mapping(e);
            self_().print("...");
            print_expr(n.a, Level::Comma, expr_flag::none());
            break;

        // ref :3234. bun's arm handles the bundler's `import.meta` rewrites
        // (hmr_ref / InternalBakeDev); mbun's transpile path has none of that
        // machinery, so this is the plain spelling only.
        case mbun::ast::NodeKind::ImportMeta:
            self_().print_space_before_identifier();
            self_().add_source_mapping(e);
            self_().print("import.meta");
            break;

        // ref :3999 — `EBoolean`
        case mbun::ast::NodeKind::BooleanLiteral: {
            const bool value { raw_(n) == "true" };
            // ref :4000-4014 — when minifying, booleans become `!0` / `!1`.
            if (self_().options.minifySyntax) {
                if (level >= Level::Prefix) {
                    self_().print(value ? "(!0)" : "(!1)");
                } else {
                    self_().print(value ? "!0" : "!1");
                }
            } else {
                self_().print_space_before_identifier();
                self_().add_source_mapping(e);
                self_().print(value ? "true" : "false");
            }
            break;
        }

        // ref :4226 — `EIdentifier`. bun routes through the renamer
        // (`print_symbol`, :2472); mbun's AST carries the name text directly and
        // has no symbol table in the print path, so this is the whole arm.
        case mbun::ast::NodeKind::Identifier:
            self_().print_space_before_identifier();
            self_().add_source_mapping(e);
            print_identifier(n.text.empty() ? raw_(n) : n.text);
            break;

        // ref :4212 — `ERegExp`. bun prints the source text verbatim
        // (`print_reg_exp_literal`, :4593) and records `prev_reg_exp_end` so a
        // following keyword cannot fuse onto the trailing flag.
        case mbun::ast::NodeKind::RegExpLiteral:
            self_().print_space_before_identifier();
            self_().add_source_mapping(e);
            self_().print(raw_(n));
            self_().prevRegExpEnd = self_().written();
            break;

        // ref :4216 — `EBigInt`. Printed verbatim including the `n` suffix.
        case mbun::ast::NodeKind::BigIntLiteral:
            self_().print_space_before_identifier();
            self_().add_source_mapping(e);
            self_().print(raw_(n));
            break;

        // ref :4222 — `ENumber`. See seam (2): the value is re-derived here.
        case mbun::ast::NodeKind::NumberLiteral: {
            self_().add_source_mapping(e);
            const std::string_view raw { raw_(n) };
            if (const std::optional<double> v { parse_number_literal_(raw) }; v.has_value()) {
                print_number(*v, level);
            } else {
                // Unparseable is not a printer error: echo the source and keep
                // the adjacency bookkeeping bun's `print_number` would have done.
                self_().print_space_before_identifier();
                self_().print(raw);
                self_().prevNumEnd = self_().written();
            }
            break;
        }

        // ref :4016 — `EString`. The parser now records the decoded value, so the
        // faithful `print_string_literal_e_string` path (:4029) is taken. The raw
        // fallback remains for any string node built without one (e.g. a synthetic
        // node), where echoing the source bytes is still the best available answer.
        case mbun::ast::NodeKind::StringLiteral: {
            self_().add_source_mapping(e);
            if (const std::u16string* value { self_().arena().string_value(n) }) {
                print_string_literal_utf16_(*value, /*allowBacktick=*/true);
            } else {
                print_string_literal_raw_(raw_(n));
            }
            break;
        }

        // mbun-only node: bun's parser DROPS parentheses and reconstructs them
        // from precedence (which is what the whole `Level` machinery is for).
        // mbun keeps an explicit `Paren` node, so it is transparent here —
        // re-printing it literally would defeat paren minimisation and produce
        // `((a + b))`.
        case mbun::ast::NodeKind::Paren:
            print_expr(n.a, level, flags);
            break;

        // ref :4364 — `EYield`
        case mbun::ast::NodeKind::Yield: {
            const bool wrap { level >= Level::Assign };
            if (wrap) {
                self_().print("(");
            }
            self_().print_space_before_identifier();
            self_().add_source_mapping(e);
            self_().print("yield");
            const bool isDelegate { (n.flags & 1u) != 0 };  // ast.cppm:49 — aux bit0 = `*`
            if (isDelegate) {
                self_().print("*");
            }
            if (n.a != mbun::ast::NONE) {
                if (!isDelegate) {
                    // ref :4380 — `yield x` needs the space; `yield*x` does not.
                    maybe_print_space();
                }
                print_expr(n.a, Level::Yield, expr_flag::none());
            }
            if (wrap) {
                self_().print(")");
            }
            break;
        }

        // ref :3769 — `EIf` (the conditional operator)
        case mbun::ast::NodeKind::Conditional: {
            const bool wrap { level >= Level::Conditional };
            if (wrap) {
                self_().print("(");
                flags &= ~ExprFlag::ForbidIn;
            }
            print_expr(n.a, Level::Conditional, flags);
            self_().print_space();
            self_().print("?");
            self_().print_space();
            print_expr(n.b, Level::Yield, expr_flag::none());
            self_().print_space();
            self_().print(":");
            self_().print_space();
            flags |= ExprFlag::ForbidIn;
            print_expr(n.c, Level::Yield, flags);
            if (wrap) {
                self_().print(")");
            }
            break;
        }

        // ref :3663 — `EDot`.
        case mbun::ast::NodeKind::Member: {
            const bool isOptionalChain { (n.flags & mbun::ast::ocflags::IsOptionalStart) != 0 };

            // ref :3665-3681. The `wrap` here is what keeps `(a?.b).c` correct.
            // Read it as: a link OUTSIDE any chain tells its target "your parent is
            // not part of a chain"; a link INSIDE one, on hearing that from its
            // parent, must re-parenthesise itself, because the source parens that
            // originally cut the chain are gone by now (mbun's Paren node is
            // print-transparent, :883). Without the parens `(a?.b).c` would print as
            // `a?.b.c`, which SHORT-CIRCUITS instead of throwing when `a` is null.
            bool wrap { false };
            if ((n.flags & mbun::ast::ocflags::IsOptionalAny) == 0) {
                flags |= ExprFlag::HasNonOptionalChainParent;  // ref :3668
            } else {
                if ((flags & ExprFlag::HasNonOptionalChainParent) != expr_flag::none()) {
                    wrap = true;
                    self_().print("(");
                }
                flags &= ~ExprFlag::HasNonOptionalChainParent;  // ref :3684
            }
            flags &= (ExprFlag::HasNonOptionalChainParent | ExprFlag::ForbidCall);  // ref :3686
            print_expr(n.a, Level::Postfix, flags);

            if (is_identifier_name(n.text)) {
                if (isOptionalChain) {
                    self_().print("?.");  // ref :3690
                } else {
                    // ref :3691-3696 — "1.toString" is a syntax error, so print
                    // "1 .toString". This is why `prev_num_end` exists.
                    if (self_().prevNumEnd == self_().written()) {
                        self_().print(" ");
                    }
                    self_().print(".");
                }
                self_().add_source_mapping(e);
                print_identifier(n.text);
            } else {
                // ref :3702-3708
                self_().print(isOptionalChain ? "?.[" : "[");
                print_string_literal_utf8(n.text, false);
                self_().print("]");
            }
            if (wrap) {
                self_().print(")");
            }
            break;
        }

        // ref :3714 — `EIndex`
        case mbun::ast::NodeKind::Index: {
            bool wrap { false };
            if ((n.flags & mbun::ast::ocflags::IsOptionalAny) == 0) {
                flags |= ExprFlag::HasNonOptionalChainParent;  // ref :3717
            } else {
                if ((flags & ExprFlag::HasNonOptionalChainParent) != expr_flag::none()) {
                    wrap = true;
                    self_().print("(");
                }
                flags &= ~ExprFlag::HasNonOptionalChainParent;  // ref :3735
            }
            flags &= ~ExprFlag::IsFollowedByOf;  // ref :3736 — the index target is
                                                 // not directly followed by `of`
            print_expr(n.a, Level::Postfix, flags);
            if ((n.flags & mbun::ast::ocflags::IsOptionalStart) != 0) {
                self_().print("?.");  // ref :3742-3746
            }
            self_().print("[");
            self_().add_source_mapping(n.b);
            print_expr(n.b, Level::Lowest, expr_flag::none());
            self_().print("]");
            if (wrap) {
                self_().print(")");
            }
            break;
        }

        // ref :3745-3752 — `EIndex` with an `EPrivateIdentifier` index. bun reaches
        // this through the same EIndex arm, so it obeys the same wrap rule; `a?.#b`
        // prints `?.` and no `.` (:3748-3750).
        case mbun::ast::NodeKind::PrivateMember: {
            const bool isOptionalChain { (n.flags & mbun::ast::ocflags::IsOptionalStart) != 0 };
            bool wrap { false };
            if ((n.flags & mbun::ast::ocflags::IsOptionalAny) == 0) {
                flags |= ExprFlag::HasNonOptionalChainParent;
            } else {
                if ((flags & ExprFlag::HasNonOptionalChainParent) != expr_flag::none()) {
                    wrap = true;
                    self_().print("(");
                }
                flags &= ~ExprFlag::HasNonOptionalChainParent;
            }
            print_expr(n.a, Level::Postfix, flags);
            self_().print(isOptionalChain ? "?." : ".");
            self_().add_source_mapping(e);
            self_().print(n.text);
            if (wrap) {
                self_().print(")");
            }
            break;
        }

        // ref :4515 — bun PANICS on a bare `EPrivateIdentifier` in expression
        // position ("Unexpected expression of type .{}") because the only legal
        // spot is `#x in obj`, which `binary_check_and_prepare` special-cases at
        // :1786 before ever recursing. mbun reaches the same place through
        // `NodeKind::PrivateName`; printing the name is strictly better than
        // aborting the process, and the `#x in obj` path below never gets here.
        case mbun::ast::NodeKind::PrivateName:
            self_().print_space_before_identifier();
            self_().add_source_mapping(e);
            self_().print(n.text.empty() ? raw_(n) : n.text);
            break;

        // ref :3435 — `ENew`
        case mbun::ast::NodeKind::New: {
            // ref :3436-3439. mbun's AST has no `can_be_unwrapped_if_unused`, so
            // the `/* @__PURE__ */` half of bun's condition is unreachable; `wrap`
            // reduces to the precedence test.
            const bool wrap { level >= Level::Call };
            if (wrap) {
                self_().print("(");
            }
            self_().print_space_before_identifier();
            self_().add_source_mapping(e);
            self_().print("new");
            self_().print_space();
            print_expr(n.a, Level::New, expr_flag::forbid_call());

            const std::span<const mbun::ast::NodeIndex> args { self_().arena().list_of(n) };
            // ref :3455 — `new X` may drop its parens, but not at Postfix or
            // above (`new X().y` must keep them).
            if (!args.empty() || level >= Level::Postfix) {
                self_().print("(");
                for (std::size_t i { 0 }; i < args.size(); ++i) {
                    if (i != 0) {
                        self_().print(",");
                        self_().print_space();
                    }
                    print_expr(args[i], Level::Comma, expr_flag::none());
                }
                self_().print(")");
            }
            if (wrap) {
                self_().print(")");
            }
            break;
        }

        // ref :3478 — `ECall`
        case mbun::ast::NodeKind::Call: {
            // ref :3479-3486. Note bun reuses the SAME `wrap` for both the
            // precedence test and the chain-cut parens — a call that must be
            // re-parenthesised because its parent is outside the chain gets one
            // pair of parens, not two. And unlike EDot/EIndex the target flags are
            // cleared to none() rather than merely having the bit removed.
            bool wrap { level >= Level::New || has_flag(flags, ExprFlag::ForbidCall) };
            ExprFlagSet targetFlags { expr_flag::none() };
            if ((n.flags & mbun::ast::ocflags::IsOptionalAny) == 0) {
                targetFlags = expr_flag::has_non_optional_chain_parent();  // ref :3482
            } else if (has_flag(flags, ExprFlag::HasNonOptionalChainParent)) {
                wrap = true;  // ref :3484 — `(a?.b())` when the parent left the chain
            }

            if (wrap) {
                self_().print("(");
            }
            // ref :3517 — bun rewrites an unbound `eval(..)` to `(0, eval)(..)`
            // to force indirect eval. That needs the symbol table (`is_unbound_
            // eval_identifier`, :2663) which is shard 4's renamer; mbun's printer
            // has no symbol table at all. DEFERRED(shard-4). (bun also requires
            // `optional_chain.is_none()` there, :3509 — `a?.()` is never indirect
            // eval — so wiring this later must keep that condition.)
            print_expr(n.a, Level::Postfix, targetFlags);
            if ((n.flags & mbun::ast::ocflags::IsOptionalStart) != 0) {
                self_().print("?.");  // ref :3520-3522
            }
            self_().print("(");
            const std::span<const mbun::ast::NodeIndex> args { self_().arena().list_of(n) };
            for (std::size_t i { 0 }; i < args.size(); ++i) {
                if (i != 0) {
                    self_().print(",");
                    self_().print_space();
                }
                print_expr(args[i], Level::Comma, expr_flag::none());
            }
            self_().print(")");
            if (wrap) {
                self_().print(")");
            }
            break;
        }

        // ref :3623 — `EImport` (dynamic `import(..)`)
        case mbun::ast::NodeKind::ImportCall: {
            self_().print_space_before_identifier();
            self_().add_source_mapping(e);
            self_().print("import(");
            const std::span<const mbun::ast::NodeIndex> args { self_().arena().list_of(n) };
            for (std::size_t i { 0 }; i < args.size(); ++i) {
                if (i != 0) {
                    self_().print(",");
                    self_().print_space();
                }
                print_expr(args[i], Level::Comma, expr_flag::none());
            }
            if (args.empty() && n.a != mbun::ast::NONE) {
                print_expr(n.a, Level::Comma, expr_flag::none());
            }
            self_().print(")");
            break;
        }

        // ref :3885 — `EArray`
        case mbun::ast::NodeKind::ArrayLiteral: {
            self_().add_source_mapping(e);
            self_().print("[");
            const std::span<const mbun::ast::NodeIndex> items { self_().arena().list_of(n) };
            for (std::size_t i { 0 }; i < items.size(); ++i) {
                if (i != 0) {
                    self_().print(",");
                    self_().print_space();
                }
                print_expr(items[i], Level::Comma, expr_flag::none());
                // ref :3907-3909 — a trailing hole needs an explicit comma:
                // `[a, ,]` has length 2, `[a, ]` has length 1.
                if (i == items.size() - 1 && items[i] != mbun::ast::NONE
                    && self_().node(items[i]).kind == mbun::ast::NodeKind::Missing) {
                    self_().print(",");
                }
            }
            self_().print("]");
            break;
        }

        // ref :4031 — `ETemplate`. bun re-cooks the parts; mbun's template node
        // keeps its source span, and re-emitting the ORIGINAL bytes is both
        // correct and the only option without decoded cooked values (seam 2).
        // ⚠️ Template SUBSTITUTIONS are therefore not re-printed either — a
        // nested arrow inside `${..}` keeps its original formatting. Fixing that
        // needs the parser to record the parts; see seam (2).
        case mbun::ast::NodeKind::TemplateLiteral:
        case mbun::ast::NodeKind::TaggedTemplate:
            self_().add_source_mapping(e);
            self_().print(raw_(n));
            break;

        // ref :4431 — `EBinary`, plus mbun's `Logical` and `Assignment`, which
        // bun models as the same `E::Binary` with a different `Op::Code`.
        case mbun::ast::NodeKind::Binary:
        case mbun::ast::NodeKind::Logical:
        case mbun::ast::NodeKind::Assignment:
            print_binary_(e, level, flags);
            break;

        // mbun-only node: bun spells `a, b` as `E::Binary` with `Op::BinComma`
        // (op.rs:78) and has no Sequence node. Lowered to the same shape here so
        // the comma operator gets bun's exact precedence and
        // `ExprResultIsUnused` propagation (:1798, :1836) instead of a
        // hand-rolled second implementation.
        case mbun::ast::NodeKind::Sequence: {
            const std::span<const mbun::ast::NodeIndex> items { self_().arena().list_of(n) };
            if (items.empty()) {
                break;
            }
            const bool wrap { level >= Level::Comma };
            if (wrap) {
                self_().print("(");
            }
            for (std::size_t i { 0 }; i < items.size(); ++i) {
                if (i != 0) {
                    self_().print(",");
                    self_().print_space();
                }
                // ref :1836 — every element but the last has an unused result.
                const ExprFlagSet elemFlags { i + 1 < items.size()
                        ? expr_flag::expr_result_is_unused()
                        : (flags & ExprFlag::ExprResultIsUnused) };
                print_expr(items[i], sub(Level::Comma, 1), elemFlags);
            }
            if (wrap) {
                self_().print(")");
            }
            break;
        }

        // ref :4386 — `EUnary`, and mbun's `Update` (`++`/`--`), which bun models
        // as `E::Unary` with `UnPre*`/`UnPost*` codes (op.rs:21-26).
        case mbun::ast::NodeKind::Unary:
        case mbun::ast::NodeKind::Update:
            print_unary_(e, level);
            break;

        // ref :3926 — `EObject`.
        case mbun::ast::NodeKind::ObjectLiteral:
            print_object_literal_(e);
            break;

        // ── the function-valued arms — arrow.cppm ─────────────────────────────
        // Called DIRECTLY, one arm per ExprData exactly as the blueprint has them.
        // These three used to share a `requires`-probed fallback that echoed the
        // raw source span when the probe missed; nothing ever satisfied the probe,
        // so every arrow/function/class expression printed its source bytes with
        // no erasure applied (TS annotations and all) and reported success. A
        // direct call cannot fail that way: if the method goes missing it is a
        // compile error. See arrow.cppm's header.
        case mbun::ast::NodeKind::Arrow:
            self_().print_arrow(e, level, flags);  // ref :3789 — `EArrow`
            break;

        // ref :3835 / :3865 — `EFunction` / `EClass`. Neither takes `level`: their
        // wrap is positional (stmt_start / export_default_start), and no operator
        // precedence can force parens on a primary expression.
        case mbun::ast::NodeKind::FunctionExpr:
            self_().print_function_expr(e);
            break;

        case mbun::ast::NodeKind::ClassExpr:
            self_().print_class_expr(e);
            break;

        default:
            // Statement/binding/TS nodes are not expressions. bun debug-panics on
            // an unexpected ExprData (:4515); the printer must not abort a build,
            // so unknown kinds print nothing and the surrounding structure stays
            // well formed.
            break;
        }
    }

private:
    // ═════════════════════════════════════════════════════════════════════════
    // ref :3926 — `EObject`.
    //
    // ⚠️ The `wrap` test is the whole reason `stmt_start` / `arrow_expr_start`
    // exist (printer_core.cppm:141-144). An object literal at the START of a
    // statement is a BLOCK, and at the start of an arrow body it is the body's
    // block — `({a: 1}).b` and `() => ({a: 1})` are only objects because of the
    // parens. bun tests it by OFFSET, not by a flag threaded down the walk: if
    // nothing has been written since the statement/arrow began, we are at that
    // position. Verified against real bun 1.3.14: `({a:1}).b;` → `({ a: 1 }).b;`
    // and `x = () => ({a:1});` → `x = () => ({ a: 1 });`, while `x = {a:1};`
    // → `x = { a: 1 };` (no parens — not at a statement start).
    //
    // `level` is deliberately unused: bun's arm ignores it too. An object literal
    // is a primary expression, so no operator precedence can force parens on it —
    // only position can, which is what `wrap` handles.
    // ═════════════════════════════════════════════════════════════════════════
    void print_object_literal_(mbun::ast::NodeIndex e) {
        const mbun::ast::Node& n { self_().node(e) };

        // ref :3927-3934
        const std::int32_t written { self_().written() };
        bool wrap { false };
        if constexpr (!D::flags().isJson) {
            wrap = self_().stmtStart == written || self_().arrowExprStart == written;
        }

        if (wrap) {
            self_().print('(');  // ref :3936
        }
        self_().add_source_mapping(e);  // ref :3938
        self_().print('{');             // ref :3939

        // ref :3940 — `e.properties.slice()`. Read once; print_property recurses
        // into the arena but nothing here mutates it.
        const std::span<const mbun::ast::NodeIndex> props { self_().arena().list_of(n) };
        // ref e.rs:1233 `is_single_line`, recorded by the parser (js_parser.cppm
        // parse_object_literal_) the same way the binding side records
        // bflags::IsSingleLine.
        const bool isSingleLine { (n.flags & mbun::ast::eflags::IsSingleLine) != 0 };

        // ref :3941 — an EMPTY object skips the entire block, which is why `{}`
        // prints as `{}` and not `{ }`. Same rule, same place, as the binding
        // object's :5131 (binding.cppm) — and the array arm has NO such padding
        // at all, an asymmetry that is bun's.
        if (!props.empty()) {
            if (!isSingleLine) {
                self_().indent();  // ref :3942-3944
            }

            // ref :3946-3951 — this three-line dance repeats verbatim three
            // times in bun (before the first property, between properties, and
            // before the `}`). Kept as a lambda rather than expanded three times:
            // the three sites must not drift apart, and each is bun's exact
            // `if is_single_line && !IS_JSON { print_space } else { print_newline;
            // print_indent }`.
            const auto separator = [&] {
                if (isSingleLine && !D::flags().isJson) {
                    self_().print_space();
                } else {
                    self_().print_newline();
                    self_().print_indent();
                }
            };

            separator();                      // ref :3946-3951
            self_().print_property(props[0]);  // ref :3952

            // ref :3954-3963
            for (std::size_t i { 1 }; i < props.size(); ++i) {
                self_().print(',');
                separator();
                self_().print_property(props[i]);
            }

            // ref :3965-3971 — the closing pad. NOTE it is NOT `separator()`:
            // the multi-line branch UNINDENTS first. Getting this wrong indents
            // the `}` one level too deep.
            if (isSingleLine && !D::flags().isJson) {
                self_().print_space();
            } else {
                self_().unindent();
                self_().print_newline();
                self_().print_indent();
            }
        }

        // ref :3973-3975 — the close-brace sourcemap entry; add_source_mapping is
        // a no-op (stmt.cppm:44-46), so there is nothing to guard.
        self_().print('}');  // ref :3976
        if (wrap) {
            self_().print(')');  // ref :3978
        }
    }

    // ref :4386 — `EUnary`. Covers mbun's `Unary` and `Update` via `to_op_code`.
    void print_unary_(mbun::ast::NodeIndex e, Level level) {
        const mbun::ast::Node& n { self_().node(e) };

        const OpPosition pos { n.kind == mbun::ast::NodeKind::Update
                ? ((n.flags & 1u) != 0 ? OpPosition::UpdatePre : OpPosition::UpdatePost)
                : OpPosition::Unary };
        const std::optional<Code> code {
            to_op_code(static_cast<mbun::js_lexer::Token>(n.aux), pos)
        };
        if (!code.has_value()) {
            return;  // unrecognised operator: print nothing rather than garbage
        }
        const Code op { *code };
        const Op entry { op_info(op) };

        const bool wrap { level >= entry.level };  // ref :4388
        if (wrap) {
            self_().print("(");
        }

        // ref :4394 — the postfix operand prints BEFORE the operator.
        if (!is_prefix(op)) {
            print_expr(n.a, sub(Level::Postfix, 1), expr_flag::none());
        }

        if (entry.isKeyword) {
            // ref :4398-4402 — `typeof`/`void`/`delete`/`await` need a space and
            // must not fuse with a preceding identifier.
            self_().print_space_before_identifier();
            self_().add_source_mapping(e);
            self_().print(entry.text);
            self_().print_space();
        } else {
            // ref :4404-4409
            print_space_before_operator(op);
            if (is_prefix(op)) {
                self_().add_source_mapping(e);
            }
            self_().print(entry.text);
            prevOp = op;
            self_().prevOpEnd = self_().written();
        }

        if (is_prefix(op)) {
            // ref :4413-4421. The comment at :4414 is the spec: "Never turn
            // `typeof (0, x)` into `typeof x` or `delete (0, x)` into `delete x`".
            // Both rewrites would change meaning — `typeof undeclared` is legal
            // but `typeof (0, undeclared)` throws, and `delete x` on a bare
            // identifier is a SyntaxError in strict mode.
            //
            // ⚠️ NOT REACHABLE from mbun input today. bun guards each side with a
            // "was originally spelled that way" flag (`WAS_ORIGINALLY_TYPEOF_
            // IDENTIFIER` / `WAS_ORIGINALLY_DELETE_OF_IDENTIFIER_OR_PROPERTY_
            // ACCESS`, :4415/:4416) that only its *parser* sets, and the typeof
            // side additionally needs the symbol table (`is_unbound_identifier`,
            // :2716) that shard 4 owns. mbun's AST has neither, so a faithful
            // port would ALWAYS fire and would wrap every `typeof x` as
            // `typeof (0, x)` — strictly worse than not porting it. The `delete`
            // half is transcribed with the flag treated as "always originally
            // spelled that way" (i.e. suppressed), which is what mbun's
            // parse-then-print round trip actually guarantees: the operand really
            // was written that way in the source.
            //
            // This arm becomes live when the parser records those two flags. Left
            // as an explicit no-op rather than a silent omission so the next
            // person sees the decision instead of the absence.
            print_expr(n.a, sub(Level::Prefix, 1), expr_flag::none());
        }

        if (wrap) {
            self_().print(")");
        }
    }

    // ═════════════════════════════════════════════════════════════════════════
    // Binary expressions — ref :1659 / :1716 / :1814 / :4431
    //
    // ⚠️ HEAP ITERATION, NOT RECURSION — and this is not incidental. bun's
    // comment at :4432-4434 and :1653-1656: a left-leaning chain (`a+b+c+...`)
    // nests as deep as the source is long, so recursing on the LEFT spine
    // overflows the stack on real minified input. printer_core.cppm:279-282
    // repeats the warning ("Do not 'simplify' it back into recursion").
    //
    // The shape: walk down the left spine pushing a visitor per level, print the
    // leftmost non-binary leaf, then unwind printing each right operand. Only the
    // LEFT spine is iterated — the right operand recurses through print_expr,
    // which is fine because right-nesting is bounded by precedence in practice.
    // ═════════════════════════════════════════════════════════════════════════

    // ref :1659
    struct BinaryExpressionVisitor {
        mbun::ast::NodeIndex e { mbun::ast::NONE };
        Code op { Code::BinAdd };
        Level level { Level::Lowest };
        ExprFlagSet flags { expr_flag::none() };
        Level leftLevel { Level::Lowest };
        ExprFlagSet leftFlags { expr_flag::none() };
        Op entry {};
        bool wrap { false };
        Level rightLevel { Level::Lowest };
    };

    // Resolve the `Op::Code` for a Binary/Logical/Assignment node. Returns
    // nullopt for an operator the seam does not recognise.
    [[nodiscard]] std::optional<Code> binary_op_(const mbun::ast::Node& n) const {
        return to_op_code(static_cast<mbun::js_lexer::Token>(n.aux), OpPosition::Binary);
    }

    // ═════════════════════════════════════════════════════════════════════════
    // skip_parens_ — the mbun-only structural seam. NOT in the blueprint.
    //
    // ⚠️ EVERY structural test in bun's print_expr must go through this, and
    // forgetting it is SILENT: the test simply never fires and a required paren
    // goes missing.
    //
    // bun's parser DISCARDS parentheses — its AST has no paren node, and the
    // printer re-derives every paren from `Level` (that is what the whole
    // precedence model is for). So when bun asks "is the left operand an
    // `E::Binary`?" (:1754) it is asking about the REAL operand. mbun's parser
    // keeps an explicit `NodeKind::Paren` (ast.cppm:61), so the same question
    // asked naively gets "no, it's a Paren" and the special case is skipped.
    //
    // Caught by four failing vectors, each a real mis-print:
    //   `(-a) ** b`      → `-a ** b`      (a SyntaxError — ref :1765-1772)
    //   `a ?? (b || c)`  → `a ?? b || c`  (a SyntaxError — ref :1752-1763)
    //   `(a && b) ?? c`  → `a && b ?? c`  (a SyntaxError — same)
    // Printing is already paren-transparent (the `Paren` arm recurses), so this
    // only concerns INSPECTION.
    [[nodiscard]] mbun::ast::NodeIndex skip_parens_(mbun::ast::NodeIndex i) const {
        while (i != mbun::ast::NONE && self_().node(i).kind == mbun::ast::NodeKind::Paren) {
            i = self_().node(i).a;
        }
        return i;
    }

    [[nodiscard]] bool is_binary_kind_(mbun::ast::NodeIndex i) const {
        i = skip_parens_(i);
        if (i == mbun::ast::NONE) {
            return false;
        }
        switch (self_().node(i).kind) {
        case mbun::ast::NodeKind::Binary:
        case mbun::ast::NodeKind::Logical:
        case mbun::ast::NodeKind::Assignment:
            return true;
        default:
            return false;
        }
    }

    // ref :4431
    void print_binary_(mbun::ast::NodeIndex e, Level level, ExprFlagSet flags) {
        const std::optional<Code> op0 { binary_op_(self_().node(e)) };
        if (!op0.has_value()) {
            return;
        }

        BinaryExpressionVisitor v {};
        v.e = e;
        v.op = *op0;
        v.level = level;
        v.flags = flags;
        v.entry = op_info(*op0);

        // ref :4448 — one shared stack to reduce allocation overhead. Kept as a
        // local rather than the `binary_expression_stack` Printer field bun uses
        // (:1643): that field exists to reuse one allocation across sibling
        // binary expressions, which needs a Core field this shard cannot add
        // (铁律 1). ⚠️ COORDINATOR: hoisting it is the follow-up, together with
        // seam (3).
        std::vector<BinaryExpressionVisitor> stack {};

        // ref :4450-4483 — descend the left spine.
        for (;;) {
            if (!binary_check_and_prepare_(v)) {
                break;
            }
            const mbun::ast::NodeIndex left { skip_parens_(self_().node(v.e).a) };

            // ref :4462 — stop iterating when the left node is not itself binary.
            if (!is_binary_kind_(left)) {
                print_expr(left, v.leftLevel, v.leftFlags);
                binary_visit_right_and_finish_(v);
                break;
            }

            const std::optional<Code> leftOp { binary_op_(self_().node(left)) };
            if (!leftOp.has_value()) {
                print_expr(left, v.leftLevel, v.leftFlags);
                binary_visit_right_and_finish_(v);
                break;
            }

            // ref :4470-4480 — only heap-allocate for nested binary expressions.
            BinaryExpressionVisitor next {};
            next.e = left;
            next.op = *leftOp;
            next.level = v.leftLevel;
            next.flags = v.leftFlags;
            next.entry = op_info(*leftOp);  // overwritten in check_and_prepare
            stack.push_back(v);
            v = next;
        }

        // ref :4487-4490 — unwind, printing each right operand back up to the top.
        while (!stack.empty()) {
            const BinaryExpressionVisitor last { stack.back() };
            stack.pop_back();
            binary_visit_right_and_finish_(last);
        }
    }

    // ref :1716
    [[nodiscard]] bool binary_check_and_prepare_(BinaryExpressionVisitor& v) {
        const mbun::ast::Node& n { self_().node(v.e) };
        const Code op { v.op };
        const Op entry { op_info(op) };
        const Level eLevel { entry.level };
        v.entry = entry;

        // ref :1723 — `in` inside a for-init must be parenthesised or it reads as
        // the for-in keyword.
        v.wrap = v.level >= eLevel || (op == Code::BinIn && has_flag(v.flags, ExprFlag::ForbidIn));

        // ref :1727-1733 — destructuring assignments must be parenthesised:
        // a statement starting `{a} = b` would parse `{a}` as a block.
        const std::int32_t written { self_().written() };
        if (written == self_().stmtStart || written == self_().arrowExprStart) {
            const mbun::ast::NodeIndex left { skip_parens_(n.a) };
            if (left != mbun::ast::NONE
                && self_().node(left).kind == mbun::ast::NodeKind::ObjectLiteral) {
                v.wrap = true;
            }
        }

        if (v.wrap) {
            self_().print("(");
            v.flags |= ExprFlag::ForbidIn;  // ref :1737
        }

        // ref :1740-1749 — associativity. The default is that BOTH sides bind
        // tighter than this operator; the associative side then relaxes back to
        // this operator's own level so a same-level chain does not re-parenthesise.
        v.leftLevel = sub(eLevel, 1);
        v.rightLevel = sub(eLevel, 1);
        if (is_right_associative(op)) {
            v.leftLevel = eLevel;
        }
        if (is_left_associative(op)) {
            v.rightLevel = eLevel;
        }

        switch (op) {
        // ref :1752-1763 — "??" can't directly contain "||" or "&&" without
        // parens (a real JS grammar restriction, not a style choice).
        case Code::BinNullishCoalescing: {
            const mbun::ast::NodeIndex left { skip_parens_(n.a) };
            if (is_binary_kind_(left)) {
                if (const std::optional<Code> lo { binary_op_(self_().node(left)) };
                    lo == Code::BinLogicalAnd || lo == Code::BinLogicalOr) {
                    v.leftLevel = Level::Prefix;
                }
            }
            const mbun::ast::NodeIndex right { skip_parens_(n.b) };
            if (is_binary_kind_(right)) {
                if (const std::optional<Code> ro { binary_op_(self_().node(right)) };
                    ro == Code::BinLogicalAnd || ro == Code::BinLogicalOr) {
                    v.rightLevel = Level::Prefix;
                }
            }
            break;
        }

        // ref :1765-1784 — "**" can't contain certain unary expressions on its
        // left: `-a ** b` is a SyntaxError, it must be `(-a) ** b`.
        case Code::BinPow: {
            const mbun::ast::NodeIndex left { skip_parens_(n.a) };
            if (left != mbun::ast::NONE) {
                const mbun::ast::Node& ln { self_().node(left) };
                switch (ln.kind) {
                case mbun::ast::NodeKind::Unary: {
                    // ref :1767-1772 — only non-update unary ops are affected;
                    // `--a ** b` is legal because `--a` is an update expression.
                    const std::optional<Code> lo {
                        to_op_code(static_cast<mbun::js_lexer::Token>(ln.aux), OpPosition::Unary)
                    };
                    if (lo.has_value() && unary_assign_target(*lo) == AssignTarget::None) {
                        v.leftLevel = Level::Call;
                    }
                    break;
                }
                // ref :1774-1776 — `await a ** b` / `undefined ** b` / `1 ** b`
                // all need the parens (the numeric case because `-1 ** b` is the
                // SyntaxError and a negative literal prints with a leading `-`).
                case mbun::ast::NodeKind::NumberLiteral:
                    v.leftLevel = Level::Call;
                    break;
                // ref :1777-1783 — when minifying, booleans print as `!0`/`!1`,
                // which are unary expressions and would then be illegal here.
                case mbun::ast::NodeKind::BooleanLiteral:
                    if (self_().options.minifySyntax) {
                        v.leftLevel = Level::Call;
                    }
                    break;
                default:
                    break;
                }
            }
            break;
        }

        default:
            break;
        }

        // ref :1786-1797 — special-case `#foo in bar`. The private name is NOT a
        // printable expression on its own (bun panics on it at :4515), so it is
        // emitted directly here and the visitor finishes early.
        const mbun::ast::NodeIndex left { skip_parens_(n.a) };
        if (op == Code::BinIn && left != mbun::ast::NONE
            && self_().node(left).kind == mbun::ast::NodeKind::PrivateName) {
            const mbun::ast::Node& ln { self_().node(left) };
            print_identifier(ln.text.empty() ? raw_(ln) : ln.text);
            binary_visit_right_and_finish_(v);
            return false;
        }

        // ref :1799-1807
        v.leftFlags = expr_flag::none();
        if (has_flag(v.flags, ExprFlag::ForbidIn)) {
            v.leftFlags |= ExprFlag::ForbidIn;
        }
        if (op == Code::BinComma) {
            v.leftFlags |= ExprFlag::ExprResultIsUnused;
        }
        return true;
    }

    // ref :1814 — `BinaryExpressionVisitor::visitRightAndFinish`
    void binary_visit_right_and_finish_(const BinaryExpressionVisitor& v) {
        const mbun::ast::Node& n { self_().node(v.e) };
        const Code op { v.op };
        const Op entry { v.entry };
        ExprFlagSet flags { expr_flag::none() };

        // ref :1820 — no space before a comma: `a, b` not `a , b`.
        if (op != Code::BinComma) {
            self_().print_space();
        }

        if (entry.isKeyword) {
            // ref :1824-1826 — `in` / `instanceof`.
            self_().print_space_before_identifier();
            self_().print(entry.text);
        } else {
            // ref :1828-1832
            print_space_before_operator(op);
            self_().print(entry.text);
            prevOp = op;
            self_().prevOpEnd = self_().written();
        }

        self_().print_space();

        // ref :1837-1839 — the right operand of a comma inherits "result unused".
        if (op == Code::BinComma && has_flag(v.flags, ExprFlag::ExprResultIsUnused)) {
            flags |= ExprFlag::ExprResultIsUnused;
        }
        if (has_flag(v.flags, ExprFlag::ForbidIn)) {
            flags |= ExprFlag::ForbidIn;
        }

        print_expr(n.b, v.rightLevel, flags);

        if (v.wrap) {
            self_().print(")");
        }
    }

    // ── ⚠️ REMOVED: `print_deferred_shard_`, the raw-source fallback ─────────
    // It probed for `print_expr_deferred` with `requires` and echoed
    // `raw_(n) = source.substr(...)` when the probe missed. No shard ever defined
    // that method, so the probe was always false and the echo was the ONLY
    // behaviour Arrow/FunctionExpr/ClassExpr ever had — source bytes, with none
    // of the erasure edits applied, reported as success. The arms now call
    // arrow.cppm directly (see the switch above); a `requires` probe that can
    // silently degrade to wrong-but-quiet output is exactly what must not grow
    // back, which is why the seam is a hard call and not a detected one.
};

}  // namespace mbun::js_printer
