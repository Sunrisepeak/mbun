// test_module_loader_jsc_smoke.cpp — T3.3 mbun.jsc.module_loader JSC smoke.
//
// Proves the load pipeline is eval-ready end to end: an in-memory module is
// resolved, read, and (for TS) transpiled through mbun.js_parser →
// mbun.js_printer, then the resulting JS is evaluated by the bun-webkit
// JavaScriptCore embedding via mbun.compat.jsc. This is the T3.3 acceptance
// smoke ("`export const x = 1 as number` → erase → eval → x===1", reduced to a
// completion-value form since ESM export statements are DEFERRED).
//
// ref (behavior): .mbun/bun-ref/src/jsc/ModuleLoader.rs transpile_source_code
// (transpiled JS is handed to JSC for module evaluation).
import std;
import mbun.jsc.module_loader;
import mbun.compat.jsc;
import mbun.resolver;

using mbun::compat::jsc::eval;
using mbun::jsc::module_loader::LoadStatus;
using mbun::jsc::module_loader::ModuleLoader;
using mbun::resolver::FileSystem;
using mbun::resolver::Options;

namespace {

int gFailed{0};

void expect(bool cond, std::string_view what) {
    if (!cond) {
        ++gFailed;
        std::println("  FAIL: {}", what);
    }
}

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
            .read_file = [this](std::string_view path) -> std::optional<std::string> {
                const auto it{files_.find(std::string{path})};
                return it == files_.end() ? std::nullopt : std::optional{it->second};
            },
        };
    }

private:
    std::map<std::string, std::string> files_;
    std::set<std::string> dirs_;
};

}  // namespace

int main() {
    MemoryFs fs;
    // TS module: annotation + `as` erased, trailing expression is the completion value.
    fs.add_file("/proj/mod.ts", "const x: number = 1 as number;\nx;\n");
    // .js passthrough module (no transpile), completion value 42.
    fs.add_file("/proj/plain.js", "40 + 2;\n");
    // Extensionless import resolving to a .ts, arithmetic over erased annotations.
    fs.add_file("/proj/calc.ts", "const a = 2 as number;\nconst b = 3 as number;\na * b;\n");
    ModuleLoader loader{fs.make(), Options{}};

    // load → transpile (erase TS) → eval == 1 (the milestone acceptance).
    {
        auto r{loader.load("./mod.ts", "/proj")};
        expect(r.status == LoadStatus::Success, "load ./mod.ts Success");
        auto v{eval(r.source)};
        expect(v.has_value(), "eval(transpiled mod.ts) succeeds");
        expect(v.has_value() && *v == 1.0, "transpiled `1 as number` evals to 1");
    }

    // load → passthrough (.js) → eval == 42.
    {
        auto r{loader.load("./plain.js", "/proj")};
        expect(r.status == LoadStatus::Success, "load ./plain.js Success");
        auto v{eval(r.source)};
        expect(v.has_value() && *v == 42.0, "passthrough .js evals to 42");
    }

    // load (extensionless → .ts) → transpile → eval == 6.
    {
        auto r{loader.load("./calc", "/proj")};
        expect(r.status == LoadStatus::Success, "load ./calc completes to .ts Success");
        auto v{eval(r.source)};
        expect(v.has_value() && *v == 6.0, "transpiled calc.ts evals to 6");
    }

    if (gFailed > 0) {
        std::println("test_module_loader_jsc_smoke: {} failed", gFailed);
        return 1;
    }
    std::println("test_module_loader_jsc_smoke: ok");
    return 0;
}
