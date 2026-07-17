// Output seam vectors derived from Bun's output/tag behaviour.
// Runtime fd, TTY, and CLI/JSC integration are intentionally DEFERRED(S1).
import std;
import mbun.output;

namespace {
int checks {0};
int failures {0};

void check(bool condition, std::string_view what) {
    ++checks;
    if (!condition) {
        ++failures;
        std::println("FAIL {}", what);
    }
}
}  // namespace

int main() {
    mbun::output::Buffer buffer;
    buffer.append("hello");
    buffer.append(" world");
    check(buffer.text() == "hello world", "buffer append/text");
    check(buffer.size() == 11, "buffer size");
    buffer.clear();
    check(buffer.empty(), "buffer clear");

    check(mbun::output::color_for("red") == mbun::output::ansi::red, "red tag");
    check(mbun::output::color_for("unknown") == std::nullopt, "unknown tag");
    check(mbun::output::color_for("bgred") == "\x1b[41m", "background tag");

    mbun::output::Diagnostic diagnostic{
        mbun::output::Severity::warning,
        "unused binding",
        mbun::output::SourceLocation{"entry.ts", 4, 9},
    };
    check(diagnostic.format() == "entry.ts:4:9: warning: unused binding", "diagnostic format");
    check(mbun::output::severity_name(mbun::output::Severity::error) == "error", "severity name");

    std::println("{} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}
