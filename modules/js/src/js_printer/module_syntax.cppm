// src/js_printer/module_syntax.cppm — module mbun.js_printer.module_syntax
//
// print_import_decl / print_export_decl — the ESM statement forms.
//
// Blueprint (.mbun/bun-ref/src/js_printer/lib.rs):
//   :2000  `print_import_statement`   (S::Import,       s.rs:198)
//   :3065  `print_clause_item` / :3077 `print_clause_item_as` + :1521 ClauseItemAs
//   :2478  `print_clause_alias`
//   :6748 / :5318 / :5372            `export <declaration>`  → stmt.cppm/func.cppm
//
// ── Why this file exists ─────────────────────────────────────────────────────
// stmt.cppm's header carried a long MODULE-SYNTAX AST-GAP note explaining that
// `import` could not be printed from this arena at all: the parser threw the
// default binding, the namespace, the specifiers and the module path away into
// C++ locals and returned a node with only kind/start/end. That gap is closed on
// the AST side (ast.cppm's `── ESM ──` section); this is the printer half. Own
// file rather than more of stmt.cppm for the reason func.cppm has one — stmt.cppm
// is the biggest shard already, and these arms are a self-contained subsystem.
//
// ── The one thing to understand before editing ───────────────────────────────
// bun's three clause arms (Import / Export / ExportFrom, lib.rs:3081-3125) look
// like three different rules and are not — not for a TRANSPILER. Each one
// compares a LOCAL name against an EXTERNAL name and prints `L`, or `L as R` when
// they differ; they disagree only on which side the symbol table supplies:
//
//     Import      print alias      , ` as ` + name_for_symbol(name)  if differ
//     Export      print name_for_symbol(name), ` as ` + alias        if differ
//     ExportFrom  print original_name (or name), ` as ` + alias      if differ
//
// `name_for_symbol(ref)` (lib.rs:2472) is `renamer.name_for_symbol`, and with no
// renamer that is just the name the source wrote. So all three collapse to ONE
// rule over the two names mbun stores in source order — print L, then ` as R` if
// the two differ. VERIFIED against real bun 1.4.0, `Bun.Transpiler.transformSync`,
// which is the oracle this file is written against:
//
//     import {a as b} from 'y'   =>  import { a as b } from "y";
//     import {a as a} from 'y'   =>  import { a } from "y";        // ← R==L folds
//     import {'a' as a} from 'y' =>  import { a } from "y";        // ← by CONTENT
//     export {a as 'b'} from 'y' =>  export { a as b } from "y";   // ← alias unquotes
//
// ⚠️ Use `Bun.Transpiler`, NOT `bun build --no-bundle`, to check anything here.
// `bun build` is a BUNDLER: it tree-shakes (`import d from 'y'` with `d` unused
// prints ""), folds and renames. Two agents independently concluded from it that
// imports need a symbol table — they do not, and encoding a bundler's output into
// this printer's expectations would be a days-long wrong turn.
//
// The three arms are kept SEPARATE below even though they are currently
// identical, because the day a renamer lands they stop being identical and this
// is where each one's seam has to be.
export module mbun.js_printer.module_syntax;

import std;
import mbun.ast;
import mbun.js_printer.flags;
import mbun.js_printer.whitespacer;  // ws<"from "> — ref :6235 `ws!(b"from ")`
import mbun.js_printer.op_level;  // Level — the `export default <expr>` arm's precedence
import mbun.js_printer.binding;  // `is_identifier` (binding.cppm:102) — reused, not re-derived

export namespace mbun::js_printer {

// ─────────────────────────────────────────────────────────────────────────────
// ModulePrinter — the ESM arms of print_stmt, as a CRTP mixin
// (printer_core.cppm:53-62 documents the contract).
// ─────────────────────────────────────────────────────────────────────────────
template <class D>
class ModulePrinter {
private:
    [[nodiscard]] D& self_() { return static_cast<D&>(*this); }

    [[nodiscard]] const mbun::ast::Node& node_(mbun::ast::NodeIndex i) { return self_().node(i); }

    // The NAME a clause/namespace name node carries, decoded.
    //
    // An Identifier's text is the name. A StringLiteral's is the source slice
    // WITH its quotes (`import { "a-b" as c }`), and bun compares and prints the
    // CONTENT — `import {'a' as a}` folds to `import { a }`, which only happens if
    // `'a'` has become `a` first.
    //
    // ⚠️ DEFERRED: escapes. A module-export string may legally contain any escape
    // (`import { "abc" as x }`), and decoding those needs the lexer's cooked
    // value, which the parser does not put on the node. Stripping the quotes is
    // exact for every string WITHOUT a backslash and is what ships; a string with
    // one is left with its escape verbatim, which prints a still-valid — if
    // needlessly quoted — specifier rather than a wrong name. Escaped
    // module-export strings do not occur in the 9395-file corpus.
    [[nodiscard]] std::string_view name_of_(mbun::ast::NodeIndex i) {
        const mbun::ast::Node& n { node_(i) };
        if (n.kind != mbun::ast::NodeKind::StringLiteral) {
            return n.text;
        }
        std::string_view raw { n.text };
        if (raw.size() >= 2 && (raw.front() == '"' || raw.front() == '\'')
            && raw.back() == raw.front() && raw.find('\\') == std::string_view::npos) {
            return raw.substr(1, raw.size() - 2);
        }
        return raw;
    }

    // ref :2478 `print_clause_alias` — a name that is a valid identifier prints
    // bare, anything else prints as a quoted string. Same shape as an object key
    // (property.cppm:361), and the same `is_identifier` answers it.
    void print_clause_alias_(std::string_view alias) {
        if (is_identifier(alias)) {
            self_().print_space_before_identifier();
            self_().print_identifier(alias);
        } else {
            self_().print('"');
            self_().print_string_characters_utf8(alias, '"');
            self_().print('"');
        }
    }

    // The module specifier. `text` is the RAW source slice with its original
    // quotes (`'y'`); bun re-quotes every string it prints through its own
    // best-quote logic, which is why `import d from 'y'` comes back double-quoted.
    void print_module_path_(std::string_view raw) {
        std::string_view inner { raw };
        if (inner.size() >= 2 && (inner.front() == '"' || inner.front() == '\'')
            && inner.back() == inner.front()) {
            inner = inner.substr(1, inner.size() - 2);
        }
        // ⚠️ Same DEFERRED as name_of_: a path holding an escape keeps it verbatim
        // rather than being re-encoded from a cooked value the node does not carry.
        // Re-quoting to `"` unconditionally (rather than asking best_quote_char) is
        // what bun does here — `import d from "y'z"` stays double-quoted (verified).
        self_().print('"');
        self_().print_string_characters_utf8(inner, '"');
        self_().print('"');
    }

    // ── the clause `{ … }` ───────────────────────────────────────────────────
    // ref :2033-2070 (import) / :6210-6260 (export). `is_single_line` picks the
    // layout exactly as it does for an object literal: `{ a, b }` vs one indented
    // specifier per line.
    // Prints `{ … }` inclusive. Structure is bun's, line for line (:6187-6220 for
    // import, :5766-5796 for export-from — the two loops are identical).
    //
    // The empty clause needs NO special case, which is the tell that this is the
    // right shape: with no items the single-line pre/post spaces land against each
    // other, so `export {} from 'y'` prints `export {  } from "y";` — two spaces,
    // exactly as bun 1.4.0 does (verified). An empty IMPORT clause never reaches
    // here at all; print_import_decl's item_count gate drops it first.
    template <class F>
    void print_clause_(std::span<const mbun::ast::NodeIndex> items, bool singleLine, F&& printItem) {
        self_().print('{');
        if (!singleLine) {
            self_().indent();
        } else {
            self_().print_space();
        }
        for (std::size_t i { 0 }; i < items.size(); ++i) {
            if (i != 0) {
                self_().print(',');
                if (singleLine) {
                    self_().print_space();
                }
            }
            if (!singleLine) {
                self_().print_newline();
                self_().print_indent();
            }
            printItem(items[i]);
        }
        if (!singleLine) {
            self_().unindent();
            self_().print_newline();
            self_().print_indent();
        } else {
            self_().print_space();
        }
        self_().print('}');
    }

    // L, then ` as R` when the two names DIFFER. See the file header: this is all
    // three of bun's arms once `name_for_symbol` is the identity.
    void print_clause_pair_(mbun::ast::NodeIndex item) {
        const mbun::ast::Node& ci { node_(item) };
        const std::string_view l { name_of_(ci.a) };
        const std::string_view r { ci.b == mbun::ast::NONE ? l : name_of_(ci.b) };
        print_clause_alias_(l);
        if (l != r) {
            self_().print(" as ");
            print_clause_alias_(r);
        }
    }

public:
    // ref :3065 / :3077 ClauseItemAs::Import. Split from Export/ExportFrom on
    // purpose — see the file header.
    void print_clause_item(mbun::ast::NodeIndex item) { print_clause_pair_(item); }
    void print_export_clause_item(mbun::ast::NodeIndex item) { print_clause_pair_(item); }
    void print_export_from_clause_item(mbun::ast::NodeIndex item) { print_clause_pair_(item); }

    // ─────────────────────────────────────────────────────────────────────────
    // print_import_decl — ref :2000 `print_import_statement`
    // ─────────────────────────────────────────────────────────────────────────
    void print_import_decl(mbun::ast::NodeIndex s) {
        const mbun::ast::Node& n { node_(s) };

        // A type-only import is erased whole (`import type {T} from 'y'` => ""),
        // and TS import-equals has already been rewritten to a `const` by the
        // erasure path and has no model here (ast.cppm mflags::IsTsImportEquals).
        if ((n.flags & (mbun::ast::mflags::IsTypeOnly | mbun::ast::mflags::IsTsImportEquals)) != 0) {
            return;
        }

        self_().print_indent();
        self_().print_space_before_identifier();
        self_().add_source_mapping(s);
        self_().print("import");

        const std::span<const mbun::ast::NodeIndex> items { self_().arena().list_of(n) };
        const bool hasStar { n.b != mbun::ast::NONE };
        // ref :6187 — an EMPTY clause collapses to the bare form, because bun's
        // `item_count` only counts a clause that HAS items. Both verified (1.4.0):
        //     import {} from 'y'     =>  import"y";
        //     import d, {} from 'y'  =>  import d from "y";
        // which is why every test below asks about the ITEMS, never the braces.
        const bool hasClause { !items.empty() };

        // ref :6176 `item_count`. Order is bun's: default, then the clause, then
        // the star — NOT default/star/clause. A clause and a star cannot coexist
        // grammatically (s.rs:191 says so outright), so the order is unobservable
        // today; it follows the blueprint anyway so the next reader diffing this
        // against lib.rs:6160 finds the same shape.
        int itemCount { 0 };

        if (n.a != mbun::ast::NONE) {  // ref :6178
            self_().print(' ');
            print_clause_alias_(name_of_(n.a));
            ++itemCount;
        }

        if (hasClause) {  // ref :6184
            if (itemCount > 0) {
                self_().print(',');
            }
            self_().print_space();
            print_clause_(items, (n.flags & mbun::ast::mflags::IsSingleLine) != 0,
                          [this](mbun::ast::NodeIndex it) { print_clause_item(it); });
            ++itemCount;
        }

        if (hasStar) {  // ref :6222
            if (itemCount > 0) {
                self_().print(',');
            }
            self_().print_space();
            self_().print_whitespacer(ws<"* as">);
            self_().print(' ');
            print_clause_alias_(name_of_(n.b));
            ++itemCount;
        }

        // ref :6235 — `from ` (note the whitespacer's TRAILING space) is printed
        // only when the import binds something. That is exactly why the bare form
        // has no space after the keyword: `import 'y'` => `import"y";` because
        // item_count is 0 and the `from` block is skipped entirely, leaving the
        // path's opening quote hard against `import`. Not a quirk to special-case
        // — it falls out of the same branch bun uses.
        if (itemCount > 0) {
            if (!self_().options.minifyWhitespace || hasStar || !hasClause) {
                self_().print(' ');
            }
            self_().print_whitespacer(ws<"from ">);
        }

        print_module_path_(n.text);
        self_().print_semicolon_after_statement();
    }

    // ─────────────────────────────────────────────────────────────────────────
    // print_export_decl — the non-declaration forms. `export <declaration>` is
    // stmt.cppm's (it delegates to print_decl_stmt / print_function_decl /
    // print_class_decl); this takes everything else, keyed by ExportDecl::aux.
    // ─────────────────────────────────────────────────────────────────────────
    void print_export_star(mbun::ast::NodeIndex s) {  // ref :5536 (S::ExportStar)
        const mbun::ast::Node& n { node_(s) };
        // ⚠️ NOT ported: bun's :5538 "give an extra newline for readability" —
        // `if !prev_stmt_tag.is_export_like() { print_newline() }`. `prev_stmt_tag`
        // is listed DEFERRED at printer_core.cppm:281 and stmt.cppm only tracks it
        // for SEmpty; wiring it is a stmt.cppm change, not this arm's, and getting
        // it wrong inserts a blank line into every file that has an `export *`.
        self_().print_indent();
        self_().print_space_before_identifier();
        self_().add_source_mapping(s);
        if (n.b != mbun::ast::NONE) {  // ref :5545 — `export * as ns from "m"`
            // The " as " literal stays in BOTH halves; only the space after
            // `export` is minified away. bun spells this Whitespacer inline rather
            // than with ws!() for exactly that reason (:5547-5550).
            self_().print_whitespacer(Whitespacer { "export * as ", "export* as " });
            print_clause_alias_(name_of_(n.b));
            self_().print(' ');
            self_().print_whitespacer(ws<"from ">);
        } else {  // ref :5553
            self_().print_whitespacer(ws<"export * from ">);
        }
        print_module_path_(n.text);
        self_().print_semicolon_after_statement();
    }

    // ref :5759 (S::ExportFrom) for `hasFrom=true`; :5583 (S::ExportClause) for
    // `hasFrom=false`. bun spends two arms on these because they are two S:: kinds;
    // they differ ONLY in the trailing path and in which clause-item routine runs,
    // so mbun keeps one body and a bool.
    //
    // ⚠️ This routine does NOT filter. For the no-`from` form the specifiers have
    // already been filtered down to the module-scope value bindings by
    // js_parser/module_scope.cppm's `filter_export_clauses`, mirroring bun, which
    // filters in its visit pass (visit_stmt.rs:168) and prints verbatim here. Do
    // not add a scope test to this file: the question is not answerable at the
    // point of print (`export {a}; let a=1` keeps `a`).
    void print_export_clause(mbun::ast::NodeIndex s, bool hasFrom) {
        const mbun::ast::Node& n { node_(s) };
        self_().print_indent();
        self_().print_space_before_identifier();
        self_().add_source_mapping(s);
        // ref :5766 `ws!(b"export {")` — the brace is fused to the keyword, and
        // print_clause_ opens with its own `{`, so only `export ` is printed here.
        self_().print_whitespacer(ws<"export ">);
        const std::span<const mbun::ast::NodeIndex> items { self_().arena().list_of(n) };

        // ref :5653 — an EMPTY clause is its own arm, printed before the loop:
        //     if slice_of(s.items).is_empty() { self.print(b"{}"); … return }
        // It is not cosmetic. print_clause_ below opens with `{` + print_space()
        // and closes with print_space() + `}`, so an empty single-line list would
        // come out `export {  };` — two spaces, which bun never emits. bun reaches
        // this arm for `export {a}` with `a` undeclared exactly as mbun now does:
        // the specifier is gone by print time (visit_stmt.rs:168 there,
        // js_parser/module_scope.cppm here), leaving the list empty.
        //
        // ⚠️ `export {} from 'y'` is NOT this, and the difference is OBSERVABLE.
        // bun's S::ExportFrom arm (:5758) has no empty-clause early-out, so it runs
        // the normal loop and its two print_space()s land either side of nothing —
        // emitting a genuine DOUBLE space. That looks like a bun bug; it is not
        // mbun's to fix. Verified 1.4.0:
        //     export {}            =>  export {};
        //     export {} from 'y'   =>  export {  } from "y";      // ← two spaces
        // Hence `!hasFrom`: gating this early-out on emptiness alone would "tidy"
        // the re-export form into `export {} from "y";` and diverge.
        if (!hasFrom && items.empty()) {
            self_().print("{}");
            self_().print_semicolon_after_statement();
            return;
        }

        print_clause_(items, (n.flags & mbun::ast::mflags::IsSingleLine) != 0,
                      [this, hasFrom](mbun::ast::NodeIndex it) {
                          if (hasFrom) {
                              print_export_from_clause_item(it);
                          } else {
                              print_export_clause_item(it);
                          }
                      });
        if (hasFrom) {
            // ref :5796 `ws!(b"} from ")` — bun fuses the closing brace; here
            // print_clause_ has already emitted it, so this is the rest.
            self_().print_whitespacer(ws<" from ">);
            print_module_path_(n.text);
        }
        self_().print_semicolon_after_statement();
    }

    // ref :6100 (S::ExportDefault). `export default <expr>` and `export default
    // function/class` — the declaration form delegates so the function/class
    // printer stays the one place that knows those shapes.
    void print_export_default(mbun::ast::NodeIndex s, bool isDecl, TopLevel tlmtlo) {
        const mbun::ast::Node& n { node_(s) };
        self_().print_indent();
        self_().print_space_before_identifier();
        self_().add_source_mapping(s);
        self_().print("export default");
        self_().print_space();
        if (n.a == mbun::ast::NONE) {
            self_().print_semicolon_after_statement();
            return;
        }
        if (isDecl) {
            const mbun::ast::NodeKind k { self_().kind_of(n.a) };
            if (k == mbun::ast::NodeKind::FunctionDecl) {
                self_().print_function_decl(n.a, tlmtlo);
                return;
            }
            if (k == mbun::ast::NodeKind::ClassDecl) {
                self_().print_class_decl(n.a, tlmtlo);
                return;
            }
        }
        // ref :6119 — the expression arm prints at Level::Comma so a top-level
        // comma expression parenthesises (`export default (a, b)`).
        self_().print_expr(n.a, Level::Comma, expr_flag::none());
        self_().print_semicolon_after_statement();
    }

    // ref :6160 (S::ExportEquals) — TS `export = e` => `module.exports = e;`
    // (verified, 1.4.0). bun emits this unconditionally, not only when targeting
    // CJS, and the erasure path already rewrites the keyword the same way
    // (js_parser.cppm's `module.exports =` edit).
    void print_export_equals(mbun::ast::NodeIndex s) {
        const mbun::ast::Node& n { node_(s) };
        self_().print_indent();
        self_().print_space_before_identifier();
        self_().add_source_mapping(s);
        self_().print("module.exports");
        self_().print_equals();
        if (n.a != mbun::ast::NONE) {
            self_().print_expr(n.a, Level::Lowest, expr_flag::none());
        }
        self_().print_semicolon_after_statement();
    }
};

}  // namespace mbun::js_printer
