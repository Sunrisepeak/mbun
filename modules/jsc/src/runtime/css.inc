// CSS printer bridge for bun:internal-for-testing.cssInternals.
//
// The JavaScript facade keeps Bun's `minifyTest(source, expected, targets)`
// helper signature; the native CSS engine owns tokenization and serialization.

JSValueRef css_minify_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                         const JSValueRef args[], JSValueRef*) {
    JsStrArg source{ctx, argc > 0 ? args[0] : nullptr};
    return make_string(ctx, mbun::css::minify(source.view()));
}
