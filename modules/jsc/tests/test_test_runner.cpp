// test_test_runner.cpp — T3.4 mbun.jsc.test_runner acceptance.
//
// Two layers, per the T3.4 plan:
//   1. prepare_source (pure logic, no VM): the `import ... from "bun:test"`
//      rewrite / bare-import strip that makes an ESM test file evaluable as a
//      JSC script.
//   2. run_source (JSC end-to-end smoke): install the bun:test harness, register
//      an inline `bun:test` script, drive collection → execution (through JSC's
//      end-of-script microtask drain for async tests) → summarise pass/fail/skip
//      and expect() call counts. This is the S1-unlock capability: a bun:test
//      file runs and reports the same pass/fail tally as bun.
//
// ref (semantics): .mbun/bun-zig-src/src/test_runner/{Collection,Execution}.zig
//   (collection discovers describe/test; execution runs
//    beforeAll → (beforeEach, test, afterEach)* → afterAll, nested by scope).
import std;
import mbun.jsc.test_runner;
import mbun.jsc.runtime;

namespace {

int gFailed{0};

void expect(bool cond, std::string_view what) {
    if (!cond) {
        ++gFailed;
        std::println("  FAIL: {}", what);
    }
}

void expect_eq(int got, int want, std::string_view what) {
    if (got != want) {
        ++gFailed;
        std::println("  FAIL: {} (got {}, want {})", what, got, want);
    }
}

}  // namespace

int main() {
    using mbun::jsc::test_runner::prepare_source;
    using mbun::jsc::test_runner::run_source;
    using mbun::jsc::test_runner::RunResult;

    // ── prepare_source: ESM → script-mode rewrite (pure logic) ───────────────
    {
        std::string out{prepare_source("import { test, expect } from \"bun:test\";\ntest(\"a\", () => {});\n")};
        expect(out.find("import") == std::string::npos, "bun:test import statement removed");
        expect(out.find("globalThis.__mbunBT") != std::string::npos,
               "bun:test import rewritten to __mbunBT destructure");
        expect(out.find("test(\"a\"") != std::string::npos, "test body preserved after rewrite");
    }
    {
        // `as` alias inside the named clause → destructuring rename.
        std::string out{prepare_source("import { it as t } from 'bun:test';\n")};
        expect(out.find("it: t") != std::string::npos, "`it as t` becomes `it: t` destructure");
    }
    {
        // A bare import of an unsupported specifier is stripped (name then fails
        // honestly at runtime rather than a whole-file syntax error).
        std::string out{prepare_source("import { peek } from \"bun\";\nlet x = 1;\n")};
        expect(out.find("import") == std::string::npos, "non-bun:test import stripped");
        expect(out.find("let x = 1") != std::string::npos, "following code preserved");
    }

    // ── run_source: bun:test end-to-end through JSC ──────────────────────────
    // Scenario A — a single passing test.
    {
        RunResult r{run_source(
            "import { test, expect } from \"bun:test\";\n"
            "test(\"a\", () => { expect(1 + 1).toBe(2); });\n")};
        expect(r.ok, "A: run_source ok");
        expect(r.error.empty(), "A: no error");
        expect_eq(r.pass, 1, "A: pass");
        expect_eq(r.fail, 0, "A: fail");
        expect_eq(r.total, 1, "A: total");
        expect_eq(r.expect_calls, 1, "A: expect() calls");
    }

    // Scenario B — one pass, one fail; failing test captured, not aborting.
    {
        RunResult r{run_source(
            "import { test, expect } from \"bun:test\";\n"
            "test(\"p\", () => { expect(1).toBe(1); });\n"
            "test(\"f\", () => { expect(1).toBe(2); });\n")};
        expect(r.ok, "B: run_source ok");
        expect_eq(r.pass, 1, "B: pass");
        expect_eq(r.fail, 1, "B: fail");
        expect_eq(r.total, 2, "B: total");
        expect(r.body.find("(fail)") != std::string::npos, "B: body reports a (fail) line");
    }

    // Scenario C — describe nesting + hook ordering (outer→inner beforeEach,
    // inner→outer afterEach), verified by an observation log.
    {
        RunResult r{run_source(
            "import { test, expect, describe, beforeEach, afterEach, beforeAll, afterAll } from \"bun:test\";\n"
            "globalThis.__log = [];\n"
            "beforeAll(() => globalThis.__log.push('bA'));\n"
            "afterAll(() => globalThis.__log.push('aA'));\n"
            "beforeEach(() => globalThis.__log.push('bE'));\n"
            "afterEach(() => globalThis.__log.push('aE'));\n"
            "describe('g', () => {\n"
            "  beforeEach(() => globalThis.__log.push('bE2'));\n"
            "  test('t1', () => { globalThis.__log.push('t1'); expect(1).toBe(1); });\n"
            "});\n"
            "test('t0', () => { globalThis.__log.push('t0'); expect(1).toBe(1); });\n")};
        expect(r.ok, "C: run_source ok");
        expect_eq(r.pass, 2, "C: pass");
        expect_eq(r.total, 2, "C: total");
        auto log{mbun::jsc::runtime::eval_to_string("globalThis.__log.join(',')")};
        expect(log.has_value() && *log == "bA,bE,bE2,t1,aE,bE,t0,aE,aA",
               "C: hook + test execution order matches bun scope semantics");
    }

    // Scenario D — async test resolves through JSC's end-of-script microtask
    // drain (Promise/await; no timers). This is the critical async smoke.
    {
        RunResult r{run_source(
            "import { test, expect } from \"bun:test\";\n"
            "test('async', async () => { const x = await Promise.resolve(41); expect(x + 1).toBe(42); });\n")};
        expect(r.ok, "D: run_source ok");
        expect_eq(r.pass, 1, "D: async pass");
        expect_eq(r.fail, 0, "D: async fail");
    }

    // Scenario E — matcher coverage (toEqual, .not, toThrow) + test.skip.
    {
        RunResult r{run_source(
            "import { test, expect } from \"bun:test\";\n"
            "test('eq', () => { expect([1,2,{a:3}]).toEqual([1,2,{a:3}]); });\n"
            "test('not', () => { expect(1).not.toBe(2); });\n"
            "test('throws', () => { expect(() => { throw new Error('boom'); }).toThrow('boom'); });\n"
            "test.skip('skipped', () => { expect(1).toBe(2); });\n")};
        expect(r.ok, "E: run_source ok");
        expect_eq(r.pass, 3, "E: pass");
        expect_eq(r.fail, 0, "E: fail");
        expect_eq(r.skip, 1, "E: skip");
        expect_eq(r.total, 4, "E: total");
        expect_eq(r.expect_calls, 3, "E: expect() calls (skipped body not run)");
    }

    // Scenario F — a genuinely failing .not / toThrow negative path counts fail.
    {
        RunResult r{run_source(
            "import { test, expect } from \"bun:test\";\n"
            "test('bad', () => { expect(() => 1).toThrow(); });\n")};
        expect_eq(r.fail, 1, "F: non-throwing fn under toThrow fails");
    }

    // Scenario G — a pending async toThrow (expect(asyncFn).toThrow() whose
    // inner promise never settles) must time out THAT test and let the rest of
    // the file run + report (regression: serve.test.ts wedged to "0 tests").
    {
        RunResult r{run_source(
            "import { test, expect } from \"bun:test\";\n"
            "test('before', () => { expect(1).toBe(1); });\n"
            "test('hangs', () => { expect(async () => { await new Promise(() => {}); }).toThrow('never'); });\n"
            "test('after', () => { expect(2).toBe(2); });\n")};
        expect(r.ok, "G: run completes despite a pending async assert");
        expect_eq(r.total, 3, "G: all tests observed");
        expect_eq(r.pass, 2, "G: surrounding tests still pass");
        expect_eq(r.fail, 1, "G: the hanging test fails individually");
    }

    // Scenario H — timers take real wall-clock time (roadmap: elapsed >= N
    // assertions across the suite depend on this) and Bun.sleepSync blocks.
    {
        const auto t0{std::chrono::steady_clock::now()};
        RunResult r{run_source(
            "import { test, expect } from \"bun:test\";\n"
            "test('real delay', async () => {\n"
            "  const s = Date.now(); await Bun.sleep(60);\n"
            "  expect(Date.now() - s >= 55).toBe(true);\n"
            "  const s2 = Date.now(); Bun.sleepSync(30);\n"
            "  expect(Date.now() - s2 >= 29).toBe(true);\n"
            "  const s3 = Date.now();\n"
            "  await new Promise((res) => setTimeout(res, 40));\n"
            "  expect(Date.now() - s3 >= 35).toBe(true);\n"
            "});\n")};
        const auto elapsed{std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - t0)
                               .count()};
        expect(r.ok && r.fail == 0 && r.pass == 1,
               "H: real-delay assertions pass (fail=" + std::to_string(r.fail) + ")");
        expect(elapsed >= 120.0, "H: wall clock actually elapsed (got " +
                                     std::to_string(elapsed) + "ms)");
    }

    // Scenario I — unref'd timers don't hold the run open (node semantics).
    {
        RunResult r{run_source(
            "import { test, expect } from \"bun:test\";\n"
            "test('unref', () => { setInterval(() => {}, 60000).unref(); expect(1).toBe(1); });\n")};
        expect(r.ok && r.pass == 1 && r.fail == 0, "I: unref'd interval doesn't wedge the run");
    }

    // Scenario J — a done-style body may also return a Promise. Calling done()
    // does not hide a later rejection from that Promise (regression: one shared
    // settlement guard made this a false pass).
    {
        RunResult r{run_source(
            "import { test } from \"bun:test\";\n"
            "test('done then reject', async (done) => {\n"
            "  done();\n"
            "  await Promise.resolve();\n"
            "  throw new Error('late rejection');\n"
            "});\n")};
        expect(r.ok, "J: run_source completes");
        expect_eq(r.pass, 0, "J: done does not hide returned Promise rejection");
        expect_eq(r.fail, 1, "J: returned Promise rejection fails the test");
        expect(r.body.find("late rejection") != std::string::npos,
               "J: returned Promise rejection is reported");
    }

    // Scenario K — package-manager symlinks make require.resolve() return a
    // lexical path while the native loader caches the canonical target. A late
    // mock must still find that loaded exports object and fan the replacement
    // out through the named-import subscription.
    {
        const auto nonce{std::chrono::steady_clock::now().time_since_epoch().count()};
        const std::filesystem::path root{
            std::filesystem::temp_directory_path() /
            std::format("mbun-test-runner-late-mock-{}", nonce)};
        const std::filesystem::path packageRoot{
            root / "node_modules/.store/pkg/node_modules/pkg"};
        std::filesystem::create_directories(packageRoot);
        std::filesystem::create_directory_symlink(".store/pkg/node_modules/pkg",
                                                   root / "node_modules/pkg");
        {
            std::ofstream out{packageRoot / "package.json"};
            out << R"({"name":"pkg","version":"1.0.0","main":"index.js"})";
        }
        {
            std::ofstream out{packageRoot / "index.js"};
            out << "export const value = () => 'original';\n";
        }
        {
            std::ofstream out{root / "subject.ts"};
            out << "import { value } from 'pkg';\n"
                   "export const observed = () => value();\n";
        }
        const std::filesystem::path testFile{root / "late-mock.test.ts"};
        {
            std::ofstream out{testFile};
            out << "import { expect, mock, test } from 'bun:test';\n"
                   "import { observed } from './subject.ts';\n"
                   "mock.module(require.resolve('pkg'), () => ({ value: () => 'mocked' }));\n"
                   "test('late mock', () => expect(observed()).toBe('mocked'));\n";
        }
        RunResult r{mbun::jsc::test_runner::run_file(testFile.string())};
        expect(r.ok, "K: symlink-package test completes");
        expect_eq(r.pass, 1, "K: late mock updates the loaded named import");
        expect_eq(r.fail, 0, "K: symlinked cache identity does not strand the original");
        std::filesystem::remove_all(root);
    }

    if (gFailed > 0) {
        std::println("test_test_runner: {} failed", gFailed);
        return 1;
    }
    std::println("test_test_runner: ok");
    return 0;
}
