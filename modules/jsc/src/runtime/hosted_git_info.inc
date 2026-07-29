// bun:internal-for-testing hostedGitInfo.parseUrl bridge.
//
// Keep hosted-git URL normalization in mbun.install.hosted_git_info: the test
// surface must exercise the same parser the installer uses, rather than a
// second JS approximation with subtly different scp-style URL handling.

JSValueRef hosted_git_info_parse_url_cb(JSContextRef ctx, JSObjectRef, JSObjectRef,
                                        std::size_t argc, const JSValueRef args[], JSValueRef*) {
    auto parsed{mbun::install::hgi::parse_url(arg_string(ctx, argc, args, 0))};
    if (!parsed) {
        return JSValueMakeNull(ctx);
    }

    JSObjectRef result{JSObjectMake(ctx, nullptr, nullptr)};
    set_prop(ctx, result, "protocol", make_string(ctx, parsed->url.protocol()));
    set_prop(ctx, result, "hostname", make_string(ctx, parsed->url.hostname()));
    set_prop(ctx, result, "pathname", make_string(ctx, parsed->url.pathname()));
    set_prop(ctx, result, "hash", make_string(ctx, parsed->url.fragment_identifier()));
    return result;
}
