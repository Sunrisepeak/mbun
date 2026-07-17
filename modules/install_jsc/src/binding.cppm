// Aggregated JSC binding registration shape. The actual JSValue/JSGlobalObject
// host functions are intentionally deferred until the JSC runtime seam lands.
// ref: bun-ref/src/install_jsc/install_binding.rs and
//      bun-zig-src/src/install_jsc/install_binding.zig.
export module mbun.install_jsc.binding;

import std;
import mbun.install_jsc.lifecycle;
import mbun.install_jsc.native_backend;
import mbun.install_jsc.package_manager;

export namespace mbun::install_jsc {

enum class BindingKind : std::uint8_t { ParseLockfile, ParseUpdateRequest, RunLifecycle };

struct BindingDescriptor {
    BindingKind kind;
    std::string_view name;
    std::uint8_t arity;
};

class BindingRegistry {
private:
    std::array<BindingDescriptor, 3> descriptors_ {{
        { BindingKind::ParseLockfile, "parseLockfile", 1 },
        { BindingKind::ParseUpdateRequest, "parseUpdateRequest", 1 },
        { BindingKind::RunLifecycle, "runLifecycle", 3 },
    }};

public:
    [[nodiscard]] constexpr std::span<const BindingDescriptor> descriptors() const noexcept {
        return descriptors_;
    }

    [[nodiscard]] std::expected<std::string, BackendError> parse_lockfile(
        std::string_view cwd, const NativeBackend& backend) const {
        if (!backend.parse_lockfile) {
            return std::unexpected(BackendError {
                .kind = ErrorKind::Unavailable,
                .operation = "parseLockfile",
                .message = "lockfile parser is not installed",
            });
        }
        return backend.parse_lockfile(cwd);
    }
};

}  // namespace mbun::install_jsc
