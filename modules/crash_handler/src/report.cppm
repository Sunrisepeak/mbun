export module mbun.crash_handler.report;

import std;
import mbun.crash_handler.events;
import mbun.crash_handler.symbols;

namespace mbun::crash_handler {

export struct ReportPayload {
    CrashEvent event;
    std::string platform;
    std::string version;
    std::string commit;
    std::vector<std::uintptr_t> addresses;
    std::optional<std::string> action;
};

inline void append_frame_(std::string& output, std::uintptr_t address,
                          const SymbolBackend& symbols) {
    auto symbol { symbols.resolve(address) };
    if (symbol) {
        output += std::format("  {} ({}) at {}:{}\n", symbol->name, symbol->module,
                              symbol->file, symbol->line);
    } else {
        output += std::format("  0x{:X}\n", address);
    }
}

// Human-readable report used by the future stderr and upload adapters. The
// version/commit/platform fields mirror Bun's trace-v1 payload metadata; a
// compressed bun.report wire encoder is DEFERRED until the runtime/backend
// integration exists.
export std::string render_report(const ReportPayload& payload,
                                 const SymbolBackend& symbols = NullSymbolBackend {}) {
    std::string output;
    output += std::format("reason: {}\n", format_reason(payload.event));
    if (!payload.platform.empty()) {
        output += std::format("platform: {}\n", payload.platform);
    }
    if (!payload.version.empty()) {
        output += std::format("version: {}\n", payload.version);
    }
    if (!payload.commit.empty()) {
        output += std::format("commit: {}\n", payload.commit);
    }
    if (payload.action) {
        output += std::format("action: {}\n", *payload.action);
    }
    for (std::uintptr_t address : payload.addresses) {
        append_frame_(output, address, symbols);
    }
    return output;
}

} // namespace mbun::crash_handler
