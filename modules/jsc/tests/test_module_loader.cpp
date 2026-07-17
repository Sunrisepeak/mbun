// test_module_loader.cpp — T3.3 mbun.jsc.module_loader PURE-LOGIC suite.
//
// The loader's resolve → read → transpile-decision pipeline is exercised off the
// JS engine: filesystem access is injected via a mbun::resolver::FileSystem
// in-memory fixture (same pattern as test_resolver.cpp), so every scenario runs
// without a real disk and without JavaScriptCore. The load→eval smoke that does
// touch JSC lives in test_module_loader_jsc_smoke.cpp.
//
// The transpile step reuses the T2.4/T2.5 transpiler chain (mbun.js_parser →
// mbun.js_printer), which erases the TS-annotation subset those suites pin
// (variable-declaration type annotations, `as` / `satisfies`, type-argument
// lists). Vectors below stay within that already-supported subset; the printer
// re-emits any statement outside it verbatim, so anything broader (function/class
// annotations, enums/namespaces lowering, JSX element transform, ESM
// import/export statements, JSON module wrapping, sourcemaps) is DEFERRED — see
// the header of test_js_printer.cpp / test_js_parser.cpp and the T3.3 plan entry.
//
// Pipeline shape re-expressed in MC++ from bun (behavior, not a line port):
//   - .mbun/bun-ref/src/jsc/ModuleLoader.rs: transpile_source_code (loader picked
//     from the file extension, JS-like loaders erase TS/JSX then hand JS to JSC).
//   - .mbun/bun-zig-src/src/jsc/ModuleLoader.zig: Bun__getDefaultLoader (ext →
//     loader, `.file` → js fallback) + transpileSourceCode loader switch.

import std;
import mbun.jsc.module_loader;
import mbun.resolver;

using mbun::jsc::module_loader::Loader;
using mbun::jsc::module_loader::LoadStatus;
using mbun::jsc::module_loader::ModuleLoader;
using mbun::jsc::module_loader::loader_for_path;
using mbun::jsc::module_loader::needs_transpile;
using mbun::jsc::module_loader::transpile;
using mbun::resolver::FileSystem;
using mbun::resolver::Options;

namespace {

int gChecks{0};
int gFailures{0};
constexpr int MAX_FAILURE_PRINTS{40};

void report_failure(std::string_view what) {
    ++gFailures;
    if (gFailures <= MAX_FAILURE_PRINTS) {
        std::println("  FAIL {}", what);
    }
}

void expect(bool cond, std::string_view what) {
    ++gChecks;
    if (!cond) {
        report_failure(what);
    }
}

template <class A, class B>
void expect_eq(const A& got, const B& want, std::string_view what) {
    ++gChecks;
    if (!(got == want)) {
        report_failure(std::format("{}: got \"{}\", want \"{}\"", what, got, want));
    }
}

// In-memory fs fixture (files carry contents; dirs derived from every ancestor).
class MemoryFs {
public:
    void add_file(std::string path, std::string contents = {}) {
        files_[path] = std::move(contents);
        std::string_view p{path};
        while (true) {
            const auto slash{p.rfind('/')};
            if (slash == std::string_view::npos || slash == 0) {
                dirs_.insert("/");
                break;
            }
            p = p.substr(0, slash);
            dirs_.insert(std::string{p});
        }
    }

    FileSystem make() const {
        return FileSystem{
            .file_exists = [this](std::string_view path) { return files_.contains(std::string{path}); },
            .dir_exists = [this](std::string_view path) { return dirs_.contains(std::string{path}); },
            .read_file =
                [this](std::string_view path) -> std::optional<std::string> {
                const auto it{files_.find(std::string{path})};
                if (it == files_.end()) {
                    return std::nullopt;
                }
                return it->second;
            },
        };
    }

private:
    std::map<std::string, std::string> files_;
    std::set<std::string> dirs_;
};

// ── extension → loader classification ──────────────────────────────────────
void test_loader_classification() {
    expect(loader_for_path("/p/a.js") == Loader::Js, "loader .js -> Js");
    expect(loader_for_path("/p/a.mjs") == Loader::Js, "loader .mjs -> Js");
    expect(loader_for_path("/p/a.cjs") == Loader::Js, "loader .cjs -> Js");
    expect(loader_for_path("/p/a.jsx") == Loader::Jsx, "loader .jsx -> Jsx");
    expect(loader_for_path("/p/a.ts") == Loader::Ts, "loader .ts -> Ts");
    expect(loader_for_path("/p/a.mts") == Loader::Ts, "loader .mts -> Ts");
    expect(loader_for_path("/p/a.cts") == Loader::Ts, "loader .cts -> Ts");
    expect(loader_for_path("/p/a.tsx") == Loader::Tsx, "loader .tsx -> Tsx");
    expect(loader_for_path("/p/a.json") == Loader::Json, "loader .json -> Json");
    // Unknown / extensionless -> Js fallback (bun `.file` -> js).
    expect(loader_for_path("/p/a.bin") == Loader::Js, "loader unknown ext -> Js fallback");
    expect(loader_for_path("/p/noext") == Loader::Js, "loader no ext -> Js fallback");

    // Js (.js/.mjs/.cjs) now transpiles too: erasure is identity on plain JS/CJS
    // but lowers ESM import/export to CommonJS (JSC C-API script mode can't link
    // static ESM otherwise — it mis-parses `import … from` as a dynamic import()).
    expect(needs_transpile(Loader::Js), "Js needs transpile (ESM->CJS lowering)");
    expect(!needs_transpile(Loader::Json), "Json needs no transpile (raw passthrough, wrap DEFERRED)");
    expect(needs_transpile(Loader::Ts), "Ts needs transpile");
    expect(needs_transpile(Loader::Tsx), "Tsx needs transpile");
    expect(needs_transpile(Loader::Jsx), "Jsx needs transpile");
}

// ── transpile: TS-annotation erasure subset ────────────────────────────────
void test_transpile_ts_erasure() {
    struct Case {
        std::string_view src;
        std::string_view want;
    };
    // Erasure preserves the original JS bytes and cuts only the TS-only spans —
    // note single quotes and spacing are kept verbatim (unlike an AST-rebuild
    // printer, which would normalise `'a'`→`"a"` and add a trailing newline).
    const Case cases[]{
        {"const x = 1 as number;", "const x = 1;"},
        {"const t4: T1 = { a: 'a' } satisfies T1;", "const t4 = { a: 'a' };"},
        {"let t7 = { a: 'test' } satisfies A;", "let t7 = { a: 'test' };"},
        {"var v = undefined satisfies 1;", "var v = undefined;"},
        {"const a = { x: 10 } satisfies Partial<Point2d>;", "const a = { x: 10 };"},
        {"f<number>.g", "f.g"},
        {"a([f<x>]);", "a([f]);"},
    };
    for (const auto& c : cases) {
        auto r{transpile(c.src, Loader::Ts)};
        if (!r) {
            report_failure(std::format("transpile(\"{}\") errored: {}", c.src, r.error()));
            ++gChecks;
            continue;
        }
        expect_eq(*r, std::string{c.want}, std::format("transpile(\"{}\")", c.src));
    }
}

// ── load(): resolve + read + transpile-decision end to end ─────────────────
void test_load_pipeline() {
    MemoryFs fs;
    fs.add_file("/proj/plain.js", "40 + 2;\n");
    fs.add_file("/proj/typed.ts", "const x = 1 as number;\n");
    fs.add_file("/proj/index.tsx", "const y = 2 as number;\n");  // classified only
    fs.add_file("/proj/data.json", "{ \"a\": 1 }");
    ModuleLoader loader{fs.make(), Options{}};

    // .js passthrough: source returned verbatim, no transpile.
    {
        auto r{loader.load("./plain.js", "/proj")};
        expect(r.status == LoadStatus::Success, "load ./plain.js Success");
        expect_eq(r.path, std::string{"/proj/plain.js"}, "load ./plain.js path");
        expect(r.loader == Loader::Js, "load ./plain.js loader=Js");
        expect_eq(r.source, std::string{"40 + 2;\n"}, "load ./plain.js source verbatim");
    }

    // .ts transpile: TS annotation erased, loader=Ts.
    {
        auto r{loader.load("./typed.ts", "/proj")};
        expect(r.status == LoadStatus::Success, "load ./typed.ts Success");
        expect(r.loader == Loader::Ts, "load ./typed.ts loader=Ts");
        expect_eq(r.source, std::string{"const x = 1;\n"}, "load ./typed.ts erased source");
    }

    // Extensionless specifier -> resolver extension completion -> .ts transpile.
    {
        auto r{loader.load("./typed", "/proj")};
        expect(r.status == LoadStatus::Success, "load ./typed (no ext) Success");
        expect_eq(r.path, std::string{"/proj/typed.ts"}, "load ./typed completes to .ts");
        expect_eq(r.source, std::string{"const x = 1;\n"}, "load ./typed erased source");
    }

    // Directory index resolution -> ./sub resolves to ./sub/index.tsx (classify).
    {
        fs.add_file("/proj/sub/index.tsx", "const z = 3 as number;\n");
        ModuleLoader loader2{fs.make(), Options{}};
        auto r{loader2.load("./sub", "/proj")};
        expect(r.status == LoadStatus::Success, "load ./sub -> index resolution Success");
        expect(r.loader == Loader::Tsx, "load ./sub loader=Tsx");
        expect_eq(r.source, std::string{"const z = 3;\n"}, "load ./sub/index.tsx erased source");
    }

    // Resolve failure -> ResolveFailed (message non-empty for diagnostics).
    {
        auto r{loader.load("./does-not-exist", "/proj")};
        expect(r.status == LoadStatus::ResolveFailed, "load missing -> ResolveFailed");
        expect(!r.message.empty(), "ResolveFailed carries a message");
    }
}

// A file the resolver sees (file_exists) but whose contents cannot be read
// (read_file miss) must surface ReadFailed, not a silent empty module.
void test_load_read_failure() {
    FileSystem fs{
        .file_exists = [](std::string_view p) { return p == "/proj/ghost.js"; },
        .dir_exists = [](std::string_view p) { return p == "/proj" || p == "/"; },
        .read_file = [](std::string_view) -> std::optional<std::string> { return std::nullopt; },
    };
    ModuleLoader loader{fs, Options{}};
    auto r{loader.load("./ghost.js", "/proj")};
    expect(r.status == LoadStatus::ReadFailed, "load unreadable -> ReadFailed");
    expect_eq(r.path, std::string{"/proj/ghost.js"}, "ReadFailed still reports resolved path");
}

// The `type` import attribute picks the loader, overriding the extension
// (.mbun/bun-ref/src/bundler/options.rs:600-604 + ast/loader.rs:224 from_string).
// Every expectation below was read off bun 1.4.0 before it was written here.
void test_type_attribute_loader() {
    using mbun::jsc::module_loader::loader_from_string;
    // LOADER_NAMES (ast/loader.rs:124-152): the names this runtime implements.
    expect(loader_from_string("text") == Loader::Text, "attr text -> Text");
    expect(loader_from_string("txt") == Loader::Text, "attr txt -> Text");
    expect(loader_from_string("file") == Loader::File, "attr file -> File");
    expect(loader_from_string("json") == Loader::Json, "attr json -> Json");
    // loader.rs:225-229 — a leading '.' is stripped first.
    expect(loader_from_string(".text") == Loader::Text, "attr .text -> Text (dot stripped)");
    // loader.rs:230-233 — retried case-insensitively.
    expect(loader_from_string("TEXT") == Loader::Text, "attr TEXT -> Text (case-insensitive)");
    // A miss leaves the extension in charge (options.rs:601 only assigns on a hit).
    expect(!loader_from_string("nope").has_value(), "unknown attr -> nullopt");

    // Extension table (options.rs:633-655).
    expect(loader_for_path("/p/a.txt") == Loader::Text, ".txt -> Text");
    expect(loader_for_path("/p/ui.css") == Loader::File, ".css -> File");
    expect(!needs_transpile(Loader::Text), "Text is never transpiled");
    expect(!needs_transpile(Loader::File), "File is never transpiled");

    FileSystem fs{
        .file_exists = [](std::string_view p) { return p == "/proj/ui.css" || p == "/proj/n.txt"; },
        .dir_exists = [](std::string_view p) { return p == "/proj" || p == "/"; },
        .read_file = [](std::string_view p) -> std::optional<std::string> {
            if (p == "/proj/ui.css") return std::string{"@media (min-width: 1px) { .a { b: c; } }"};
            if (p == "/proj/n.txt") return std::string{"hi\n"};
            return std::nullopt;
        },
    };
    ModuleLoader loader{fs, Options{}, /*cjs=*/true};

    // The bug this fixes: without the attribute the CSS was loaded as JS and the
    // lexer rejected '@'. With it, the module's default export is the contents.
    auto css{loader.load("./ui.css", "/proj", "text")};
    expect(css.status == LoadStatus::Success, "css + type=text loads");
    expect(css.loader == Loader::Text, "css + type=text -> Text (attr beats ext)");
    expect(css.source.find("@media (min-width: 1px)") != std::string::npos,
           "Text module carries the file's contents");
    expect(css.source.find("__esModule") != std::string::npos,
           "Text module marks __esModule so default-import interop binds the string");

    // No attribute: .css is the File loader, whose value is the PATH, and the
    // bytes are never read (so the contents must NOT appear in the module).
    auto bare{loader.load("./ui.css", "/proj")};
    expect(bare.status == LoadStatus::Success, "css without attr loads");
    expect(bare.loader == Loader::File, "css without attr -> File");
    expect(bare.source.find("/proj/ui.css") != std::string::npos, "File module exports the path");
    expect(bare.source.find("@media") == std::string::npos, "File module never reads the bytes");

    // .txt is Text by extension alone.
    auto txt{loader.load("./n.txt", "/proj")};
    expect(txt.loader == Loader::Text, ".txt without attr -> Text");
    expect(txt.source.find("hi") != std::string::npos, ".txt module carries contents");

    // An unrecognized attribute is ignored — the extension still decides.
    auto ignored{loader.load("./n.txt", "/proj", "bogus")};
    expect(ignored.loader == Loader::Text, "unknown attr ignored -> extension wins");
}

}  // namespace

int main() {
    test_loader_classification();
    test_transpile_ts_erasure();
    test_load_pipeline();
    test_load_read_failure();
    test_type_attribute_loader();

    std::println("test_module_loader: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
