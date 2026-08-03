// src/js_printer/unsupported.cppm — module mbun.js_printer.unsupported
//
// The printer's failure channel: the one place an AST kind with no port is
// RECORDED instead of silently dropped.
//
// ═════════════════════════════════════════════════════════════════════════════
// WHY THIS MODULE EXISTS
// ═════════════════════════════════════════════════════════════════════════════
//
// `print_stmt` used to end in
//
//     default:
//         break;   // "dropping them silently is wrong, but so is aborting"
//
// and that arm was load-bearing for a whole class of wrong answers. A statement
// kind the printer had no port for produced NO bytes and NO complaint, so:
//
//   * `enum E{} export const v=E.A` printed `export const v = E.A` with `E`
//     never declared — a ReferenceError at runtime, reported as success;
//   * a file whose every `import` was dropped still returned
//     `TranspileResult::ok == true`, because `ok` only ever reflected the PARSE
//     (js_parser.cppm:5126 `out.ok = r.ok`). The printer had no way to say no.
//
// The comment defending that arm was right about one thing and wrong about the
// conclusion. It IS true that mbun's printer is a library and must not abort the
// process the way bun does (:6484 `Output::panic`) — bun can panic because a bad
// tag there is an internal invariant break, while mbun's arena legitimately
// holds kinds this shard has not ported yet. But "cannot abort" does not imply
// "must stay quiet": the third option is to RECORD the kind and let the caller
// decide. That is this module.
//
// bun does not need an equivalent because bun has no unported kinds — every
// StmtData variant either has a printer arm or is lowered away before the
// printer runs (SEnum/SNamespace: visit/visit_stmt.rs:89/:120). This type is
// therefore mbun-specific scaffolding for an in-progress port, not a port of
// anything, and it should SHRINK to nothing as the arms land.
//
// ═════════════════════════════════════════════════════════════════════════════
// WHY A SEPARATE MODULE
// ═════════════════════════════════════════════════════════════════════════════
//
// The natural home looks like `printer_core` — it holds all the other shared
// printer state. But printer_core is deliberately "the AST-independent half"
// (printer_core.cppm:119) and does not `import mbun.ast`; a NodeKind-typed
// record there would break that separation for every shard. So this is its own
// CRTP-free base, mixed into `Printer` alongside the shards (printer.cppm), and
// reachable from stmt.cppm / expr.cppm through the usual `self_()` (AGENTS.md
// 规则 10 — new code starts in its own submodule).
// ═════════════════════════════════════════════════════════════════════════════
export module mbun.js_printer.unsupported;

import std;
import mbun.ast;

export namespace mbun::js_printer {

// ─────────────────────────────────────────────────────────────────────────────
// UnsupportedSink — records AST kinds the printer has no port for.
//
// NOT a CRTP template: it needs no access to `Derived`, and keeping it plain
// means `Printer` can inherit it without adding to the shard contract.
// ─────────────────────────────────────────────────────────────────────────────
class UnsupportedSink {
public:
    struct Record {
        mbun::ast::NodeKind kind {};
        std::uint32_t offset {};  // byte offset into the ORIGINAL source
    };

private:
    // A malformed or wildly unported file can hit this thousands of times; the
    // count stays exact but only the first few are kept, so a diagnostic cannot
    // itself become the memory problem. bun caps its own log the same way
    // (`Log::MAX_ERRORS`).
    static constexpr std::size_t MAX_RECORDED { 8 };

    std::vector<Record> records_ {};
    std::size_t count_ { 0 };

public:
    // Called from a printer arm that cannot emit correct bytes for `kind`.
    // Printing nothing and recording is the honest pair: the output is still
    // well formed (so the surrounding walk survives and the caller can see how
    // far it got), but it is now KNOWN to be incomplete.
    void record_unsupported(mbun::ast::NodeKind kind, std::uint32_t offset) {
        if (records_.size() < MAX_RECORDED) {
            records_.push_back(Record { kind, offset });
        }
        count_ += 1;
    }

    [[nodiscard]] bool has_unsupported() const { return count_ != 0; }

    // Exact even when `records_` was capped.
    [[nodiscard]] std::size_t unsupported_count() const { return count_; }

    [[nodiscard]] std::span<const Record> unsupported() const { return records_; }

    // The first unported kind, i.e. the one to blame in a single-line
    // diagnostic. `MAX_RECORDED >= 1`, so this is non-empty whenever
    // `has_unsupported()`.
    [[nodiscard]] Record first_unsupported() const { return records_.front(); }

    // Human-readable, for `TranspileResult::error`. Shape mirrors the parser's
    // own messages so a caller printing either reads the same.
    [[nodiscard]] std::string unsupported_message() const {
        if (!has_unsupported()) {
            return {};
        }
        const Record& r { records_.front() };
        std::string msg { "printer: no port for " };
        msg += mbun::ast::node_kind_name(r.kind);
        if (count_ > 1) {
            msg += " (and ";
            msg += std::to_string(count_ - 1);
            msg += " more unported node";
            if (count_ > 2) {
                msg += 's';
            }
            msg += ")";
        }
        return msg;
    }

    void clear_unsupported() {
        records_.clear();
        count_ = 0;
    }
};

}  // namespace mbun::js_printer
