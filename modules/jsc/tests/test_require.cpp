// test_require.cpp — T3.3b increment 1: CommonJS require() + module cache.
//
// Drives mbun.jsc.runtime's native require (__mbun_require_native(spec, fromDir))
// through the public eval API against real fixtures written to a temp dir:
//   - resolve + read + wrap + eval + module.exports
//   - module cache identity (require twice → same object)
//   - `exports.x =` and `module.exports = {}` forms
//   - circular require sees the partial exports (CommonJS semantics)
//   - relative require from a nested module (per-module __dirname binding)
// The ESM→CJS transpile step is increment 2; here fixtures are hand-written CJS.
import std;
import mbun.jsc.runtime;

namespace {

int gFailed{0};

void expect(bool cond, std::string_view what) {
    if (!cond) {
        ++gFailed;
        std::println("  FAIL: {}", what);
    }
}

void expect_num(std::string_view src, double want) {
    auto r{mbun::jsc::runtime::eval_number(src)};
    expect(r.has_value(), std::format("eval `{}` ok ({})", src, r ? "" : r.error()));
    if (r) expect(*r == want, std::format("eval `{}` == {} (got {})", src, want, *r));
}

std::string q(const std::string& s) { return "\"" + s + "\""; }

}  // namespace

int main() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir{fs::temp_directory_path(ec) / "mbun-require-test"};
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "lib", ec);

    auto write{[&](const fs::path& rel, std::string_view body) {
        std::ofstream f{dir / rel, std::ios::binary};
        f << body;
    }};

    // module.exports = {...} form + a nested lib module.
    write("math.js", "function add(a,b){return a+b;} module.exports = { add: add, PI: 3 };");
    write("lib/str.js", "exports.upper = function (s) { return String(s).toUpperCase(); };");
    // uses a relative require from a nested dir (its own __dirname).
    write("lib/combo.js",
          "var str = require('./str'); exports.shout = function (s) { return str.upper(s) + '!'; };");
    // circular: a <-> b, each reads the other's partial exports.
    write("a.js", "exports.name='a'; var b=require('./b'); exports.bName=b.name;");
    write("b.js", "exports.name='b'; var a=require('./a'); exports.aFromB=a.name;");
    // ESM TypeScript helper: require() must CJS-transpile it (named + default +
    // type erasure + enum) and link it.
    write("esm.ts",
          "export const V = 7; export function dbl(x: number): number { return x*2; }\n"
          "enum K { A, B } export { K };\n"
          "export default function d(): string { return 'def'; }");

    const std::string d{dir.string()};
    // A helper JS expression: require a module from the temp dir.
    auto req{[&](std::string_view spec) {
        return "globalThis.__mbun_require_native(" + q(std::string{spec}) + "," + q(d) + ")";
    }};

    // basic require + module.exports object.
    expect_num(req("./math") + ".add(2,3)", 5.0);
    expect_num(req("./math") + ".PI", 3.0);
    // exports.x form.
    expect_num("(" + req("./lib/str") + ".upper('ab') === 'AB') ? 1 : 0", 1.0);
    // cache identity: two requires yield the same object.
    expect_num("(" + req("./math") + " === " + req("./math") + ") ? 1 : 0", 1.0);
    // nested module's own relative require (per-module __dirname).
    expect_num("(" + req("./lib/combo") + ".shout('hi') === 'HI!') ? 1 : 0", 1.0);
    // circular: a required first; b (loaded during a) sees a's partial exports.
    expect_num("(" + req("./a") + ".bName === 'b') ? 1 : 0", 1.0);
    expect_num("(" + req("./b") + ".aFromB === 'a') ? 1 : 0", 1.0);
    // ESM .ts helper: require() CJS-transpiles + links named/default/enum exports.
    expect_num(req("./esm") + ".V", 7.0);
    expect_num(req("./esm") + ".dbl(21)", 42.0);
    expect_num(req("./esm") + ".K.B", 1.0);              // enum re-export
    expect_num("(" + req("./esm") + ".default() === 'def') ? 1 : 0", 1.0);  // default export
    expect_num("(" + req("./esm") + ".__esModule === true) ? 1 : 0", 1.0);   // interop marker

    // missing module → throws (eval_number returns error → not a value).
    {
        auto r{mbun::jsc::runtime::eval_number(req("./does-not-exist") + " ? 1 : 0")};
        expect(!r.has_value(), "require of missing module throws");
    }

    // node built-in modules (path/assert/os/events + bun) resolve without a file.
    expect_num("(require('node:path').join('a','b','../c') === 'a/c') ? 1 : 0", 1.0);
    expect_num("(require('path').basename('/x/y.ts') === 'y.ts') ? 1 : 0", 1.0);
    expect_num("(require('node:path').resolve('/a','b','c') === '/a/b/c') ? 1 : 0", 1.0);
    expect_num("(require('node:path').extname('a.test.ts') === '.ts') ? 1 : 0", 1.0);
    expect_num("(function(){ try { require('assert').strictEqual(1,2); return 0; } catch(e){ return 1; } })()", 1.0);
    expect_num("(require('node:util').format('%s=%d','a',3) === 'a=3') ? 1 : 0", 1.0);
    expect_num("(require('os').platform() === 'linux') ? 1 : 0", 1.0);
    expect_num("(function(){ const E=require('events'); const e=new E(); let g=0; e.on('x',v=>g=v); e.emit('x',7); return g; })()", 7.0);
    expect_num("(typeof require('bun').stringWidth === 'function') ? 1 : 0", 1.0);

    fs::remove_all(dir, ec);

    if (gFailed > 0) {
        std::println("test_require: {} failed", gFailed);
        return 1;
    }
    std::println("test_require: ok");
    return 0;
}
