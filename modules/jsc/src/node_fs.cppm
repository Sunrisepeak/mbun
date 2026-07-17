// Node fs/fs-promises host-function descriptor seam.
//
// References:
//   bun Rust: src/runtime/node/{node_fs,node_fs_binding}.rs
//   bun Zig:  src/runtime/node/{node_fs,node_fs_binding}.zig
//
// This module translates only the compile-time dispatch metadata shared by
// Bun's sync and async bindings. It intentionally contains no syscalls, JSC
// values, promises, event-loop ownership, Stats, Dirent, or watcher ABI.
export module mbun.jsc.node_fs;

import std;

export namespace mbun::jsc::node_fs {

enum class AsyncBackend : std::uint8_t {
    work_pool,
    uv_request,
    cp_task,
};

enum class BindingPath : std::uint8_t {
    generated,
    handwritten_cp,
    handwritten_readdir,
};

struct Operation {
    std::string_view name;
    std::string_view syncName;
    std::string_view implementation;
    AsyncBackend asyncBackend;
    BindingPath bindingPath;
    bool hasAbortSignal;
    bool hasRecursiveAsyncOverride;
};

// Mirrors the binding rows in Bun's Rust node_fs_binding.rs and the equivalent
// declarations in Zig. `implementation` records the underlying NodeFS enum:
// notably JS `realpath` dispatches RealpathNonNative, while `realpathNative`
// dispatches Realpath. Argument counts are deliberately absent because Bun
// validates them in each FsArgument::from_js implementation, not this table.
inline constexpr std::array OPERATIONS {
    Operation { "access", "accessSync", "access", AsyncBackend::work_pool, BindingPath::generated, false, false },
    Operation { "appendFile", "appendFileSync", "appendFile", AsyncBackend::work_pool, BindingPath::generated, false, false },
    Operation { "close", "closeSync", "close", AsyncBackend::uv_request, BindingPath::generated, false, false },
    Operation { "copyFile", "copyFileSync", "copyFile", AsyncBackend::work_pool, BindingPath::generated, false, false },
    Operation { "cp", "cpSync", "cp", AsyncBackend::cp_task, BindingPath::handwritten_cp, false, false },
    Operation { "exists", "existsSync", "exists", AsyncBackend::work_pool, BindingPath::generated, false, false },
    Operation { "chown", "chownSync", "chown", AsyncBackend::work_pool, BindingPath::generated, false, false },
    Operation { "chmod", "chmodSync", "chmod", AsyncBackend::work_pool, BindingPath::generated, false, false },
    Operation { "fchmod", "fchmodSync", "fchmod", AsyncBackend::work_pool, BindingPath::generated, false, false },
    Operation { "fchown", "fchownSync", "fchown", AsyncBackend::work_pool, BindingPath::generated, false, false },
    Operation { "fdatasync", "fdatasyncSync", "fdatasync", AsyncBackend::work_pool, BindingPath::generated, false, false },
    Operation { "fstat", "fstatSync", "fstat", AsyncBackend::work_pool, BindingPath::generated, false, false },
    Operation { "fsync", "fsyncSync", "fsync", AsyncBackend::work_pool, BindingPath::generated, false, false },
    Operation { "ftruncate", "ftruncateSync", "ftruncate", AsyncBackend::work_pool, BindingPath::generated, false, false },
    Operation { "futimes", "futimesSync", "futimes", AsyncBackend::work_pool, BindingPath::generated, false, false },
    Operation { "lchmod", "lchmodSync", "lchmod", AsyncBackend::work_pool, BindingPath::generated, false, false },
    Operation { "lchown", "lchownSync", "lchown", AsyncBackend::work_pool, BindingPath::generated, false, false },
    Operation { "link", "linkSync", "link", AsyncBackend::work_pool, BindingPath::generated, false, false },
    Operation { "lstat", "lstatSync", "lstat", AsyncBackend::work_pool, BindingPath::generated, false, false },
    Operation { "mkdir", "mkdirSync", "mkdir", AsyncBackend::work_pool, BindingPath::generated, false, false },
    Operation { "mkdtemp", "mkdtempSync", "mkdtemp", AsyncBackend::work_pool, BindingPath::generated, false, false },
    Operation { "open", "openSync", "open", AsyncBackend::uv_request, BindingPath::generated, false, false },
    Operation { "read", "readSync", "read", AsyncBackend::uv_request, BindingPath::generated, false, false },
    Operation { "write", "writeSync", "write", AsyncBackend::uv_request, BindingPath::generated, false, false },
    Operation { "readdir", "readdirSync", "readdir", AsyncBackend::work_pool, BindingPath::handwritten_readdir, false, true },
    Operation { "readFile", "readFileSync", "readFile", AsyncBackend::work_pool, BindingPath::generated, true, false },
    Operation { "writeFile", "writeFileSync", "writeFile", AsyncBackend::work_pool, BindingPath::generated, true, false },
    Operation { "readlink", "readlinkSync", "readlink", AsyncBackend::work_pool, BindingPath::generated, false, false },
    Operation { "readv", "readvSync", "readv", AsyncBackend::uv_request, BindingPath::generated, false, false },
    Operation { "realpath", "realpathSync", "realpathNonNative", AsyncBackend::work_pool, BindingPath::generated, false, false },
    Operation { "realpathNative", "realpathNativeSync", "realpath", AsyncBackend::work_pool, BindingPath::generated, false, false },
    Operation { "rename", "renameSync", "rename", AsyncBackend::work_pool, BindingPath::generated, false, false },
    Operation { "rm", "rmSync", "rm", AsyncBackend::work_pool, BindingPath::generated, false, false },
    Operation { "rmdir", "rmdirSync", "rmdir", AsyncBackend::work_pool, BindingPath::generated, false, false },
    Operation { "stat", "statSync", "stat", AsyncBackend::work_pool, BindingPath::generated, false, false },
    Operation { "statfs", "statfsSync", "statfs", AsyncBackend::uv_request, BindingPath::generated, false, false },
    Operation { "symlink", "symlinkSync", "symlink", AsyncBackend::work_pool, BindingPath::generated, false, false },
    Operation { "truncate", "truncateSync", "truncate", AsyncBackend::work_pool, BindingPath::generated, false, false },
    Operation { "unlink", "unlinkSync", "unlink", AsyncBackend::work_pool, BindingPath::generated, false, false },
    Operation { "utimes", "utimesSync", "utimes", AsyncBackend::work_pool, BindingPath::generated, false, false },
    Operation { "lutimes", "lutimesSync", "lutimes", AsyncBackend::work_pool, BindingPath::generated, false, false },
    Operation { "writev", "writevSync", "writev", AsyncBackend::uv_request, BindingPath::generated, false, false },
};

inline constexpr std::span<const Operation> operations() noexcept {
    return OPERATIONS;
}

inline constexpr const Operation* find_operation(std::string_view name) noexcept {
    for (const auto& operation : OPERATIONS) {
        if (operation.name == name || operation.syncName == name) {
            return &operation;
        }
    }
    return nullptr;
}

inline constexpr bool is_fs_module(std::string_view specifier) noexcept {
    return specifier == "fs" || specifier == "node:fs";
}

inline constexpr bool is_promises_module(std::string_view specifier) noexcept {
    return specifier == "fs/promises" || specifier == "node:fs/promises";
}

}  // namespace mbun::jsc::node_fs
