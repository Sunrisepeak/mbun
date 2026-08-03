// bun:internal-for-testing hostedGitInfo.{parseUrl,fromUrl} bridges.
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

// `Representation` → the string `fromUrl()` reports as `default`.
// PORT-SOURCE: compat/bun/src/install/hosted_git_info.rs:98-113 — the enum
// derives `strum::IntoStaticStr` with `serialize_all = "lowercase"`, so the JS
// name is just the variant lowercased.
std::string_view hosted_git_info_representation_name(mbun::install::hgi::Representation rep) {
    using mbun::install::hgi::Representation;
    switch (rep) {
    case Representation::Shortcut: return "shortcut";
    case Representation::Sshurl: return "sshurl";
    case Representation::Ssh: return "ssh";
    case Representation::Https: return "https";
    case Representation::Git: return "git";
    case Representation::Http: return "http";
    }
    return "";
}

// PORT-SOURCE: compat/bun/src/install_jsc/hosted_git_info_jsc.rs:10-56
// (`HostedGitInfoJsc::to_js`) and :95-131 (`js_from_url`). Same six keys, same
// null-for-absent convention, same "not a git URL → null" mapping — only the
// parse itself is mbun's (mbun.install.hosted_git_info::from_url), which is the
// point: the installer and this test surface must not diverge.
JSValueRef hosted_git_info_from_url_cb(JSContextRef ctx, JSObjectRef, JSObjectRef,
                                       std::size_t argc, const JSValueRef args[], JSValueRef* exc) {
    if (argc != 1) {
        if (exc != nullptr) {
            *exc = make_named_error(ctx, "Error",
                                    "hostedGitInfo.prototype.fromUrl takes exactly 1 argument");
        }
        return JSValueMakeUndefined(ctx);
    }
    if (!JSValueIsString(ctx, args[0])) {
        if (exc != nullptr) {
            *exc = make_named_error(
                ctx, "Error",
                "hostedGitInfo.prototype.fromUrl takes a string as its first argument");
        }
        return JSValueMakeUndefined(ctx);
    }

    auto info{mbun::install::hgi::from_url(val_to_string(ctx, args[0]))};
    if (!info) {
        return JSValueMakeNull(ctx);
    }

    JSObjectRef result{JSObjectMake(ctx, nullptr, nullptr)};
    set_prop(ctx, result, "type",
             make_string(ctx, mbun::install::hgi::host_provider_name(info->host_provider)));
    set_prop(ctx, result, "domain",
             make_string(ctx, mbun::install::hgi::host_domain(info->host_provider)));
    set_prop(ctx, result, "project", make_string(ctx, info->project));
    set_prop(ctx, result, "user",
             info->user ? make_string(ctx, *info->user) : JSValueMakeNull(ctx));
    set_prop(ctx, result, "committish",
             info->committish ? make_string(ctx, *info->committish) : JSValueMakeNull(ctx));
    set_prop(ctx, result, "default",
             make_string(ctx, hosted_git_info_representation_name(info->default_representation)));
    return result;
}
