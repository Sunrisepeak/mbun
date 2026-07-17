import std;
import mbun.jsc_macros;

namespace {
int checks {};
int failures {};

void check(bool condition, std::string_view message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::println("FAIL: {}", message);
    }
}
}

int main() {
    using namespace mbun::jsc_macros;

    constexpr auto method {host_fn_descriptor(
        "load",
        "load",
        "WidgetPrototype__load",
        HostFunctionKind::Method,
        true)};
    constexpr auto methodWrapper {generate_host_wrapper(method)};
    check(methodWrapper.kind == WrapperKind::HostFunction, "host wrapper kind");
    check(methodWrapper.uses_receiver && methodWrapper.uses_global,
          "method receiver/global contract");
    check(methodWrapper.target_symbol == "WidgetPrototype__load",
          "C++ symbol preserved");

    constexpr auto cached {cached_property_descriptor(
        "Widget",
        "idleTimeout",
        "WidgetPrototype__idleTimeoutGetCachedValue",
        "WidgetPrototype__idleTimeoutSetCachedValue")};
    constexpr auto getter {generate_cached_getter(cached)};
    constexpr auto setter {generate_cached_setter(cached)};
    check(getter.target_symbol.ends_with("GetCachedValue"), "cached getter symbol");
    check(setter.target_symbol.ends_with("SetCachedValue") && setter.uses_global,
          "cached setter write-barrier contract");

    constexpr auto klass {class_binding_descriptor(
        "Widget",
        "Widget",
        "Widget__fromJS",
        "Widget__create",
        "Widget__getConstructor",
        "WidgetClass__finalize",
        false,
        false,
        true,
        true)};
    constexpr auto hooks {generate_class_hooks(klass)};
    check(hooks.size() == 4 && hooks[0].wrapper_name == "from_js",
          "class hook descriptors");
    check(hooks[0].target_symbol == "Widget__fromJS" &&
              hooks[3].target_symbol == "WidgetClass__finalize",
          "class canonical symbols preserved");
    check(!klass.has_constructor && klass.has_estimated_size,
          "class option boundary");

    std::println("jsc_macros checks: {}, failures: {}", checks, failures);
    return failures == 0 ? 0 : 1;
}
