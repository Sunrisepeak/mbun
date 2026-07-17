import std;
import mbun.runtime_ffi;

namespace {
int checks{};
int failures{};

int add(int left, int right) { return left + right; }

void check(bool condition, std::string_view message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::println("FAIL: {}", message);
    }
}

void test_error_values() {
    auto error{mbun::runtime_ffi::Error::missing_library("missing")};
    check(error.kind == mbun::runtime_ffi::ErrorKind::missing_library, "library error kind");
    check(error.subject == "missing", "library error subject");
}

void test_injected_backend_call() {
    mbun::runtime_ffi::Backend backend{
        .load = [](std::string_view name) -> std::expected<void*, mbun::runtime_ffi::Error> {
            if (name != "fixture") {
                return std::unexpected(mbun::runtime_ffi::Error::missing_library(name));
            }
            return reinterpret_cast<void*>(0x1);
        },
        .lookup = [](void* handle, std::string_view name)
            -> std::expected<void*, mbun::runtime_ffi::Error> {
            if (handle != reinterpret_cast<void*>(0x1) || name != "add") {
                return std::unexpected(mbun::runtime_ffi::Error::missing_symbol(name));
            }
            return reinterpret_cast<void*>(&add);
        },
        .close = [](void*) {},
    };
    auto library{mbun::runtime_ffi::Library::open("fixture", backend)};
    check(library.has_value(), "injected library loads");
    auto symbol{library->symbol("add")};
    check(symbol.has_value() && symbol->address() == reinterpret_cast<void*>(&add),
          "injected symbol resolves");
    auto result{mbun::runtime_ffi::call<int>(symbol->address(), 2, 3)};
    check(result.has_value() && *result == 5, "typed call seam invokes function");
}

void test_missing_library_and_symbol() {
    mbun::runtime_ffi::Backend backend{
        .load = [](std::string_view name) -> std::expected<void*, mbun::runtime_ffi::Error> {
            return std::unexpected(mbun::runtime_ffi::Error::missing_library(name));
        },
    };
    auto library{mbun::runtime_ffi::Library::open("nope", backend)};
    check(!library && library.error().kind == mbun::runtime_ffi::ErrorKind::missing_library,
          "missing library propagates");
}
}

int main() {
    test_error_values();
    test_injected_backend_call();
    test_missing_library_and_symbol();
    std::println("test_runtime_ffi: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}
