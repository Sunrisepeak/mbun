import std;
import mbun.codegen;

namespace {
int failures { 0 };

void check(bool condition, std::string_view message) {
    if (!condition) {
        ++failures;
        std::println("FAIL: {}", message);
    }
}
}

int main() {
    using namespace mbun::codegen;

    OutputBuffer buffer;
    buffer.append("const x = 1;");
    buffer.append_char('\n');
    buffer.append("x");
    check(buffer.view() == "const x = 1;\nx", "output buffer retains emitted bytes");
    check(buffer.line() == 1 && buffer.column() == 1, "output position tracks lines and columns");

    SymbolTable symbols;
    auto original { symbols.add("value", SymbolKind::local) };
    auto alias { symbols.add("alias", SymbolKind::imported) };
    symbols.merge(alias, original);
    check(symbols.follow(alias) == original, "symbol links follow merged references");

    BufferBackend backend { &symbols };
    backend.emit("let ");
    backend.emit_symbol(original, "value");
    backend.emit(" = 1;");
    backend.emit_mapping(SourceMapState { 0, 4, 0, 0, 0 });
    auto artifact { backend.finish() };
    check(artifact.code == "let value = 1;", "backend emits through the output seam");
    check(artifact.source_map_end.generated_column == 4, "backend retains source-map end state");

    if (failures != 0) {
        std::println("{} checks failed", failures);
        return 1;
    }
    std::println("codegen checks passed");
}
