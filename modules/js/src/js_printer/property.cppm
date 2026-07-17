// src/js_printer/property.cppm — module mbun.js_printer.property
//
// CAP-BUILD-PRINTER — `print_property`, the last hole in the printer.
//
// A 1:1 port of the `G::Property` half of bun's `Printer` impl block, per
// AGENTS.md 「移植三段法」. Blueprint: .mbun/bun-ref/src/js_printer/lib.rs
//
//   :4743  `print_property`      — the function this file is built around
//   :4759  the Spread arm        :4812  IsStatic     :4816-4838  Get/Set/AutoAccessor
//   :4843  the method arm        :4869  the computed-key arm
//   :4906  the EString-key arm (+ shorthand folding, :4921)
//   :5006  the trailing kind/method/value/initializer tail
//   :5045  `print_initializer`   — bun defines it directly after print_property
//
// The shard contract is at the top of printer_core.cppm — read that first. In
// short: CRTP mixin, cross-shard calls go through `self_()`. This file calls
// shard 2's `print_expr` / `print_identifier` / `source` and shard 3's `arena()`
// / `node()` / `print_space_before_identifier` / `add_source_mapping`
// (stmt.cppm:24-49 homes those three), and must not redefine any of them: two
// definitions in two mixins is an ambiguous base at assembly.
//
// ═════════════════════════════════════════════════════════════════════════════
// Why this shard could not be written until now
// ═════════════════════════════════════════════════════════════════════════════
// 414263eb parked it, and it was right to: print_property is almost entirely a
// function of `G::Property`'s fields (g.rs:143), and mbun's Property node was
// `a=key b=value` and nothing else. Porting it then would have been transcription
// against an invented model. e9c3a411 landed the field and taught the parser to
// record it (PropertyKind -> aux, pflags -> flags, initializer -> c), so this is
// now a mechanical port and every arm below has real input to switch on.
//
// ═════════════════════════════════════════════════════════════════════════════
// The AST mapping — bun's `G::Property` onto mbun's flat node (ast.cppm:230-283)
// ═════════════════════════════════════════════════════════════════════════════
//   g.rs:143 G::Property{key}         -> a    (NONE for spread — g.rs:158)
//                        {value}      -> b    (NONE for a class field — g.rs:161)
//                        {initializer}-> c
//                        {kind}       -> aux  (ast::PropertyKind, g.rs:245)
//                        {flags}      -> flags(ast::pflags, ast/lib.rs:3283)
//
// ⚠️ This is NOT the `b::Property` that binding.cppm prints (b.rs:68). The two
// share a name and nothing else — see the note at ast.cppm:230.
//
// ═════════════════════════════════════════════════════════════════════════════
// DEFERRED — with reasons and blueprint line numbers, not as an excuse
// ═════════════════════════════════════════════════════════════════════════════
//  1. THE METHOD ARMS (:4843-4860, :4880, :5006-5020) — FIXED. bun reaches
//     THROUGH the value for everything a method needs: `async`/`*` come off the
//     value function's `G::FnFlags` (:4844/:4847) and the parameter list and body
//     are printed by `print_func` (:4880, :5017). Both prerequisites have landed:
//       (a) the parser now BUILDS the value function for a method shorthand, in
//           both parse_object_property_ and parse_class_member_, carrying
//           IsAsync/IsGenerator/HasRestArg on the function exactly where g.rs:285
//           keeps them. `{m(){}}` used to print `{m}`; it now prints `{m(){}}`.
//       (b) `print_func` exists — func.cppm (ref :2527), together with
//           `print_class` (:2543) and the two decl arms stmt.cppm calls.
//     ⚠️ There is still NO Property-level `IsAsync`, and there must not be: bun
//     has no such flag (ast/lib.rs:3283-3287 is the whole Property set), and the
//     `async` of `{async m(){}}` lives on the VALUE FUNCTION. A Property bit
//     would be a model bun does not have.
//     ⚠️ This printer is STILL not wired into `transpile`, which remains
//     erasure-based (see printer.cppm) — that is a separate, much larger change.
//  2. THE ENUM-INLINING KEY REWRITE (:4770-4810). `minify_syntax` + a computed
//     `EDot` key whose target resolves to an imported enum constant is folded to
//     a string/number key and un-computed. It needs `try_to_get_imported_enum_
//     value` (a cross-module symbol/enum table) — mbun has no symbol table in the
//     print path at all, which is the same reason binding.cppm defers
//     `print_symbol` and renamer.rs is parked. There is no partial version of
//     this that is not a guess.
//  3. `print_symbol(priv_.ref_)` (:4904). bun's private key is an
//     `E::PrivateIdentifier` NODE resolved through the symbol table. The parser
//     now BUILDS that node (mbun's `PrivateName`, carrying the `#name` text), so
//     the key exists; what is still deferred is only the symbol RESOLUTION —
//     mbun has no renamer, so the name prints from the node's own text, exactly
//     as binding.cppm:203 does for the same reason. pflags::IsPrivate stays
//     because there is no symbol table to ask instead (ast.cppm's note).
//  4. THE UTF-16 KEY ARM (:4952-4991) — `can_print_identifier_utf16` /
//     `print_identifier_utf16` / `WasShorthand`-driven folding. mbun stores key
//     text as UTF-8 source slices; there is no UTF-16 key to take this arm.
//     binding.cppm defers the identical arm (:5205-5215) for the identical
//     reason. NOTE: a decoded-string pool is landing in ast.cppm in parallel; if
//     keys ever carry decoded UTF-16, this arm is where it plugs in.
//  5. CLASS MEMBERS — FIXED. parse_class_member_ used to build a Property as an
//     ERASURE MARKER: it advanced over the key and the body and built neither,
//     so the node had a == NONE and b == NONE, and every class member printed as
//     nothing (`class C { m(){} }` -> `class C { ; }`). The parser now builds
//     both, and func.cppm ports `print_class`. The `key == NONE` guard below
//     STAYS: bun's `item.key.expect(...)` (:4906) is an infallible unwrap only
//     because bun's type system guarantees it, and mbun's arena is a flat record
//     that can still hold a keyless non-spread Property. Printing nothing beats
//     reproducing a panic on input bun cannot represent.
//  6. `stack_check.is_safe_to_recurse()` (:4744-ish, as at :5053). printer_core
//     has no `stack_check`; it is print_expr's and print_stmt's problem too, so
//     it is not invented here. Tracked where shard 3/4 left it.
//
// ═════════════════════════════════════════════════════════════════════════════
// A latent bug this file used to route around — FIXED, and staying fixed
// ═════════════════════════════════════════════════════════════════════════════
// binding.cppm:366 calls `self_().print_string_literal_utf8(name, false)`, and
// that name did NOT exist on the assembled `Printer`: expr.cppm defined it
// private and underscore-suffixed. The real `Printer::print_binding` could not
// be instantiated at all, and nothing caught it because
// test_js_printer_binding.cpp's harness defines a stub of that name. It is now
// public and unsuffixed on ExprPrinter — which is what bun declares it as
// (lib.rs:3022 `pub fn`) — and tests/test_js_printer_real.cpp instantiates the
// REAL Printer so it cannot regress silently again.
//
// `print_quoted_key_` below did NOT collapse into that seam, and should not:
// they are two different bun functions. This file's key path is bun's
// `print_string_literal_e_string(&key_str, false)` (:4917); binding.cppm:366 is
// bun's `print_string_literal_utf8` (:5180). Same bytes for mbun's UTF-8 key
// slices today, different functions in the blueprint — and the day keys carry
// decoded UTF-16 (see DEFERRED 4), this one takes the e-string arm and the other
// does not.
export module mbun.js_printer.property;

import std;
import mbun.ast;
import mbun.js_printer.quote;
import mbun.js_printer.flags;
import mbun.js_printer.op_level;
import mbun.js_printer.binding;  // `is_identifier` (binding.cppm:102) — reused, not re-derived

export namespace mbun::js_printer {

// ─────────────────────────────────────────────────────────────────────────────
// PropertyPrinter — ref lib.rs:4743 `print_property` + :5045 `print_initializer`.
// ─────────────────────────────────────────────────────────────────────────────
template <typename D>
class PropertyPrinter {
private:
    [[nodiscard]] D& self_() { return static_cast<D&>(*this); }

    [[nodiscard]] const mbun::ast::Node& node_(mbun::ast::NodeIndex i) {
        return self_().arena().at(i);
    }

    // An Identifier/StringLiteral key carries no `text` when the parser built it
    // straight from a token span, so fall back to the source slice — the same
    // idiom shard 2's EIdentifier arm uses (expr.cppm:813).
    [[nodiscard]] std::string_view name_of_(mbun::ast::NodeIndex i) {
        const mbun::ast::Node& n { node_(i) };
        if (!n.text.empty()) {
            return n.text;
        }
        return self_().source.substr(n.start, n.end - n.start);
    }

    // ref :4917 `print_string_literal_e_string(&key_str, false)` — a key that is
    // not a valid identifier must be quoted. See the "latent bug" note in the
    // header for why this is local rather than shard 2's helper.
    void print_quoted_key_(std::string_view text) {
        std::uint8_t quote { '"' };
        if constexpr (!D::flags().isJson) {
            // ⚠️ instantiate over std::uint8_t, never char: the cost per unit is
            // `static_cast<std::uint32_t>(str[i])`, and a signed char would
            // sign-extend every byte >= 0x80 past every arm of the switch.
            // expr.cppm:698 documents the same trap.
            quote = best_quote_char_for_string(
                std::span<const std::uint8_t> {
                    reinterpret_cast<const std::uint8_t*>(text.data()), text.size() },
                /*allowBacktick=*/false);
        }
        self_().print(static_cast<char>(quote));
        self_().print_string_characters_utf8(text, quote);
        self_().print(static_cast<char>(quote));
    }

    // ref :4890 — "Automatically print numbers that would cause a syntax error as
    // computed properties": `{ -1: 0 }` must print as `{ [-1]: 0 }`. The predicate
    // is shard 2's (it matches on a folded `ENumber` value); binding.cppm:151
    // gates the identical call the identical way. NOT reachable from mbun's parser
    // today: a numeric key is a NumberLiteral whose text is the source slice, so
    // there is no folded value to test and no key forces itself computed.
    [[nodiscard]] bool key_must_be_computed_(mbun::ast::NodeIndex key) {
        if (node_(key).kind != mbun::ast::NodeKind::NumberLiteral) {
            return false;
        }
        if constexpr (requires { self_().number_property_key_must_be_computed(key); }) {
            return self_().number_property_key_must_be_computed(key);
        } else {
            return false;
        }
    }

    // DEFERRED(1). ref :4843-4860 — a method's `async` / `*` prefixes, which bun
    // reads off the VALUE function's flags, and :4880/:5017 `print_func`. Returns
    // true when it printed the whole method (bun `return`s there).
    [[nodiscard]] bool try_print_method_(mbun::ast::NodeIndex value, std::uint8_t flags) {
        if (value == mbun::ast::NONE || (flags & mbun::ast::pflags::IsMethod) == 0) {
            return false;
        }
        if (node_(value).kind != mbun::ast::NodeKind::FunctionExpr) {
            return false;  // bun's guard is `if let ExprData::EFunction(func)`
        }
        if constexpr (requires(mbun::ast::NodeIndex v) { self_().print_func(v); }) {
            self_().print_func(value);
            return true;
        } else {
            return false;
        }
    }

public:
    // ref :5045 — `= <expr>`. bun defines this immediately after print_property
    // and it is its only caller today.
    void print_initializer(mbun::ast::NodeIndex initial) {
        if (initial == mbun::ast::NONE) {
            return;
        }
        self_().print_space();
        self_().print('=');
        self_().print_space();
        self_().print_expr(initial, Level::Comma, expr_flag::none());
    }

    // ref :4743
    void print_property(mbun::ast::NodeIndex prop) {
        if (prop == mbun::ast::NONE) {
            return;
        }
        const mbun::ast::Node& n { node_(prop) };
        const mbun::ast::PropertyKind kind { static_cast<mbun::ast::PropertyKind>(n.aux) };
        const mbun::ast::NodeIndex key { n.a };
        const mbun::ast::NodeIndex value { n.b };
        const mbun::ast::NodeIndex init { n.c };
        // ref :4745-4756 — bun shallow-copies the property because it REWRITES
        // `flags` (and `key`) below; the copy is what makes that non-destructive.
        // Only `flags` is rewritten by the arms this port keeps, so only flags is
        // copied. ⚠️ `n` is a reference INTO the arena's node vector — nothing in
        // this printer mutates the arena, so it stays valid across the walk.
        std::uint8_t flags { n.flags };

        if constexpr (!D::flags().isJson) {
            // ref :4759 — `{...rest}`. Keyless by construction (g.rs:158).
            if (kind == mbun::ast::PropertyKind::Spread) {
                self_().print("...");
                self_().print_expr(value, Level::Comma, expr_flag::none());
                return;
            }

            // DEFERRED(2): the minify_syntax enum-inlining key rewrite (:4770-4810).

            // ref :4812
            if ((flags & mbun::ast::pflags::IsStatic) != 0) {
                self_().print("static");
                self_().print_space();
            }

            // ref :4816-4838
            switch (kind) {
            case mbun::ast::PropertyKind::Get:
                self_().print_space_before_identifier();
                self_().print("get");
                self_().print_space();
                break;
            case mbun::ast::PropertyKind::Set:
                self_().print_space_before_identifier();
                self_().print("set");
                self_().print_space();
                break;
            case mbun::ast::PropertyKind::AutoAccessor:
                self_().print_space_before_identifier();
                self_().print("accessor");
                self_().print_space();
                break;
            default:
                break;
            }

            if (value != mbun::ast::NONE) {
                // ref :4838-4853 — a method's `async` / `*` prefixes. bun reads
                // them off the VALUE FUNCTION's flags, never off the Property:
                // there is no `flags::Property::IsAsync` in bun's set
                // (ast/lib.rs:3283-3287), and inventing one here would be a model
                // bun does not have. The parser now builds the value function for
                // a method shorthand and puts IsAsync/IsGenerator on it exactly
                // where g.rs:285 keeps them, so this reads back what bun reads.
                //
                // ⚠️ The space rule is bun's and looks wrong until you check it:
                // `async` and `*` each print with NO trailing space, and a space
                // is printed only when BOTH are present (:4846-4851). So
                // `{async *ag(){}}` prints `async* ag` — `async`, `*`, space —
                // NOT `async *ag`. Verified against real bun 1.3.14, which prints
                // `{ async* ag() {} }`. The space before a lone `async`'s key
                // comes from print_identifier's print_space_before_identifier,
                // not from here.
                if (node_(value).kind == mbun::ast::NodeKind::FunctionExpr &&
                    (flags & mbun::ast::pflags::IsMethod) != 0) {
                    const std::uint8_t fnf { node_(value).flags };
                    const bool isAsync { (fnf & mbun::ast::fnflags::IsAsync) != 0 };
                    const bool isGenerator { (fnf & mbun::ast::fnflags::IsGenerator) != 0 };
                    if (isAsync) {  // ref :4840-4843
                        self_().print_space_before_identifier();
                        self_().print("async");
                    }
                    if (isGenerator) {  // ref :4844-4846
                        self_().print('*');
                    }
                    if (isGenerator && isAsync) {  // ref :4847-4851
                        self_().print_space();
                    }
                }

                // ref :4862 — "If var is declared in a parent scope and var is then
                // written via destructuring pattern, key is null".
                if (key == mbun::ast::NONE) {
                    self_().print_expr(value, Level::Comma, expr_flag::none());
                    return;
                }
            }
        }

        // ref :4906 `item.key.expect("infallible: prop has key")`. See DEFERRED(5):
        // mbun CAN hand us a keyless non-spread Property (every class member is
        // one), so where bun unwraps, this returns — printing nothing beats
        // panicking on input bun's type system cannot produce.
        if (key == mbun::ast::NONE) {
            return;
        }

        // ref :4890
        if constexpr (!D::flags().isJson) {
            if ((flags & mbun::ast::pflags::IsComputed) == 0 && key_must_be_computed_(key)) {
                flags |= mbun::ast::pflags::IsComputed;
            }
        }

        // ref :4869 — the computed-key arm.
        if constexpr (!D::flags().isJson) {
            if ((flags & mbun::ast::pflags::IsComputed) != 0) {
                self_().print('[');
                self_().print_expr(key, Level::Comma, expr_flag::none());
                self_().print(']');

                if (value != mbun::ast::NONE) {
                    if (try_print_method_(value, flags)) {  // ref :4880
                        return;
                    }
                    self_().print(':');
                    self_().print_space();
                    self_().print_expr(value, Level::Comma, expr_flag::none());
                }
                print_initializer(init);  // ref :4886
                return;
            }
        }

        // ── the key ─────────────────────────────────────────────────────────
        // ref :4906-5004. bun matches on `EPrivateIdentifier` / `EString` / `_`.
        // mbun splits what bun's EString covers across two node kinds — an
        // identifier-like key is a bare Identifier, a quoted key is a StringLiteral
        // whose text is the source slice — exactly as binding.cppm:337-345 records.
        const mbun::ast::NodeKind keyKind { node_(key).kind };
        bool allowShorthand { true };
        if (keyKind == mbun::ast::NodeKind::Identifier) {
            // ref :4906-4919 — the utf8 EString arm.
            const std::string_view name { name_of_(key) };
            self_().add_source_mapping(key);
            if constexpr (!D::flags().isJson) {
                if (is_identifier(name)) {
                    self_().print_space_before_identifier();
                    self_().print_identifier(name);
                } else {
                    allowShorthand = false;
                    print_quoted_key_(name);  // ref :4917 — `{"a-b": x}`
                }
            } else {
                allowShorthand = false;
                print_quoted_key_(name);
            }

            // ref :4921-4932 — "Use a shorthand property if the names are the
            // same". ⚠️ bun compares the key text against the value's RESOLVED
            // SYMBOL NAME, not against the parser's WasShorthand flag: a renamer
            // that rewrites `a` to `a$1` must UNFOLD `{a}` back into `{a: a$1}`.
            // So the flag the parser recorded is deliberately not consulted here —
            // the names are. (mbun has no renamer, so this reduces to the flag
            // today; it will not once one lands. Same reasoning as binding.cppm:355.)
            if (value != mbun::ast::NONE &&
                node_(value).kind == mbun::ast::NodeKind::Identifier &&
                name_of_(value) == name) {
                print_initializer(init);  // ref :4924
                if (allowShorthand) {
                    return;
                }
            }
        } else {
            // ref :5000 — the `_ =>` arm: any other key data prints as an
            // expression at Level::Lowest. mbun's StringLiteral/NumberLiteral keys
            // land here, which is what binding.cppm:369 does with the same two.
            self_().print_expr(key, Level::Lowest, expr_flag::none());
        }

        // ref :5006-5020 — a Get/Set (never Normal/AutoAccessor) prints its value
        // function directly, with no `:`; then the generic method arm; then the
        // plain `key: value`.
        if (kind != mbun::ast::PropertyKind::Normal &&
            kind != mbun::ast::PropertyKind::AutoAccessor) {
            // DEFERRED(1): bun `print_func`s the value here (:5013) and returns.
            if (value != mbun::ast::NONE &&
                node_(value).kind == mbun::ast::NodeKind::FunctionExpr) {
                if constexpr (requires(mbun::ast::NodeIndex v) { self_().print_func(v); }) {
                    self_().print_func(value);
                    return;
                }
            }
        }

        if (value != mbun::ast::NONE) {
            if (try_print_method_(value, flags)) {  // ref :5021
                return;
            }
            self_().print(':');
            self_().print_space();
            self_().print_expr(value, Level::Comma, expr_flag::none());
        }

        // ref :5040 — in JSON a property has no initializer (bun debug-asserts it).
        print_initializer(init);
    }
};

}  // namespace mbun::js_printer
