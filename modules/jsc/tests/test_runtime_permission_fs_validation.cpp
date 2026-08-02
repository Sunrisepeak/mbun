// Permission fs adapter contract: public validators run before a path denial,
// and node's binding-level callback denials are delivered before the API
// returns. References: node lib/fs.js and src/node_file.cc.
import std;
import mbun.jsc.runtime;

namespace {

void fail(std::string_view message) {
    std::println(std::cerr, "FAIL: {}", message);
}

}  // namespace

int main() {
    using namespace mbun::jsc::runtime;

    // Configure the model before the process-global runtime is first touched.
    // Eval mode has no entry-point grant, so the fixed path below is denied.
    set_permission_command_line({"--permission"}, /*hasEvalString=*/true, {}, {});

    const auto result{eval_to_string(R"JS((() => {
  const fs = require("fs");
  const path = "/tmp/mbun-permission-validation-denied";
  const checks = [];
  const check = (condition, label) => {
    if (!condition) throw new Error(label);
    checks.push(label);
  };
  const throwsCode = (label, code, fn) => {
    let error;
    try { fn(); } catch (caught) { error = caught; }
    check(error && error.code === code, label + ":" + String(error && error.code));
  };

  // Every secondary argument is invalid while `path` itself is valid but
  // denied. The public validator must win over ERR_ACCESS_DENIED.
  throwsCode("access mode", "ERR_INVALID_ARG_TYPE", () => fs.access(path, "bad", () => {}));
  throwsCode("access callback", "ERR_INVALID_ARG_TYPE", () => fs.access(path, 0, 1));
  throwsCode("mkdir recursive", "ERR_INVALID_ARG_TYPE",
             () => fs.mkdir(path, { recursive: "bad" }, () => {}));
  throwsCode("mkdir mode", "ERR_INVALID_ARG_TYPE", () => fs.mkdir(path, { mode: {} }, () => {}));
  throwsCode("mkdir callback", "ERR_INVALID_ARG_TYPE", () => fs.mkdir(path, {}, 1));
  throwsCode("chmod mode", "ERR_INVALID_ARG_TYPE", () => fs.chmod(path, {}, () => {}));
  throwsCode("chmod callback", "ERR_INVALID_ARG_TYPE", () => fs.chmod(path, 0o600, 1));
  throwsCode("utimes atime", "ERR_INVALID_ARG_TYPE", () => fs.utimes(path, {}, 0, () => {}));
  throwsCode("utimes callback", "ERR_INVALID_ARG_TYPE", () => fs.utimes(path, 0, 0, 1));
  throwsCode("lutimes mtime", "ERR_INVALID_ARG_TYPE", () => fs.lutimes(path, 0, {}, () => {}));
  throwsCode("chown uid", "ERR_INVALID_ARG_TYPE", () => fs.chown(path, "bad", 0, () => {}));
  throwsCode("chown callback", "ERR_INVALID_ARG_TYPE", () => fs.chown(path, 0, 0, 1));
  throwsCode("lchown gid", "ERR_INVALID_ARG_TYPE", () => fs.lchown(path, 0, "bad", () => {}));

  let protocolReads = 0;
  const invalidPath = { get protocol() { ++protocolReads; throw new Error("protocol getter"); } };
  throwsCode("invalid path-like", "ERR_INVALID_ARG_TYPE", () => fs.accessSync(invalidPath));
  check(protocolReads === 0, "protocol getter not observed");

  // Pinned node rejects these callback requests from the binding immediately,
  // after validation and before the public API returns.
  for (const name of ["access", "chown", "lchown"]) {
    const order = ["before"];
    const callback = (error) => order.push("callback:" + String(error && error.code));
    if (name === "access") fs[name](path, 0, callback);
    else fs[name](path, 0, 0, callback);
    order.push("after");
    check(order.join(",") === "before,callback:ERR_ACCESS_DENIED,after", name + " ordering");
  }

  // These entries use node's synchronous permission macro even in callback
  // form. Retain the corpus behavior that made the focused file green.
  throwsCode("utimes denied", "ERR_ACCESS_DENIED", () => fs.utimes(path, 0, 0, () => {}));
  throwsCode("lutimes denied", "ERR_ACCESS_DENIED", () => fs.lutimes(path, 0, 0, () => {}));
  throwsCode("mkdir denied", "ERR_ACCESS_DENIED", () => fs.mkdir(path, () => {}));
  throwsCode("chmod denied", "ERR_ACCESS_DENIED", () => fs.chmod(path, 0o600, () => {}));

  return String(checks.length);
})())JS")};

    if (!result) {
        fail(result.error());
        return 1;
    }
    if (*result != "22") {
        fail(std::format("expected 22 checks, got {}", *result));
        return 1;
    }
    std::println("test_runtime_permission_fs_validation: ok");
    return 0;
}
