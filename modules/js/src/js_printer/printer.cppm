// src/js_printer/printer.cppm — module mbun.js_printer.printer
//
// CAP-BUILD-PRINTER shard 1/4 — the assembly point.
//
// This file exists to do exactly one thing: mix the shards' CRTP bases into the
// concrete `Printer`. It is the ONLY file that names all four shards, and it
// stays small on purpose. The contract shards 2/3/4 code against is documented
// in full at the top of printer_core.cppm — read that first.
//
// Blueprint: .mbun/bun-ref/src/js_printer/lib.rs:1607 (`struct Printer`), :7021
// (its construction site).
export module mbun.js_printer.printer;

import std;
import mbun.js_printer.quote;
import mbun.js_printer.options;
import mbun.js_printer.printer_core;

import mbun.js_printer.expr;     // shard 2
import mbun.js_printer.stmt;     // shard 3
import mbun.js_printer.binding;  // shard 4
import mbun.js_printer.property;  // print_property (lib.rs:4743)
import mbun.js_printer.func;      // print_func / print_class (lib.rs:2527/:2543)
import mbun.js_printer.arrow;     // EArrow/EFunction/EClass arms (lib.rs:3789/:3835/:3865)
import mbun.js_printer.module_syntax;  // print_import_decl / the export forms (lib.rs:2000)
import mbun.js_printer.unsupported;    // the failure channel (mbun-specific; no bun analogue)

export namespace mbun::js_printer {

// ─────────────────────────────────────────────────────────────────────────────
// Printer — ref lib.rs:1607
//
// As each shard lands, add its base to the list below and nothing else changes:
// every cross-shard call already routes through `self_()`, so the methods
// resolve at instantiation.
//
// All four shards have landed, so the list below is complete — this is the whole
// of bun's one 6000-line `impl` block, reassembled.
//
// The shard bases are empty (no state of their own — all state lives in
// PrinterCore), so [[no_unique_address]] is unnecessary: EBO already makes them
// free. `UnsupportedSink` is the one exception and is deliberately NOT a shard:
// it carries state (the unported-kind log), takes no `Derived`, and belongs to
// no blueprint — bun has no analogue because bun has no unported kinds. It sits
// here rather than in PrinterCore because it is NodeKind-typed and PrinterCore
// is the AST-independent half (printer_core.cppm:119).
// ─────────────────────────────────────────────────────────────────────────────
template <PrinterFlags F, ByteSink W = StringSink>
class Printer
    : public PrinterCore<Printer<F, W>, F, W>
    , public UnsupportedSink                    // the failure channel
    , public ExprPrinter<Printer<F, W>>       // shard 2
    , public StmtPrinter<Printer<F, W>>       // shard 3
    , public BindingPrinter<Printer<F, W>>     // shard 4
    , public PropertyPrinter<Printer<F, W>>     // print_property (lib.rs:4743)
    , public FuncPrinter<Printer<F, W>>         // print_func/print_class (lib.rs:2527)
    , public ArrowPrinter<Printer<F, W>>        // EArrow/EFunction/EClass (lib.rs:3789)
    , public ModulePrinter<Printer<F, W>> {     // import/export forms (lib.rs:2000)
private:
    using Core = PrinterCore<Printer<F, W>, F, W>;

public:
    using Core::Core;
};

// ─────────────────────────────────────────────────────────────────────────────
// ⚠️ shard 3 status — `Printer::print_stmt` compiles but cannot yet INSTANTIATE.
//
// Adding the base above is free: a class template's member bodies are not
// instantiated until used, so StmtPrinter's calls to `self_().print_expr(…)` /
// `self_().print_binding(…)` cost nothing until someone calls `print_stmt`. The
// moment shard 2/4 land their mixins those names resolve and it goes live — that
// is the whole point of the CRTP contract (printer_core.cppm:53-62).
//
// Until then StmtPrinter is exercised by tests/test_js_printer_stmt.cpp, which
// assembles its own Printer-shaped type with a stubbed `print_expr` that echoes
// the source slice. That tests every statement arm — indentation, semicolons,
// ASI, the dangling-else brace — independently of shard 2's schedule.
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// print_into — the driver, for the common case of printing into a std::string.
//
// ref lib.rs:7640 (`print`) / the crate-root `print_with_writer{,_and_platform}`
// / `print_common_js` / `print_json` drivers described at lib.rs:5-7.
//
// DEFERRED(shard-3): the actual walk. `print_into` currently constructs the
// Printer and hands it back — there is no `print_stmt` to call yet. It is here
// so the wiring exists and is compiled; the body grows a
// `printer.print_stmt(program, ...)` the moment shard 3 lands.
//
// ⚠️ NOT wired into `mbun::js_parser::transpile`, deliberately. Today transpile
// is erasure-based (original bytes + `Arena::erase_slice` edits) and is what the
// whole 187-sample suite runs through. Swapping it to an AST-rebuild printer is
// a separate, much larger change with a much larger blast radius; it is not this
// shard's to make.
// ─────────────────────────────────────────────────────────────────────────────
template <PrinterFlags F>
[[nodiscard]] Printer<F, StringSink> make_printer(std::string& out, Options opts = {}) {
    return Printer<F, StringSink> { StringSink { out }, std::move(opts) };
}

}  // namespace mbun::js_printer
