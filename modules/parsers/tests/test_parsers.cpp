// test_parsers.cpp — first-stage parser seam vectors.
//
// The vectors reflect Bun's parser family boundaries: byte-oriented source,
// location-bearing diagnostics, and format selection. Full JSON/JSON5/YAML
// grammar and the existing TOML implementation remain separate follow-ups.
import std;
import mbun.parsers;

namespace {

int checks { 0 };
int failures { 0 };

void check(bool condition, std::string_view what) {
    ++checks;
    if (!condition) {
        ++failures;
        std::println("FAIL: {}", what);
    }
}

}  // namespace

int main() {
    using namespace mbun::parsers;
    const Input input { "first\nsecond\nthird" };
    check(input.size() == 18, "input preserves byte length");
    check(input.slice(6, 6) == "second", "input exposes zero-copy slices");
    const Position position = input.position(7);
    check(position.offset == 7 && position.line == 2 && position.column == 2,
          "position reports 1-based line and byte column");
    check(input.position(input.size() + 10).offset == input.size(),
          "position clamps offsets at end of input");

    const auto error = ParseError::at(ErrorKind::InvalidNumber, input, 7,
                                      "invalid numeric literal");
    check(error.kind == ErrorKind::InvalidNumber, "error retains category");
    check(error.position.offset == 7 && error.position.line == 2,
          "error retains source position");
    check(error.message == "invalid numeric literal", "error retains message");

    check(detect_format("config.toml") == Format::Toml, "detects TOML extension");
    check(detect_format("package.jsonc") == Format::Json, "JSONC uses JSON parser family");
    check(detect_format("data.json5") == Format::Json5, "detects JSON5 extension");
    check(detect_format("values.yaml") == Format::Yaml, "detects YAML extension");
    check(detect_format(".npmrc") == Format::Ini, "detects INI dot-file");
    check(detect_format("README") == Format::Unknown, "unknown extension stays unknown");

    const auto& registry = format_registry();
    check(registry.size() >= 5, "registry exposes parser family descriptors");
    check(find_format("toml") != nullptr, "registry lookup is case-insensitive");
    check(find_format("no-such-format") == nullptr, "unknown format lookup is null");

    std::println("parsers: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}
