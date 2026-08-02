// T1.5 CLI 骨架测试 — 子命令分发与 --version/--help 快速路径。
// 参考 bun CLI 行为（bun --version 输出裸版本号；无参数输出用法帮助）。
// bun Rust 版对应优化: commit 3e1c9a77 (clap 前置 fast-path) — mbun 直接以
// 单遍手写分发实现，无需完整参数解析库介入热路径。
import std;
import mbun.cli;

namespace {

int gFailed = 0;

void expect(bool cond, std::string_view what) {
    if (!cond) {
        ++gFailed;
        std::println("  FAIL: {}", what);
    }
}

} // namespace

int main() {
    using mbun::cli::Action;
    using mbun::cli::parse;
    using mbun::cli::parse_build;
    using mbun::cli::parse_test;
    using mbun::cli::resolve_tsconfig_override_path;
    using mbun::cli::take_tsconfig_override;

    // --version / -v → 裸版本号快速路径
    expect(parse({"--version"}).action == Action::Version, "--version → Version");
    expect(parse({"-v"}).action == Action::Version, "-v → Version");

    // --help / -h / help → 用法帮助
    expect(parse({"--help"}).action == Action::Help, "--help → Help");
    expect(parse({"-h"}).action == Action::Help, "-h → Help");
    expect(parse({"help"}).action == Action::Help, "help → Help");

    // 无参数 → 帮助（同 bun 行为）
    expect(parse({}).action == Action::Help, "no args → Help");

    // `install` is a real command routed to modules/install.
    expect(parse({"install"}).action == Action::Install, "install → Install");
    expect(parse({"i"}).action == Action::Install, "i → Install (bun alias)");

    // `build` 由 R7 接通：路由到 Build，flag 解析交给 parse_build。
    {
        auto p = parse({"build"});
        expect(p.action == Action::Build, "build → Build");
        expect(p.argument == "build", "build 保留原始命令名");
    }

    // parse_build：entrypoint/值 flag（`--flag value` 与 `--flag=value` 两种形式）
    // 依据 bun BUILD_ONLY_PARAMS(src/cli/Arguments.rs :401-550)。
    {
        auto b = parse_build({"./a.ts", "--outdir", "out", "--target=bun"});
        expect(b.parseError.empty(), "干净解析无 parseError");
        expect(b.entryPoints.size() == 1 && b.entryPoints[0] == "./a.ts", "操作数即 entrypoint");
        expect(b.outdir == "out", "--outdir <v> 取下一个 token");
        expect(b.target == "bun", "--target=<v> 内联值");
        expect(b.sourcemap == "none", "默认无 sourcemap");
        expect(b.format == "esm", "默认 format=esm");
    }
    {
        // `--sourcemap` 是 `<STR>?`：裸 flag 意为 linked（Arguments.rs :2406-2417）。
        auto b = parse_build({"a.ts", "--sourcemap"});
        expect(b.sourcemap == "linked", "裸 --sourcemap → linked");
        expect(b.entryPoints.size() == 1, "裸 --sourcemap 不吞掉 entrypoint");
    }
    {
        auto b = parse_build({"a.ts", "--target=wasm"});
        expect(!b.parseError.empty(), "非法 target 报错");
    }
    {
        // 未实现的 flag 必须被识别并上报，不能静默忽略。
        auto b = parse_build({"a.ts", "--minify-syntax"});
        expect(b.parseError.empty(), "--minify-syntax 是已知 flag，非解析错误");
        expect(b.unsupported.size() == 1 && b.unsupported[0] == "--minify-syntax",
               "--minify-syntax 记入 unsupported");
    }
    {
        auto b = parse_build({"a.ts", "--nope"});
        expect(!b.parseError.empty(), "未知 flag 报错");
    }
    {
        auto b = parse_build({"-e", "react", "--external", "lodash", "a.ts"});
        expect(b.external.size() == 2, "-e/--external 累积成列表");
        expect(b.entryPoints.size() == 1 && b.entryPoints[0] == "a.ts", "值 flag 不吞 entrypoint");
    }
    {
        auto b = parse_build({"a.ts", "--tsconfig-override", "config/build.json"});
        expect(b.parseError.empty(), "build accepts shared --tsconfig-override");
        expect(b.tsconfigOverride == std::optional<std::string>{"config/build.json"},
               "build preserves tsconfig override for resolver routing");
        expect(std::ranges::none_of(b.unsupported, [](const std::string& flag) {
                   return flag == "--tsconfig-override";
               }),
               "build does not misclassify tsconfig override as unsupported");
    }
    {
        auto b = parse_build({"a.ts", "--tsconfig-override"});
        expect(!b.parseError.empty(), "build rejects dangling --tsconfig-override");
    }

    // `mbun test <file>` → Test（T3.4 S1 解锁：直跑 bun 原生测试文件）
    // 操作数/flag 现由 parse_test() 二次解析（parse() 只做子命令分发）。
    {
        auto p = parse({"test", "some/file.test.ts"});
        expect(p.action == Action::Test, "test <file> → Test");
        auto t = parse_test({"some/file.test.ts"});
        expect(t.filters.size() == 1 && t.filters[0] == "some/file.test.ts",
               "test 保留文件路径操作数 (parse_test)");
    }
    {
        // 跳过 flag，取非 flag 操作数为过滤器
        auto t = parse_test({"--bail", "a.test.ts"});
        expect(t.filters.size() == 1 && t.filters[0] == "a.test.ts", "test 跳过 flag 取文件操作数");
    }
    {
        auto p = parse({"test"});
        auto t = parse_test({});
        expect(p.action == Action::Test && t.filters.empty(), "test 无文件 → 空过滤器");
    }
    {
        auto t = parse_test({"--tsconfig-override=custom.json", "math.test.ts"});
        expect(t.parseError.empty(), "test accepts shared --tsconfig-override");
        expect(t.tsconfigOverride == std::optional<std::string>{"custom.json"},
               "test preserves tsconfig override for runtime routing");
        expect(t.filters.size() == 1 && t.filters[0] == "math.test.ts",
               "test override does not swallow test filter");
    }
    {
        auto t = parse_test({"--tsconfig-override"});
        expect(!t.parseError.empty(), "test rejects dangling --tsconfig-override");
    }

    // TRANSPILER_PARAMS_ is shared by run/test/build. Its required-value reader
    // must agree for split/inline spellings and reject a dangling occurrence.
    {
        mbun::cli::TsconfigOverrideArg option{};
        const std::array<std::string_view, 2> split{"--tsconfig-override", "config.json"};
        expect(take_tsconfig_override(split, 0, option) == 2,
               "shared tsconfig reader consumes split spelling");
        expect(option.value == std::optional<std::string>{"config.json"},
               "shared tsconfig reader keeps split value");
    }
    {
        mbun::cli::TsconfigOverrideArg option{};
        const std::array<std::string_view, 1> dangling{"--tsconfig-override"};
        expect(take_tsconfig_override(dangling, 0, option) == 1,
               "shared tsconfig reader consumes dangling flag");
        expect(!option.parseError.empty(), "shared tsconfig reader reports missing value");
    }
    {
        const std::filesystem::path cwd{"/workspace/project"};
        expect(resolve_tsconfig_override_path("config/tsconfig.json", cwd) ==
                   "/workspace/project/config/tsconfig.json",
               "tsconfig override resolves against post---cwd working directory");
    }

    // 未知命令 → Unknown + 保留输入
    {
        auto p = parse({"frobnicate"});
        expect(p.action == Action::Unknown, "unknown → Unknown");
        expect(p.argument == "frobnicate", "unknown 保留原始输入");
    }

    // 版本号常量与清单一致性由人工维护，此处仅约束非空与语义化格式
    expect(!mbun::cli::VERSION.empty(), "VERSION 非空");
    expect(std::ranges::count(mbun::cli::VERSION, '.') == 2, "VERSION 为 x.y.z 形式");

    if (gFailed > 0) {
        std::println("test_cli: {} failed", gFailed);
        return 1;
    }
    std::println("test_cli: ok");
    return 0;
}
