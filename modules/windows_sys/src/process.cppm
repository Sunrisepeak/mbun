// Process creation contract for the future Win32 backend.
// Ref: bun-zig-src/src/windows_sys/externs.zig (OpenProcess/job APIs) and
// bun-ref/src/windows_sys/externs.rs. CreateProcessW/job setup is DEFERRED.
export module mbun.windows_sys.process;

import std;
import mbun.windows_sys.error;

export namespace mbun::windows_sys {

using ProcessId = std::uint32_t;

struct ProcessRequest {
    std::u16string_view image{};
    std::span<const std::u16string_view> arguments{};
    std::u16string_view workingDirectory{};
};

struct ProcessResult {
    ProcessId processId{0};
    std::intptr_t processHandle{-1};
};

inline std::expected<ProcessResult, Error> launch_deferred(ProcessRequest request) {
    (void)request;
    return std::unexpected(deferred_error("CreateProcessW"));
}

} // namespace mbun::windows_sys
