// Descriptor-only vectors for the first Node fs translation seam. These test
// Bun's Rust/Zig binding table shape; no syscall or JSC compatibility is
// claimed by this file.
import std;
import mbun.jsc.node_fs;

namespace {

using namespace mbun::jsc::node_fs;

int gFailed{};

void check(bool condition, std::string_view message) {
    if (condition) return;
    ++gFailed;
    std::println(std::cerr, "FAIL: {}", message);
}

void check_operation(std::string_view name, std::string_view syncName,
                     AsyncBackend backend) {
    const auto* operation{find_operation(name)};
    check(operation != nullptr, std::string{name} + " is described");
    if (operation == nullptr) return;
    check(operation->syncName == syncName, std::string{name} + " sync alias");
    check(operation->asyncBackend == backend, std::string{name} + " async backend");
    check(find_operation(syncName) == operation, std::string{syncName} + " resolves to same row");
}

}  // namespace

int main() {
    check(operations().size() == 42, "Rust/Zig binding table has 42 translated rows");

    check_operation("open", "openSync", AsyncBackend::uv_request);
    check_operation("close", "closeSync", AsyncBackend::uv_request);
    check_operation("read", "readSync", AsyncBackend::uv_request);
    check_operation("writev", "writevSync", AsyncBackend::uv_request);
    check_operation("fdatasync", "fdatasyncSync", AsyncBackend::work_pool);
    check_operation("cp", "cpSync", AsyncBackend::cp_task);

    const auto* readFile{find_operation("readFile")};
    const auto* writeFile{find_operation("writeFileSync")};
    check(readFile != nullptr && readFile->hasAbortSignal, "readFile carries abort signal metadata");
    check(writeFile != nullptr && writeFile->hasAbortSignal, "writeFile carries abort signal metadata");

    const auto* readdir{find_operation("readdir")};
    check(readdir != nullptr && readdir->hasRecursiveAsyncOverride,
          "recursive readdir selects its dedicated async task");
    check(readdir != nullptr && readdir->asyncBackend == AsyncBackend::work_pool,
          "non-recursive readdir keeps the generic work-pool task");

    const auto* realpath{find_operation("realpath")};
    const auto* native{find_operation("realpathNative")};
    check(realpath != nullptr && realpath->implementation == "realpathNonNative",
          "JS realpath maps to Bun's emulated implementation");
    check(native != nullptr && native->implementation == "realpath",
          "internal realpathNative maps to Bun's native implementation");

    check(is_fs_module("fs") && is_fs_module("node:fs"), "fs aliases are recognized");
    check(is_promises_module("fs/promises") && is_promises_module("node:fs/promises"),
          "fs/promises aliases are recognized");
    check(find_operation("watch") == nullptr, "hand-written watcher ABI is outside descriptor table");
    check(find_operation("not-an-fs-operation") == nullptr, "unknown operation is rejected");

    if (gFailed != 0) {
        std::println(std::cerr, "test_node_fs_descriptor: {} failed", gFailed);
        return 1;
    }
    std::println("test_node_fs_descriptor: ok");
    return 0;
}
