import std;
import mbun.crash_handler;

namespace {

int failures { 0 };

void check(bool condition, std::string_view message) {
    if (!condition) {
        ++failures;
        std::println("FAIL: {}", message);
    }
}

struct FixedSymbols final : mbun::crash_handler::SymbolBackend {
    std::optional<mbun::crash_handler::Symbol> resolve(std::uintptr_t address) const override {
        if (address == 0x1234) {
            return mbun::crash_handler::Symbol { "js_panic", "mbun", "fixture.js", 42 };
        }
        return std::nullopt;
    }
};

} // namespace

int main() {
    using namespace mbun::crash_handler;

    check(format_reason(CrashEvent::panic("invoked crashByPanic() handler"))
              == "invoked crashByPanic() handler",
          "panic reason preserves message");
    check(format_reason(CrashEvent::segmentation_fault(0xabc))
              == "Segmentation fault at address 0xABC",
          "segmentation fault reason uses uppercase hexadecimal");
    check(format_reason(CrashEvent::out_of_memory()) == "Bun ran out of memory",
          "out of memory reason matches Bun");

    auto seed { TraceSeed::begin_address(0x1234) };
    check(seed.kind == TraceSeedKind::begin_address && seed.address == 0x1234,
          "begin address seed retains the capture anchor");

    ReportPayload payload { CrashEvent::panic("boom"), "linux", "1.3.14", "deadbeef",
                            { 0x1234, 0x5678 }, "parsing input.js" };
    std::string report { render_report(payload, FixedSymbols {}) };
    check(report.find("reason: boom") != std::string::npos, "report includes reason");
    check(report.find("action: parsing input.js") != std::string::npos,
          "report includes current action");
    check(report.find("js_panic (mbun) at fixture.js:42") != std::string::npos,
          "report resolves known symbols");
    check(report.find("0x5678") != std::string::npos, "report keeps unknown address");

    if (failures != 0) {
        std::println("{} checks failed", failures);
        return 1;
    }
    std::println("crash_handler checks passed");
}
