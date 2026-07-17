// Host binding descriptors and pure-parser bridge. Actual JSValue/JSGlobalObject
// and FD adapters remain behind NativeBackend, matching Bun's testing APIs.
// ref: bun-ref/src/patch_jsc/testing.rs; bun-zig-src/src/patch_jsc/testing.zig.
export module mbun.patch_jsc.binding;

import std;
import mbun.patch;
import mbun.patch_jsc.backend;
import mbun.patch_jsc.error;
import mbun.patch_jsc.json;

export namespace mbun::patch_jsc {

enum class BindingKind : std::uint8_t { Parse, Apply, MakeDiff };

struct BindingDescriptor {
    BindingKind kind;
    std::string_view name;
    std::uint8_t arity;
};

class Binding {
private:
    std::array<BindingDescriptor, 3> descriptors_ {{
        { BindingKind::Parse, "parse", 1 },
        { BindingKind::Apply, "apply", 2 },
        { BindingKind::MakeDiff, "makeDiff", 2 },
    }};

public:
    [[nodiscard]] constexpr std::span<const BindingDescriptor> descriptors() const noexcept {
        return descriptors_;
    }

    [[nodiscard]] Result<std::string> parse(std::string_view source) const {
        auto parsed { mbun::patch::parse_patch_file(source) };
        if (!parsed) {
            return std::unexpected(Error {
                .kind = ErrorKind::Parse,
                .operation = "parse",
                .message = std::string(mbun::patch::parse_err_name(parsed.error())),
            });
        }
        return serialize_patch_file(*parsed);
    }

    [[nodiscard]] Result<bool> apply(std::string_view source, std::string_view directory,
                                     const NativeBackend& backend) const {
        if (!backend.apply) {
            return std::unexpected(Error {
                .kind = ErrorKind::Unavailable,
                .operation = "apply",
                .message = "apply backend is not installed",
            });
        }
        auto applied { backend.apply(source, directory) };
        if (!applied) {
            return std::unexpected(std::move(applied.error()));
        }
        return true;
    }

    [[nodiscard]] Result<std::string> make_diff(std::string_view oldDirectory,
                                                 std::string_view newDirectory,
                                                 const NativeBackend& backend) const {
        if (!backend.make_diff) {
            return std::unexpected(Error {
                .kind = ErrorKind::Unavailable,
                .operation = "makeDiff",
                .message = "diff backend is not installed",
            });
        }
        return backend.make_diff(oldDirectory, newDirectory);
    }
};

} // namespace mbun::patch_jsc
