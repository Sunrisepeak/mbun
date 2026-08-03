// Focused end-to-end coverage for Bun's `$` shell template tag.
// Ref: bun src/js/builtins/shell.ts and compat/bun/test/js/bun/util/which.test.ts.
import std;
import mbun.jsc.runtime;

namespace {

std::string js_quote(std::string_view input) {
    std::string output{"\""};
    for (const unsigned char byte : input) {
        switch (byte) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (byte < 0x20U) {
                    output += std::format("\\u{:04x}", byte);
                } else {
                    output.push_back(static_cast<char>(byte));
                }
        }
    }
    output.push_back('"');
    return output;
}

class TempDirectory {
public:
    TempDirectory() {
        const auto stamp{std::chrono::steady_clock::now().time_since_epoch().count()};
        path_ = std::filesystem::temp_directory_path() /
                std::format("mbun-shell-tag-{}", stamp);
        std::filesystem::create_directories(path_);
        std::ofstream{path_ / "relative-tool"} << "#!/bin/sh\nprintf 'relative-ok'\n";
    }

    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace

int main() {
#if defined(_WIN32)
    std::println("test_runtime_shell_tag: skipped on Windows until the Windows shell backend lands");
    return 0;
#else
    TempDirectory temp;
    const std::string root{temp.path().string()};
    const std::string source{std::format(R"JS(
globalThis.__mbunShellTagFocused = {{ done: false, error: "", checks: [] }};
(async () => {{
  const {{ $ }} = require("bun");
  const check = (condition, name) => {{
    if (!condition) throw new Error(name);
    __mbunShellTagFocused.checks.push(name);
  }};

  check(typeof $ === "function", "bun exports the shell tag");
  check($.nothrow() === $, "$.nothrow mutates and returns the tag");
  const noThrow = await $`exit 7`;
  check(noThrow.exitCode === 7, "$.nothrow resolves nonzero exits");

  $.throws(true);
  let shellError;
  try {{ await $`printf failure 1>&2; exit 9`; }} catch (error) {{ shellError = error; }}
  check(shellError && shellError.name === "ShellError" && shellError.exitCode === 9,
        "$.throws rejects with ShellError");
  check(shellError && shellError.stderr.toString().includes("failure"),
        "ShellError preserves stderr");

  let depth100 = "depth-ok";
  for (let index = 0; index < 100; ++index) depth100 = [depth100];
  const depthOutput = await $`printf "%s" ${{depth100}}`.text();
  check(depthOutput === "depth-ok", "100 nested template arrays are accepted");
  let depth101 = "too-deep";
  for (let index = 0; index < 101; ++index) depth101 = [depth101];
  let depthError;
  try {{ $`printf "%s" ${{depth101}}`; }} catch (error) {{ depthError = error; }}
  check(String(depthError).includes("cannot be nested more than 100"),
        "101 nested template arrays are rejected");

  const injected = "hello world;$(printf injected)";
  const interpolated = await $`printf "%s" ${{injected}}`.text();
  check(interpolated === injected, "template values are escaped as one shell word");
  const nestedMarker = {} + "/nested-injection-marker";
  const nestedInjection = "x; touch " + nestedMarker;
  const nestedOutput = await $`echo "$(printf '%s' ${{nestedInjection}})"`.text();
  check(nestedOutput.trim() === nestedInjection,
        "nested command substitution interpolation remains data");

  const cwd = await $`pwd`.cwd({}).text();
  check(cwd.trim() === {}, "ShellPromise.cwd controls execution cwd");
  const env = await $`printf "%s" "$SHELL_TAG_VALUE"`
    .env({{ ...process.env, SHELL_TAG_VALUE: "env-ok" }}).text();
  check(env === "env-ok", "ShellPromise.env controls execution env");
  const mutableEnv = {{ ...process.env, SHELL_TAG_SNAPSHOT: "before" }};
  const snapshotPromise = $`printf "%s" "$SHELL_TAG_SNAPSHOT"`.env(mutableEnv);
  mutableEnv.SHELL_TAG_SNAPSHOT = "after";
  const snapshot = await snapshotPromise.text();
  check(snapshot === "before", "ShellPromise.env snapshots immediately");

  const output = await $`printf shape`.quiet();
  check(typeof $.ShellOutput === "undefined" && output.constructor.name === "ShellOutput" &&
        Object.keys(output).sort().join(",") === "exitCode,stderr,stdout" &&
        output.exitCode === 0 &&
        Buffer.isBuffer(output.stdout) && Buffer.isBuffer(output.stderr) &&
        output.text() === "shape",
        "ShellOutput exposes Bun-compatible output shape");
  const lines = [];
  for await (const line of $`printf 'one\ntwo\n'`.lines()) lines.push(line);
  check(JSON.stringify(lines) === JSON.stringify(["one", "two", ""]),
        "ShellPromise.lines yields Bun line splitting semantics");

  await $`chmod +x ./relative-tool`.cwd({});
  const relative = await $`./relative-tool`.cwd({}).text();
  check(relative === "relative-ok", "chmod and relative commands execute in cwd");

  __mbunShellTagFocused.count = __mbunShellTagFocused.checks.length;
  __mbunShellTagFocused.done = true;
}})().catch((error) => {{
  __mbunShellTagFocused.error = String(error && error.message) + " | " +
    String(error && (error.stack || error));
  __mbunShellTagFocused.done = true;
}});
)JS",
                                                js_quote(root), js_quote(root), js_quote(root),
                                                js_quote(root), js_quote(root))};

    const auto started{mbun::jsc::runtime::eval(source, "shell-tag-focused.js")};
    if (!started) {
        std::println("test_runtime_shell_tag: setup failed: {}", started.error());
        return 1;
    }
    mbun::jsc::runtime::pump_event_loop("globalThis.__mbunShellTagFocused.done");
    const auto result{mbun::jsc::runtime::eval_to_string(
        "JSON.stringify(globalThis.__mbunShellTagFocused)")};
    if (!result) {
        std::println("test_runtime_shell_tag: result failed: {}", result.error());
        return 1;
    }
    std::println("{}", *result);
    if (result->find("\"error\":\"\"") == std::string::npos ||
        result->find("\"count\":15") == std::string::npos ||
        std::filesystem::exists(temp.path() / "nested-injection-marker")) {
        return 1;
    }
    std::println("test_runtime_shell_tag: ok");
    return 0;
#endif
}
