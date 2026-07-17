// src/js_parser/trim_imports.cppm — module mbun.js_parser.trim_imports
//
// TS unused-import elision: bun's `trim_unused_imports`.
//
//   import {T} from 'y'; let x: T;   -->   let x;          // 'y' never loaded
//
// This is a CORRECTNESS feature, not an optimization: a TS import may be "fake"
// (it exists only in the type system), so leaving it in makes the runtime
// resolve — and evaluate — a module the program never asked for.
//
// ── ⚠️ WHICH PATH TURNS THIS ON — the three disagree, all three MEASURED ─────
// bun 1.4.0 (.mbun/bin/bun-rust). Do not infer these from each other:
//
//   Bun.Transpiler / transformSync   OFF   trim_unused_imports.unwrap_or(
//                                          tree_shaking), tree_shaking=false
//                                          — runtime/api/JSTranspiler.rs:645
//   the RUNTIME loader (bun run x.ts) ON    .unwrap_or_else(|| loader
//                                          .is_typescript()) — bundler/
//                                          transpiler.rs:1606-1609
//   the BUNDLER  (bun build)          ON    loader.is_typescript() || …
//                                          — bundler/ParseTask.rs:2435-2436
//
// Verified, `Bun.Transpiler` DEFAULT (loader "ts"), i.e. this flag OFF:
//     import {T} from 'y'; let x: T   =>  import { T } from "y";\nlet x;
// The import SURVIVES. So `trim` defaults OFF here to match, exactly as
// minify_syntax does (js_parser.cppm) and for the same reason: the corpus metric
// is scored against Bun.Transpiler, and a default-on trim would move it.
// Verified on the RUNTIME path by side effect — `bun run` of a file importing a
// module that logs prints nothing when the binding is type-only.
//
// ── WHY A NAME-KEYED MAP AND NOT A SYMBOL TABLE ──────────────────────────────
// bun has TWO tiers of this, and ships both:
//
//   ACCURATE   visit pass. `record_usage(ref)` (p.rs:1747) bumps
//              `ts_use_counts[ref]` (p.rs:1771) — indexed by a RESOLVED Ref —
//              and scan_imports.rs:155/173/219 reads it. Needs scopes +
//              p.symbols. mbun has neither (module_scope.cppm:44-47).
//   APPROXIMATE parse pass. `parse_pass_symbol_uses` (parse_entry.rs:436) is a
//              `StringArrayHashMap<ParsePassSymbolUse>` (parser.rs:1767) — keyed
//              by NAME. Registered at p.rs:4067/4161/4242 when the import is
//              parsed; `store_name_in_ref` (p.rs:5022) flips `used` on any
//              value-position identifier. parse_entry.rs:425-427 says why it
//              exists: "The problem with our scan pass approach is type-only
//              imports. We don't have accurate symbol counts."
//
// This file is the APPROXIMATE tier, which is the one mbun can host: it needs no
// scope chain, only a name→binding map and one hook on the identifier arm.
//
// The two tiers differ on exactly one thing — SHADOWING:
//     import {a} from 'y'; function f(){ let a=1; return a; }
// The accurate tier resolves the inner `a` to the local and TRIMS the import;
// the name-keyed tier sees the name `a` referenced and KEEPS it. That error is
// one-directional — it KEEPS an import bun would drop, never drops one bun would
// keep — so it can cost a stray module evaluation but can never delete a live
// binding. Both tiers of bun 1.4.0 were run against each other on this repo's
// own TS corpus (`git ls-files bun/` ∩ .ts, 2421 files parsed by both):
// scanImports (approximate) vs transformSync+trim (accurate) disagree on
// **1 file (0.04%)**, and that one is a dynamic-import artifact of the harness,
// not a shadow. On .tsx the raw number is higher but every case inspected was
// the injected JSX runtime import, not a shadow. So the approximation is, on
// real TypeScript, the same answer — which is why bun is willing to ship it as
// its scan tier.
//
// ── ERASURE, NOT PRINTING ────────────────────────────────────────────────────
// This drives the DEFAULT (erasure) path: it appends spans to `Arena::edits_`,
// which is the only thing that path reads (ast.cppm:143). `erase_slice`
// (ast.cppm:665) sorts at apply time and drops edits overlapping an
// already-applied one, so a whole-statement erasure correctly subsumes the
// inner `{type a}` edit the specifier loop already recorded — outer sorts first
// (smaller start), inner is then skipped. That is why these edits may be
// appended AFTER the parse has finished.
//
// The output is legal JS, not bun's bytes: dropping one specifier of
// `import {a, b} from 'y'` leaves `import {a,} from 'y'` (a's recorded span
// carries its trailing comma — the same span `{type a}` erases). A trailing
// comma in an import clause is legal, and `import d, {} from 'y'` is too. The
// erasure path never matched bun's printer bytes anyway (it keeps the source's
// own quotes), so its contract is semantics, not layout.
//
// ── MEASURED against bun 1.4.0, and the TWO KNOWN GAPS ───────────────────────
// Every .ts file in this repo's corpus (2429 comparable) was transpiled by both
// and the SURVIVING MODULE SPECIFIERS compared (bytes cannot be, see above):
// **2427 agree (99.92%), 0 over-trimmed, 2 under-trimmed**. Over-trimming is the
// only dangerous direction — it deletes a binding the code still reads — and
// there is none. Both under-trims keep a module bun drops, costing an extra
// module evaluation and nothing else:
//
//   1. TOP-LEVEL `using`/`await using`. The module-using wrapper HOISTS import
//      statements out of the try/catch it wraps the body in and re-emits their
//      text (js_parser.cppm parse_program_/emit_module_using_wrapper_), so an
//      erasure recorded over the original span never reaches the output. Fixing
//      it means teaching the hoist to skip trimmed statements; it is left alone
//      because the direction is safe and the combination (top-level `using` +
//      an otherwise-unused TS import) is rare.
//   2. `sinonjs/fake-timers.test.ts` — bun's OWN two tiers disagree on this one
//      file too (it is the single divergence in the 2421-file scan-vs-transform
//      run above). A dynamic-import shape, not a shadow.
//
// Both were found by comparing against the oracle rather than by reading this
// code, which is also how three earlier bugs here surfaced: an unguarded
// finish_stmt that segfaulted the DEFAULT path, an eager mark_used that trimmed
// a live import whose use PRECEDED it (compat/bun/test/harness.ts), and a
// string_view into a caller's stack (see mark_used).
export module mbun.js_parser.trim_imports;

import std;
import mbun.ast;

export namespace mbun::js_parser::detail {

// The parse-pass, name-keyed unused-import tracker.
// ref parse_entry.rs:436 + p.rs:4067/5022 + scan_imports.rs:141.
class ImportTrimmer {
public:
    // `import 'y'` (bare) and `export … from 'y'` (re-export) are never trimmed:
    // both are import records with no local binding to be unused.
    // ref scan_imports.rs:273-281 — `found_imports` gates the whole test, and a
    // record that WAS_ORIGINALLY_BARE_IMPORT is exempt (parse_entry.rs:475-486).
    struct Stmt {
        std::uint32_t start{0};
        std::uint32_t end{0};
        bool trimmable{false};  // false => bare / type-only / CJS-lowered: leave alone
        std::uint32_t first{0};
        std::uint32_t count{0};
    };

    struct Binding {
        std::string_view name;
        std::uint32_t stmt{0};
        // The ClauseItem's own span, trailing comma included — the exact span the
        // inline `{type a}` erasure uses. Zero-width => not individually
        // erasable (a default or namespace binding), so it only ever
        // participates in the whole-statement decision.
        std::uint32_t specStart{0};
        std::uint32_t specEnd{0};
    };

    [[nodiscard]] bool enabled() const { return enabled_; }
    void set_enabled(bool on) { enabled_ = on; }

    // Open a statement record. Caller must pair it with finish_stmt.
    std::uint32_t begin_stmt() {
        stmts_.push_back(Stmt{.first = static_cast<std::uint32_t>(bindings_.size())});
        return static_cast<std::uint32_t>(stmts_.size() - 1);
    }

    // A local binding this import introduces. `specStart == specEnd` marks it as
    // whole-statement-only (default / namespace).
    void add_binding(std::uint32_t stmt, std::string_view name, std::uint32_t specStart = 0,
                     std::uint32_t specEnd = 0) {
        if (!enabled_ || name.empty() || stmt >= stmts_.size()) {
            return;
        }
        bindings_.push_back(Binding{name, stmt, specStart, specEnd});
        stmts_[stmt].count++;
    }

    void finish_stmt(std::uint32_t stmt, std::uint32_t start, std::uint32_t end, bool trimmable) {
        // Guarded like every other entry point: when trim is off the caller
        // never ran begin_stmt, so `stmt` indexes an empty vector. (It did
        // exactly that, and the default path segfaulted — the one path that
        // must never change.)
        if (!enabled_ || stmt >= stmts_.size()) {
            return;
        }
        Stmt& s = stmts_[stmt];
        s.start = start;
        s.end = end;
        s.trimmable = trimmable;
    }

    // A VALUE-position reference to `name`. ref p.rs:5022 `store_name_in_ref`.
    //
    // Type positions never reach here, and that is structural rather than
    // careful: types.cppm "records arena edits over the type's span and builds no
    // AST nodes" (types.cppm:22-23), so a name inside a type annotation never
    // reaches the expression parser's identifier arm. That is what makes
    // `let x: T` and `let x: typeof T` both leave T unused — matching the oracle,
    // which trims both.
    //
    // ⚠️ This RECORDS the name; it does not resolve it. Resolution is deferred to
    // apply() because a reference may PRECEDE its import — bun's own test
    // harness (compat/bun/test/harness.ts) calls `basename()` at line 61 and imports it
    // at line 1208. Marking against an already-registered binding here silently
    // lost that use and trimmed a live import: an over-trim, the one direction
    // that is a ReferenceError rather than a stray module load. bun does not hit
    // this because the tier that drives its OUTPUT is the visit pass, which runs
    // after the whole parse; its parse-pass tier (scan only) does mark eagerly.
    //
    // ⚠️ OWNS its key. The obvious `unordered_set<string_view>` is a dangling
    // read waiting to happen: most callers hand over a view into the source
    // buffer (stable), but the object-literal shorthand arm hands over
    // `keyIdent` — a LOCAL std::string, and a short name lives in its SSO buffer
    // *on the stack*, so the view dangles the instant the arm returns. It did,
    // silently: the mark landed on freed bytes and the import was trimmed
    // anyway. Copying here costs an allocation only above the SSO threshold and
    // makes the API safe for every future caller, which a lifetime rule in a
    // comment would not.
    void mark_used(std::string_view name) {
        if (!enabled_ || name.empty()) {
            return;
        }
        usedNames_.emplace(name);
    }

    // Record the erasures. Call once, after the whole program is parsed —
    // `export {a}; import {a} from 'y'` order-independence requires it, the same
    // way module_scope.cppm's filter does.
    void apply(mbun::ast::Arena& arena) const {
        if (!enabled_) {
            return;
        }
        for (const Stmt& s : stmts_) {
            // `found_imports` — a statement with no local binding is a bare
            // import or a re-export and is always kept (scan_imports.rs:273).
            if (!s.trimmable || s.count == 0) {
                continue;
            }
            bool anyUsed = false;
            for (std::uint32_t i = s.first; i < s.first + s.count; ++i) {
                anyUsed = anyUsed || used(bindings_[i]);
            }
            if (!anyUsed) {
                // is_unused_in_typescript — every binding dead, so the whole
                // record goes (scan_imports.rs:283-289). This subsumes any inner
                // specifier edit via erase_slice's overlap rule.
                //
                // CJS: the statement's span already carries an edit — the
                // `require()` lowering the parser recorded in its place — and a
                // second edit over the same span would lose to it (erase_slice
                // applies the first and skips overlaps). So retract that lowering
                // instead of racing it; blank_edit turns it into the deletion this
                // wants. ESM: no such edit exists, blank_edit says so, and the
                // deletion is recorded normally. One path, both modes.
                if (!arena.blank_edit(s.start, s.end)) {
                    arena.add_edit(s.start, s.end);
                }
                continue;
            }
            // Partially used: drop only the dead specifiers that have a span of
            // their own. ref scan_imports.rs:219 — the accurate tier compacts the
            // items list; erasing the source span is the erasure-path equivalent.
            //
            // A NO-OP in CJS mode, deliberately: these spans sit inside the import
            // statement, whose whole span the `require()` lowering already
            // replaced, so erase_slice skips them as overlaps. Nothing is lost that
            // matters — the module is loaded either way (some binding IS used), so
            // the surviving `let dead = __mbun_i0.dead` is an unused local bound to
            // undefined, not a side effect and not an error. The WHOLE-statement
            // case above is the one that changes behavior, and it works in CJS.
            for (std::uint32_t i = s.first; i < s.first + s.count; ++i) {
                const Binding& b = bindings_[i];
                if (!used(b) && b.specStart < b.specEnd) {
                    arena.add_edit(b.specStart, b.specEnd);
                }
            }
        }
    }

private:
    // Resolved at apply() time, never at reference time — see mark_used. The
    // std::string temporary is built once per BINDING (a handful per file), not
    // per reference, so the heterogeneous-lookup dance this avoids would buy
    // nothing and costs libc++/libstdc++ portability.
    [[nodiscard]] bool used(const Binding& b) const {
        return usedNames_.contains(std::string{b.name});
    }

    bool enabled_{false};
    std::vector<Stmt> stmts_;
    std::vector<Binding> bindings_;
    // Every name referenced from a value position anywhere in the file. Keyed by
    // NAME with no scope, so a shadowing local (`function f(){ let a=1 }`) marks
    // an import `a` used — the conservative direction. See the header. Owning:
    // see mark_used.
    std::unordered_set<std::string> usedNames_;
};

}  // namespace mbun::js_parser::detail
