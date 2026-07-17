// Injectable native seam for JSC conversion, filesystem and git spawn.
// Rust and Zig both keep these effects in patch_jsc/testing rather than in the
// pure patch parser; std::function keeps the first MC++ slice portable.
export module mbun.patch_jsc.backend;

import std;
import mbun.patch_jsc.error;

export namespace mbun::patch_jsc {

struct NativeBackend {
    using Parse = std::function<Result<std::string>(std::string_view)>;
    using Apply = std::function<VoidResult(std::string_view, std::string_view)>;
    using MakeDiff = std::function<Result<std::string>(std::string_view, std::string_view)>;

    Parse parse {};
    Apply apply {};
    MakeDiff make_diff {};

    [[nodiscard]] bool ready() const noexcept {
        return static_cast<bool>(parse) && static_cast<bool>(apply) && static_cast<bool>(make_diff);
    }
};

inline NativeBackend deferred_backend() {
    auto unavailable = [](std::string_view operation) -> Error {
        return { .kind = ErrorKind::Unavailable,
                 .operation = std::string(operation),
                 .message = "patch_jsc native backend is DEFERRED(S-patch-jsc)" };
    };
    return {
        .parse = [unavailable](std::string_view) -> Result<std::string> {
            return std::unexpected(unavailable("parse"));
        },
        .apply = [unavailable](std::string_view, std::string_view) -> VoidResult {
            return std::unexpected(unavailable("apply"));
        },
        .make_diff = [unavailable](std::string_view, std::string_view) -> Result<std::string> {
            return std::unexpected(unavailable("makeDiff"));
        },
    };
}

} // namespace mbun::patch_jsc
