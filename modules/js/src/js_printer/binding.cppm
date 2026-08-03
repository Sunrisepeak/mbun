// src/js_printer/binding.cppm — module mbun.js_printer.binding
//
// CAP-BUILD-PRINTER shard 4/4 — the binding (destructuring pattern) printer.
//
// A 1:1 port of the `BindingData::*` half of bun's `Printer` impl block, per
// AGENTS.md 「移植三段法」. Blueprint: .mbun/bun-ref/src/js_printer/lib.rs
//
//   :5052  `print_binding`                    — the match this file is built around
//   :5280  `maybe_print_default_binding_value`
//   :5148  the computed-key arm    :5163  the EString-key arm (shorthand folding)
//
// The shard contract is at the top of printer_core.cppm — read it first. In
// short: CRTP mixin, cross-shard calls go through `self_()`. This file calls
// shard 2's `print_expr` / `print_identifier` / `print_string_literal_utf8` and
// shard 3's `arena()` / `node()` / `print_space_before_identifier` /
// `add_source_mapping` (stmt.cppm:24-49 homes those three, provisionally) — none
// of which this file may redefine: two definitions in two mixins is an ambiguous
// base at assembly.
//
// ═════════════════════════════════════════════════════════════════════════════
// The AST mapping — bun's `B` union onto mbun's flat arena (ast.cppm:71-76)
// ═════════════════════════════════════════════════════════════════════════════
//   b.rs B::BIdentifier{ref}      -> BindingIdentifier  text=name
//   b.rs b::Array{items,          -> BindingArray       list=BindingElement
//                 has_spread,                           flags HasSpread|IsSingleLine
//                 is_single_line}
//   b.rs b::Object{properties,    -> BindingObject      list=BindingProperty
//                  is_single_line}                      flags IsSingleLine
//   nodes.rs ArrayBinding{binding,-> BindingElement     a=binding b=default
//                         default_value}
//   b.rs:68 b::Property{flags,key,-> BindingProperty    a=key b=value c=default
//                       value,default}                  flags IsComputed|IsSpread|
//                                                             WasShorthand
//   b.rs B::BMissing              -> BindingMissing     (an array hole)
//
// ⚠️ The array/object spread asymmetry is bun's and is deliberate (ast.cppm:198):
// an array carries `has_spread` at the ARRAY level and the printer asks
// `has_spread && is_last` (:5104, mirroring binding.rs:230), while an object tags
// spread PER PROPERTY (`flags::Property::IsSpread`, :5142 / binding.rs:258). Do
// not "unify" them — the two models stay 1:1 so neither side needs a translation
// table.
//
// ═════════════════════════════════════════════════════════════════════════════
// DEFERRED — with reasons, not as an excuse
// ═════════════════════════════════════════════════════════════════════════════
//  1. `print_symbol(b.ref)` (:5063) -> `print_identifier(text)`. bun resolves a
//     `Ref` through the symbol table so the RENAMER can rename it; mbun's parser
//     records the source name on the node and has no symbol table, so there is no
//     Ref to resolve and nothing to rename. See the renamer note below.
//  2. `MAY_HAVE_MODULE_INFO` (:5065-5075, :5185-5199, :5225-5240). Every arm of
//     bun's print_binding that binds a name also feeds `module_info` — add_var
//     for a top-level declaration, add_export_info_local for an export. mbun has
//     no `analyze_transpiled_module` producer (flags.cppm:126 already models the
//     `tlm` shape and defers the same thing), so `tlm` is threaded to every site
//     bun threads it to and consumed nowhere. Keeping the parameter is the point:
//     when ModuleInfo lands it is a body-fill, not an archaeology dig.
//  3. `number_property_key_must_be_computed` (:5147) — "{ -1: 0 }" must print as
//     "{ [-1]: 0 }". It matches on `ExprData::ENumber` and belongs to shard 2
//     alongside the rest of the ExprData dispatch; :5147 calls it through `self`,
//     so this file calls `self_().number_property_key_must_be_computed(key)` and
//     shard 2 owns the predicate. NOT reachable from mbun's parser today: a
//     numeric key is parsed as a NumberLiteral whose text is the source slice, so
//     there is no folded value to test. Gated behind `requires` so this file
//     compiles before shard 2 lands it.
//  4. `stack_check.is_safe_to_recurse()` (:5053). bun guards every recursive
//     printer entry against stack overflow on adversarial nesting. printer_core
//     has no `stack_check` yet; the guard belongs there (it is `print_stmt`'s and
//     `print_expr`'s problem too, not just this file's), so it is not invented
//     here. Tracked at the same place shard 3 left it.
//  5. `can_print_identifier_utf16` / `print_identifier_utf16` (:5205-5215). mbun
//     stores key text as UTF-8 source slices; there is no UTF-16 string in the
//     arena to take that branch. The UTF-8 branch (:5170) is the one that runs.
//
// ⚠️ NOT wired into `mbun::js_parser::transpile` — transpile is erasure-based and
// this printer prints nothing in production yet. See printer.cppm.
export module mbun.js_printer.binding;

import std;
import mbun.ast;
import mbun.js_printer.flags;
import mbun.js_printer.op_level;

export namespace mbun::js_printer {

// ─────────────────────────────────────────────────────────────────────────────
// is_identifier — ref lexer.rs `is_identifier`, as called at lib.rs:5172 to
// decide whether an EString key can be printed bare (`{a: x}`) or has to be
// quoted (`{"a-b": x}`).
//
// bun asks the full Unicode ID_Start/ID_Continue tables. This asks the ASCII
// subset plus `$`/`_`, and — deliberately — treats every byte >= 0x80 as an
// identifier part. That is the same latin1-shaped answer `print_identifier`'s
// own gate gives (printer_core.cppm:110 documents the verified 1.3.14 behaviour:
// a non-ASCII identifier is left alone unless target:"bun"), so a key like
// `héllo` stays a bare identifier here exactly as it does there. A key that
// needs quoting for a reason ASCII cannot see (an unpaired surrogate, say)
// cannot reach this path: mbun's parser only ever produces UTF-8 source slices.
[[nodiscard]] constexpr bool is_identifier_start_byte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || c >= 0x80;
}

[[nodiscard]] constexpr bool is_identifier(std::string_view s) {
    if (s.empty()) {
        return false;
    }
    if (!is_identifier_start_byte(static_cast<unsigned char>(s.front()))) {
        return false;
    }
    for (const char ch : s.substr(1)) {
        const unsigned char c { static_cast<unsigned char>(ch) };
        if (!is_identifier_start_byte(c) && !(c >= '0' && c <= '9')) {
            return false;
        }
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// BindingPrinter — ref lib.rs:5052 `print_binding` + :5280
// `maybe_print_default_binding_value`.
// ─────────────────────────────────────────────────────────────────────────────
template <typename D>
class BindingPrinter {
private:
    [[nodiscard]] D& self_() { return static_cast<D&>(*this); }

    [[nodiscard]] const mbun::ast::Node& node_(mbun::ast::NodeIndex i) {
        return self_().arena().at(i);
    }

    // ref :5280 — `= <expr>` after a binding target. The default hangs off the
    // BindingElement (`b`) / BindingProperty (`c`); bun reaches it through the
    // `HasDefaultValue` trait, which is the same one slot by another name.
    void maybe_print_default_binding_value_(mbun::ast::NodeIndex def) {
        if (def == mbun::ast::NONE) {
            return;
        }
        self_().print_space();
        self_().print('=');
        self_().print_space();
        self_().print_expr(def, Level::Comma, expr_flag::none());
    }

    // ref :5147 — "Automatically print numbers that would cause a syntax error
    // as computed properties". Shard 2 owns the predicate (see DEFERRED 3); until
    // it exists, no key forces itself computed, which is exactly the answer for
    // every key mbun's parser can currently produce.
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

public:
    // ref :5052
    void print_binding(mbun::ast::NodeIndex binding, TopLevelAndIsExport tlm) {
        // DEFERRED(4): bun guards recursion here (:5053).
        if (binding == mbun::ast::NONE) {
            return;
        }
        switch (node_(binding).kind) {
        // ref :5060 — `BindingData::BMissing(_) => {}`. An array hole prints as
        // nothing at all; the comma that delimits it is the array arm's job.
        case mbun::ast::NodeKind::BindingMissing:
            return;

        // ref :5061
        case mbun::ast::NodeKind::BindingIdentifier:
            print_binding_identifier_(binding, tlm);
            return;

        // ref :5079
        case mbun::ast::NodeKind::BindingArray:
            print_binding_array_(binding, tlm);
            return;

        // ref :5125
        case mbun::ast::NodeKind::BindingObject:
            print_binding_object_(binding, tlm);
            return;

        default:
            // Not a binding. bun's `B` union cannot represent this — the type
            // system forbids it — so there is no blueprint behaviour to port.
            // Printing nothing is the honest answer: silently emitting the node
            // as an expression would paper over a parser bug at the one place a
            // test would notice it.
            return;
        }
    }

private:
    // ref :5061-5077
    void print_binding_identifier_(mbun::ast::NodeIndex binding, TopLevelAndIsExport tlm) {
        self_().print_space_before_identifier();
        self_().add_source_mapping(binding);
        // DEFERRED(1): bun is `print_symbol(b.ref)`.
        self_().print_identifier(node_(binding).text);
        // DEFERRED(2): bun feeds module_info here (:5065-5075).
        (void)tlm;
    }

    // ref :5079-5122
    void print_binding_array_(mbun::ast::NodeIndex binding, TopLevelAndIsExport tlm) {
        const bool isSingleLine { (node_(binding).flags & mbun::ast::bflags::IsSingleLine) != 0 };
        const bool hasSpread { (node_(binding).flags & mbun::ast::bflags::HasSpread) != 0 };
        // The span is read once: `print_binding` recurses, but nothing in this
        // printer mutates the arena, so the list stays valid across the walk.
        const std::span<const mbun::ast::NodeIndex> items { self_().arena().list_of(
            node_(binding)) };

        self_().print('[');
        if (!items.empty()) {
            if (!isSingleLine) {
                self_().indent();
            }

            for (std::size_t i { 0 }; i < items.size(); ++i) {
                if (i != 0) {
                    self_().print(',');
                    if (isSingleLine) {
                        self_().print_space();
                    }
                }
                if (!isSingleLine) {
                    self_().print_newline();
                    self_().print_indent();
                }

                const bool isLast { i + 1 == items.size() };
                // ref :5104 / binding.rs:230 — array-level has_spread, asked of
                // the last element. There is no per-element spread flag to read.
                if (hasSpread && isLast) {
                    self_().print("...");
                }

                const mbun::ast::NodeIndex item { items[i] };
                // A hole is a bare BindingMissing; every other item is a
                // BindingElement wrapping (target, default).
                const bool isHole { node_(item).kind == mbun::ast::NodeKind::BindingMissing };
                if (isHole) {
                    print_binding(item, tlm);
                } else {
                    print_binding(node_(item).a, tlm);
                    maybe_print_default_binding_value_(node_(item).b);
                }

                // ref :5111 — "Make sure there's a comma after trailing missing
                // items": `[a, ,]` must not print as `[a, ]`, which is a
                // one-element array.
                if (isLast && isHole) {
                    self_().print(',');
                }
            }

            if (!isSingleLine) {
                self_().unindent();
                self_().print_newline();
                self_().print_indent();
            }
        }
        self_().print(']');
    }

    // ref :5125-5275
    void print_binding_object_(mbun::ast::NodeIndex binding, TopLevelAndIsExport tlm) {
        const bool isSingleLine { (node_(binding).flags & mbun::ast::bflags::IsSingleLine) != 0 };
        const std::span<const mbun::ast::NodeIndex> properties { self_().arena().list_of(
            node_(binding)) };

        self_().print('{');
        if (!properties.empty()) {
            if (!isSingleLine) {
                self_().indent();
            }

            for (std::size_t i { 0 }; i < properties.size(); ++i) {
                if (i != 0) {
                    self_().print(',');
                }
                // ⚠️ ref :5136-5141 — an object's separator logic is NOT the
                // array's: single-line prints a space BEFORE every property
                // (including the first, which is what gives `{ a }` its inner
                // padding), where the array prints one only between elements.
                if (isSingleLine) {
                    self_().print_space();
                } else {
                    self_().print_newline();
                    self_().print_indent();
                }

                print_binding_property_(properties[i], tlm);
            }

            if (!isSingleLine) {
                self_().unindent();
                self_().print_newline();
                self_().print_indent();
            } else {
                self_().print_space();  // ref :5270 — the closing `{ a }` pad
            }
        }
        self_().print('}');
    }

    // One `b::Property` — ref :5142-5266.
    void print_binding_property_(mbun::ast::NodeIndex property, TopLevelAndIsExport tlm) {
        const std::uint8_t flags { node_(property).flags };
        const mbun::ast::NodeIndex key { node_(property).a };
        const mbun::ast::NodeIndex value { node_(property).b };
        const mbun::ast::NodeIndex def { node_(property).c };

        // ref :5142 — object rest is per-property (binding.rs:258).
        if ((flags & mbun::ast::bflags::IsSpread) != 0) {
            self_().print("...");
            print_binding(value, tlm);
            maybe_print_default_binding_value_(def);
            return;
        }

        // ref :5147-5167 — a computed key, or a number that must become one.
        if ((flags & mbun::ast::bflags::IsComputed) != 0 || key_must_be_computed_(key)) {
            self_().print('[');
            self_().print_expr(key, Level::Comma, expr_flag::none());
            self_().print("]:");
            self_().print_space();
            print_binding(value, tlm);
            maybe_print_default_binding_value_(def);
            return;
        }

        // ref :5169-5245 — the EString-key arm. mbun splits what bun's EString
        // covers across two node kinds: an identifier-like key keeps its name in
        // `text` (a bare Identifier node), a quoted key is a StringLiteral whose
        // text is the source slice. Both are bun's `EString`; the question bun
        // asks of it — "is this a valid identifier?" — is the same one.
        const mbun::ast::NodeKind keyKind { node_(key).kind };
        if (keyKind == mbun::ast::NodeKind::Identifier) {
            const std::string_view name { node_(key).text };
            self_().add_source_mapping(key);
            if (is_identifier(name)) {
                self_().print_space_before_identifier();
                self_().print_identifier(name);

                // ref :5175-5202 — "Use a shorthand property if the names are
                // the same". bun compares the KEY TEXT against the target's
                // RESOLVED symbol name, not against the parser's shorthand flag:
                // a renamer that renames `a` to `a$1` must turn `{a}` back into
                // `{a: a$1}`. So the flag the parser recorded is not consulted
                // here — the names are. (mbun has no renamer yet, so today this
                // reduces to the flag; it will not once one lands. See the
                // renamer note at the bottom of this file.)
                if (node_(value).kind == mbun::ast::NodeKind::BindingIdentifier &&
                    node_(value).text == name) {
                    // DEFERRED(2): module_info (:5185-5199).
                    maybe_print_default_binding_value_(def);
                    return;
                }
            } else {
                // ref :5180 — `{"a-b": x}`: not an identifier, so quote it.
                self_().print_string_literal_utf8(name, /*allowBacktick=*/false);
            }
        } else if (keyKind == mbun::ast::NodeKind::StringLiteral ||
                   keyKind == mbun::ast::NodeKind::NumberLiteral) {
            // ref :5248 — the `_ =>` arm: any other key data prints as an
            // expression at Level::Lowest.
            self_().print_expr(key, Level::Lowest, expr_flag::none());
        } else {
            self_().print_expr(key, Level::Lowest, expr_flag::none());
        }

        // ref :5259
        self_().print(':');
        self_().print_space();
        print_binding(value, tlm);
        maybe_print_default_binding_value_(def);
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// print_property (lib.rs:4743) — NOT ported: blocked on the AST, and on what
// ═════════════════════════════════════════════════════════════════════════════
// This shard was scoped as `print_binding` + `print_property` + `renamer`.
// print_property is the OBJECT-LITERAL / CLASS-MEMBER property printer (bun's
// `G::Property`, g.rs:143) — a different type from the `b::Property` this file
// prints (b.rs:68), sharing only a name. Shard 2 deferred the `ObjectLiteral`
// arm of print_expr to it (expr.cppm:696, :1127), so it is genuinely the last
// hole in the printer. It is not ported because mbun's AST cannot represent its
// input, not because of scope fatigue:
//
//   * bun's `G::Property` is {initializer, kind, flags, key, value,
//     class_static_block, ts_decorators} (g.rs:143-157). mbun's Property node is
//     `a=key b=value` and nothing else (ast.cppm:46).
//   * print_property is almost entirely a function OF the fields mbun lacks:
//     `kind` drives the Spread/Get/Set/AutoAccessor arms (:4759, :4816-4838),
//     `flags` drives IsStatic (:4812), IsComputed (:4869), IsMethod (:4843),
//     and `initializer` drives :4890. mbun's parser PARSES `get`/`set`/`async`/
//     method shorthand (js_parser.cppm:4192) and then discards every bit of it —
//     a getter and a plain property build the identical node. So the port has no
//     input to read: it would be a transcription against an invented model, the
//     same 黑盒臆测 核心原则 ② forbids and the same reason renamer.rs is parked
//     below.
//   * The unblock is the sequence this shard just walked for bindings, which is
//     the precedent to copy: the AST agent added the 6 Binding* nodes (a109e13a)
//     -> the parser built the tree -> the printer walked it. Here that is:
//     (1) ast.cppm grows `PropertyKind` (g.rs:160) + `flags::Property` (the same
//     bitset ast.cppm:198 already cites for bindings) + an initializer slot;
//     (2) js_parser records them where it currently drops them; (3) this port
//     becomes mechanical. Step (1) is the AST's to make — this shard is scoped
//     out of ast.cppm — and steps (2)/(3) are cheap once it exists.
//
// ═════════════════════════════════════════════════════════════════════════════
// renamer.rs — NOT ported, and why
// ═════════════════════════════════════════════════════════════════════════════
// The renamer (.mbun/bun-ref/src/js_printer/renamer.rs, 1196 lines) is not
// ported because it has no subject to act on, not because it is large:
//
//   * Every entry point takes a `Ref` and a `SymbolTable`: `NumberRenamer`
//     (:339) and `MinifyRenamer` (:695) both exist to answer
//     `name_for_symbol(ref) -> &str`, and `assign_nested_scope_slots` (:1064)
//     walks `p.scopes` assigning slots. mbun's parser builds no symbol table, no
//     scopes, and no Refs — a binding identifier carries its source name as text
//     (ast.cppm:71). There is nothing to rename, and no Ref to rename it by.
//   * So a port today would be 1196 lines against a fabricated symbol model,
//     which is exactly the "黑盒臆测" AGENTS.md 核心原则 ② forbids: the shape
//     would be invented here and every later consumer would inherit the guess.
//
// What this file does instead is leave the SEAM in the right place, so the port
// is a substitution and not a rewrite. Both places bun consults the symbol table
// from print_binding are marked and behave correctly under a future renamer:
//   * `print_binding_identifier_` calls `print_identifier(text)` where bun calls
//     `print_symbol(ref)` — one call site to change.
//   * The shorthand fold compares the key text against the TARGET'S NAME (:5178),
//     as bun does, rather than trusting the parser's WasShorthand flag. That is
//     the distinction that matters: the day a renamer renames `a` to `a$1`,
//     `{a}` must print as `{a: a$1}`, and this comparison already says so.
// The prerequisite is a symbol table in mbun.ast + scope tracking in the parser
// (bun: `p.scopes` / `Symbol::Map`). That is a parser capability, not a printer
// one, and it is the thing to build before renamer.rs is worth translating.
// ═════════════════════════════════════════════════════════════════════════════

}  // namespace mbun::js_printer
