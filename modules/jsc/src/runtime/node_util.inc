// node_util.inc — native bridge for the node:util completion partition.
//
// Only what node:util needs from a native subsystem lives here; the JS shaping
// (validators, error codes, util.types brand checks) is in
// builtins/node_util_extra.cppm.

// __mbunDotenvNative.parse(content) — backs node's util.parseEnv.
// ref: compat/bun/src/runtime/node/node_util_binding.rs parse_env, which runs
// the same env loader with OVERRIDE=true / IS_PROCESS=false and copies the
// resulting map into a plain object. Node validates `content` as a string, and
// a String *object* is accepted (the corpus pins exactly that case:
// util.parseEnv(new String("FOO=bar"))).
// Argument validation (including the String-object case) stays on the JS side
// in the node:util partition, where node's ERR_INVALID_ARG_TYPE shape lives.
JSValueRef dotenv_parse_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                           const JSValueRef args[], JSValueRef*) {
    JsStrArg src{ctx, argc > 0 ? args[0] : nullptr};
    mbun::dotenv::Map map;
    mbun::dotenv::parse_into(map, src.view(),
                             mbun::dotenv::ParseOptions{.override_existing = true,
                                                        .is_process = false,
                                                        .expand = true});
    JSObjectRef out{JSObjectMake(ctx, nullptr, nullptr)};
    for (std::size_t i{0}; i < map.count(); ++i) {
        set_prop(ctx, out, std::string{map.key_at(i)}.c_str(),
                 make_string(ctx, std::string{map.value_at(i)}));
    }
    return out;
}
