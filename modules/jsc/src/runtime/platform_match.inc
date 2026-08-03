// platform_match.inc — bun:internal-for-testing isArchitectureMatch /
// isOperatingSystemMatch bridge.
//
// PORT-SOURCE: compat/bun/src/js/internal-for-testing.ts:177-188 declares both
// as `$newRustFunction("npm.rs", "Architecture.jsFunctionArchitectureIsMatch")`
// / `"OperatingSystem.jsFunctionOperatingSystemIsMatch"`, which run the package
// manager's OWN `Negatable<T>::apply/combine` then `is_match(CURRENT)`
// (compat/bun/src/install_types/resolver_hooks.rs:588-760, :810-990).
//
// So this bridge is deliberately thin: the allow/block-list algebra already
// lives in C++ as mbun.install.npm.negatable, and `bun install`'s own os/cpu
// gate (modules/install/src/registry_install.cppm:841 is_disabled) goes through
// exactly these functions. Re-deriving the rules in JS would let the test
// surface drift away from the installer it is supposed to be testing — the same
// reasoning that keeps hostedGitInfo.parseUrl on the installer's parser.

// Read a JS string array (npm's `cpu` / `os` field shape) into owned strings.
// A bare string is accepted the way npm's own `"os": "linux"` spelling is.
std::vector<std::string> platform_token_list(JSContextRef ctx, std::size_t argc,
                                             const JSValueRef args[], std::size_t i) {
    std::vector<std::string> tokens;
    if (argc <= i) {
        return tokens;
    }
    if (JSValueIsArray(ctx, args[i])) {
        JSObjectRef arr{JSValueToObject(ctx, args[i], nullptr)};
        JSStringRef lenKey{JSStringCreateWithUTF8CString("length")};
        double len{JSValueToNumber(ctx, JSObjectGetProperty(ctx, arr, lenKey, nullptr), nullptr)};
        JSStringRelease(lenKey);
        if (!(len > 0)) {
            return tokens;
        }
        for (unsigned idx = 0; idx < static_cast<unsigned>(len); ++idx) {
            tokens.push_back(val_to_string(ctx, JSObjectGetPropertyAtIndex(ctx, arr, idx, nullptr)));
        }
        return tokens;
    }
    if (JSValueIsString(ctx, args[i])) {
        tokens.push_back(val_to_string(ctx, args[i]));
    }
    return tokens;
}

template <class T>
bool platform_tokens_match(JSContextRef ctx, std::size_t argc, const JSValueRef args[], T current) {
    const std::vector<std::string> owned{platform_token_list(ctx, argc, args, 0)};
    std::vector<std::string_view> views;
    views.reserve(owned.size());
    for (const std::string& tok : owned) {
        views.emplace_back(tok);
    }
    return mbun::install::npm::is_match(
        mbun::install::npm::negatable_from_tokens<T>(views), current);
}

JSValueRef is_architecture_match_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                    const JSValueRef args[], JSValueRef*) {
    return JSValueMakeBoolean(
        ctx, platform_tokens_match<mbun::install::npm::Architecture>(
                 ctx, argc, args, mbun::install::npm::CURRENT_ARCH));
}

JSValueRef is_operating_system_match_cb(JSContextRef ctx, JSObjectRef, JSObjectRef,
                                        std::size_t argc, const JSValueRef args[], JSValueRef*) {
    return JSValueMakeBoolean(
        ctx, platform_tokens_match<mbun::install::npm::OperatingSystem>(
                 ctx, argc, args, mbun::install::npm::CURRENT_OS));
}
