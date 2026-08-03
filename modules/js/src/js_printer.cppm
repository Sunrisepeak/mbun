// src/js_printer.cppm — module mbun.js_printer
//
// Aggregator for the JS printer subsystem. Re-exports the parts; holds no code
// of its own (AGENTS.md 规则 10 — 「目录 + 多个子模块 .cppm」+ 薄聚合模块, same
// shape as modules/http/src/http.cppm).
//
// CAP-BUILD-PRINTER: a 1:1 port of bun's real printer
// (.mbun/bun-ref/src/js_printer/lib.rs, 8446 lines) via AGENTS.md 「移植三段法」.
// Parallel-translated in four shards; the contract between them is documented at
// the top of js_printer/printer_core.cppm.
//
//   encoding      — Encoding, WTF-8 decode/encode, hex + escape sequences
//   quote         — can_print_without_escape, best_quote_char_for_string,
//                   write_pre_quoted_string{,_inner}, quote_for_json,
//                   write_json_string, the escape scan          [shard 1 ✅]
//   whitespacer   — Whitespacer + ws<"...">, indentation buffers [shard 1 ✅]
//   flags         — ExprFlag(Set), TopLevel, ClauseItemAs, PrintResult
//   options       — Options, PrintJsonOptions, the print-time callbacks
//   printer_core  — the Printer skeleton + the shard contract
//   op_level      — Level, the operator-precedence enum shared by stmt+expr
//                   (bun keeps it in src/ast/op.rs:155)          [shard 3 ✅]
//   stmt          — print_stmt + print_block/print_body/print_if/print_decls
//                                                                [shard 3 ✅]
//   func          — print_fn_args/print_func/print_class + the S::Function /
//                   S::Class arms of print_stmt (ref lib.rs:2487-2590, :5307,
//                   :5361). Its own module: expr.cppm is at the 2000-line cap
//                   and stmt.cppm only NAMED these.
//   printer       — the CRTP assembly point
//   legacy        — ⚠️ the superseded 648-line subset printer; still the spec
//                   for tests/test_js_printer.cpp. See its header.
//
// Status: shard 1 (strings/quotes/escapes/options/skeleton) is done and diff-
// verified against real bun 1.3.14. print_expr / print_stmt / print_binding +
// renamer are NOT ported yet, so nothing here can print a program — the printer
// is not wired into `transpile`, which still runs the erasure path.
export module mbun.js_printer;

export import mbun.js_printer.encoding;
export import mbun.js_printer.quote;
export import mbun.js_printer.whitespacer;
export import mbun.js_printer.flags;
export import mbun.js_printer.options;
export import mbun.js_printer.printer_core;
export import mbun.js_printer.op_level;
export import mbun.js_printer.expr_op;  // shard 2 — Op::Code + TABLE (ref ast/op.rs)
export import mbun.js_printer.expr;     // shard 2 — print_expr (ref lib.rs:3195)
export import mbun.js_printer.stmt;
export import mbun.js_printer.binding;  // shard 4 — print_binding (ref lib.rs:5052)
export import mbun.js_printer.property;  // print_property (ref lib.rs:4743)
export import mbun.js_printer.func;      // print_func/print_class (ref lib.rs:2527/:2543)
export import mbun.js_printer.module_syntax;  // import/export forms (ref lib.rs:2000/:3077)
export import mbun.js_printer.unsupported;  // the unported-kind log; no bun analogue
export import mbun.js_printer.printer;
export import mbun.js_printer.legacy;
