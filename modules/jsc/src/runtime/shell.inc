// Bun `$` native bridge: tagged-template values are converted to the pure
// mbun.shell compiler's TemplateArgument model. The JS-facing ShellPromise and
// process lifetime live in the dedicated shell builtin partition.

// Mirrors bun's `ShellErr::throw_js` (src/runtime/shell/shell_body.rs:97-101): the
// `Custom` arm funnels the message through `to_error_instance`, so `$` rejects with a
// real Error whose `.message` is the shell diagnostic. `fs_throw` raises a bare JS
// string instead, which leaves `err.message` undefined for every `$` caller.
JSValueRef shell_throw(JSContextRef context, JSValueRef* exception, const std::string& message,
                       const char* code = nullptr) {
    if (exception != nullptr) {
        JSValueRef messageValue{make_string(context, message)};
        JSObjectRef errorObject{JSObjectMakeError(context, 1, &messageValue, nullptr)};
        if (code != nullptr) set_prop(context, errorObject, "code", make_string(context, code));
        *exception = errorObject;
    }
    return JSValueMakeUndefined(context);
}

std::optional<std::size_t> shell_array_length(JSContextRef context, JSValueRef value,
                                              JSValueRef* exception) {
    if (!JSValueIsArray(context, value)) return std::nullopt;
    JSObjectRef array{JSValueToObject(context, value, exception)};
    if (array == nullptr || (exception != nullptr && *exception != nullptr)) return std::nullopt;
    JSStringRef lengthKey{JSStringCreateWithUTF8CString("length")};
    JSValueRef lengthValue{JSObjectGetProperty(context, array, lengthKey, exception)};
    JSStringRelease(lengthKey);
    if (exception != nullptr && *exception != nullptr) return std::nullopt;
    const double length{JSValueToNumber(context, lengthValue, exception)};
    if (exception != nullptr && *exception != nullptr) return std::nullopt;
    if (!std::isfinite(length) || length < 0 || length > 1'000'000) return std::nullopt;
    return static_cast<std::size_t>(length);
}

bool shell_value_to_string(JSContextRef context, JSValueRef value, std::string& output,
                           JSValueRef* exception) {
    JSStringRef string{JSValueToStringCopy(context, value, exception)};
    if (string == nullptr || (exception != nullptr && *exception != nullptr)) return false;
    output = to_utf8(string);
    JSStringRelease(string);
    return true;
}

bool shell_collect_words(JSContextRef context, JSValueRef value, std::uint32_t depth,
                         std::vector<std::string>& output, JSValueRef* exception) {
    if (const auto length{shell_array_length(context, value, exception)}) {
        if (depth >= mbun::shell::MAX_TEMPLATE_ARRAY_DEPTH) {
            shell_throw(context, exception,
                        "Shell script template arrays cannot be nested more than 100 levels deep");
            return false;
        }
        JSObjectRef array{JSValueToObject(context, value, exception)};
        if (array == nullptr || (exception != nullptr && *exception != nullptr)) return false;
        for (std::size_t index{0}; index < *length; ++index) {
            JSValueRef element{JSObjectGetPropertyAtIndex(
                context, array, static_cast<unsigned>(index), exception)};
            if (exception != nullptr && *exception != nullptr) return false;
            if (!shell_collect_words(context, element, depth + 1, output, exception)) return false;
        }
        return true;
    }
    if (exception != nullptr && *exception != nullptr) return false;

    // bun (shell_body.rs:885-905): an object with a truthy `raw` property
    // stringifies that property; compile_template's null-byte guard then applies
    // (so `${ {raw:"a\0b"} }` throws "must be a string without null bytes").
    if (JSValueIsObject(context, value) && !JSValueIsArray(context, value)) {
        JSObjectRef obj{JSValueToObject(context, value, exception)};
        if (exception != nullptr && *exception != nullptr) return false;
        // bun (shell_body.rs:785-805): a file-backed Blob — `Bun.file(path)` —
        // interpolates as its PATH, not as its contents or "[object Blob]".
        // mbun's Bun.file wrapper tags the Blob with __isBunFile/__name.
        JSStringRef bunFileKey{JSStringCreateWithUTF8CString("__isBunFile")};
        JSValueRef bunFileVal{JSObjectGetProperty(context, obj, bunFileKey, exception)};
        JSStringRelease(bunFileKey);
        if (exception != nullptr && *exception != nullptr) return false;
        if (bunFileVal != nullptr && JSValueToBoolean(context, bunFileVal)) {
            JSStringRef nameKey{JSStringCreateWithUTF8CString("__name")};
            JSValueRef nameVal{JSObjectGetProperty(context, obj, nameKey, exception)};
            JSStringRelease(nameKey);
            if (exception != nullptr && *exception != nullptr) return false;
            if (nameVal != nullptr && !JSValueIsUndefined(context, nameVal)
                && !JSValueIsNull(context, nameVal)) {
                std::string path;
                if (!shell_value_to_string(context, nameVal, path, exception)) return false;
                output.push_back(std::move(path));
                return true;
            }
        }
        JSStringRef rawKey{JSStringCreateWithUTF8CString("raw")};
        JSValueRef rawVal{JSObjectGetProperty(context, obj, rawKey, exception)};
        JSStringRelease(rawKey);
        if (exception != nullptr && *exception != nullptr) return false;
        if (rawVal != nullptr && JSValueToBoolean(context, rawVal)) {
            std::string raw;
            if (!shell_value_to_string(context, rawVal, raw, exception)) return false;
            output.push_back(std::move(raw));
            return true;
        }
    }

    std::string word;
    if (!shell_value_to_string(context, value, word, exception)) return false;
    output.push_back(std::move(word));
    return true;
}

// Is `value` bun's raw escape hatch, `{ raw: "..." }`, as a WHOLE interpolation?
// On true, `out` holds the string to splice into the shell source verbatim.
// Arrays and file-backed Blobs are not raw (a Blob interpolates as its path).
bool shell_whole_raw_text(JSContextRef context, JSValueRef value, std::string& out,
                          JSValueRef* exception) {
    if (!JSValueIsObject(context, value) || JSValueIsArray(context, value)) return false;
    JSObjectRef obj{JSValueToObject(context, value, exception)};
    if (obj == nullptr || (exception != nullptr && *exception != nullptr)) return false;
    JSStringRef rawKey{JSStringCreateWithUTF8CString("raw")};
    JSValueRef rawVal{JSObjectGetProperty(context, obj, rawKey, exception)};
    JSStringRelease(rawKey);
    if (exception != nullptr && *exception != nullptr) return false;
    if (rawVal == nullptr || !JSValueToBoolean(context, rawVal)) return false;
    return shell_value_to_string(context, rawVal, out, exception);
}

// __mbunShellNative.escape(str) — backs Bun.$.escape. ref: BunObject.rs
// shell_escape: a string that needs no escaping is returned unchanged (bun
// hands back the very same JSValue), everything else comes back double-quoted.
JSValueRef shell_escape_cb(JSContextRef context, JSObjectRef, JSObjectRef, std::size_t argc,
                           const JSValueRef arguments[], JSValueRef* exception) {
    if (argc < 1) {
        return shell_throw(context, exception, "shell escape expected at least 1 argument");
    }
    std::string text;
    if (!shell_value_to_string(context, arguments[0], text, exception)) {
        return JSValueMakeUndefined(context);
    }
    if (!mbun::shell::needs_escape(text)) return arguments[0];
    return make_string(context, mbun::shell::escape_string(text));
}

JSValueRef shell_compile_cb(JSContextRef context, JSObjectRef, JSObjectRef, std::size_t argc,
                            const JSValueRef arguments[], JSValueRef* exception) {
    if (argc < 2) {
        return shell_throw(context, exception, "Bun.$ requires template strings and values");
    }
    const auto rawLength{shell_array_length(context, arguments[0], exception)};
    const auto valueLength{shell_array_length(context, arguments[1], exception)};
    if (!rawLength || !valueLength) {
        if (exception != nullptr && *exception != nullptr) return JSValueMakeUndefined(context);
        return shell_throw(context, exception, "Bun.$ expects array template inputs");
    }

    JSObjectRef rawArray{JSValueToObject(context, arguments[0], exception)};
    JSObjectRef valueArray{JSValueToObject(context, arguments[1], exception)};
    if (rawArray == nullptr || valueArray == nullptr ||
        (exception != nullptr && *exception != nullptr)) {
        return JSValueMakeUndefined(context);
    }

    std::vector<std::string> rawSegments;
    rawSegments.reserve(*rawLength);
    for (std::size_t index{0}; index < *rawLength; ++index) {
        JSValueRef value{JSObjectGetPropertyAtIndex(
            context, rawArray, static_cast<unsigned>(index), exception)};
        if (exception != nullptr && *exception != nullptr) return JSValueMakeUndefined(context);
        std::string raw;
        if (!shell_value_to_string(context, value, raw, exception)) {
            return JSValueMakeUndefined(context);
        }
        rawSegments.push_back(std::move(raw));
    }

    std::vector<mbun::shell::TemplateArgument> templateArguments;
    templateArguments.reserve(*valueLength);
    std::uint32_t jsObjCount{0};
    for (std::size_t index{0}; index < *valueLength; ++index) {
        JSValueRef value{JSObjectGetPropertyAtIndex(
            context, valueArray, static_cast<unsigned>(index), exception)};
        if (exception != nullptr && *exception != nullptr) return JSValueMakeUndefined(context);
        mbun::shell::TemplateArgument argument;
        // bun tests array-buffer-backed values first and never stringifies them: they
        // become out_jsobjs entries referenced by \x08__bun_N (shell_body.rs:766-777).
        // Stringifying a Uint8Array instead turns `${buf}` into the word "0,0,0,..." and
        // runs it as a command. Blob/ReadableStream/Response are refs in bun too
        // (shell_body.rs:807-840); those class checks are not wired up here yet.
        if (JSValueGetTypedArrayType(context, value, nullptr) != kJSTypedArrayTypeNone) {
            argument.jsObjIndex = jsObjCount++;
        } else if (std::string rawText; shell_whole_raw_text(context, value, rawText, exception)) {
            // bun shell_body.rs:885-905 — a WHOLE `${{ raw: "..." }}` interpolation
            // is spliced into the shell source unescaped. shell_collect_words'
            // raw branch stays for a raw object nested inside an interpolated
            // array, which bun still turns into a single word.
            argument.rawText = std::move(rawText);
        } else if (exception != nullptr && *exception != nullptr) {
            return JSValueMakeUndefined(context);
        } else if (!shell_collect_words(context, value, 0, argument.words, exception)) {
            return JSValueMakeUndefined(context);
        }
        templateArguments.push_back(std::move(argument));
    }

    constexpr auto platform{
#if defined(_WIN32)
        mbun::shell::TemplatePlatform::Windows
#else
        mbun::shell::TemplatePlatform::Posix
#endif
    };
    auto compiled{mbun::shell::compile_template(rawSegments, templateArguments, platform)};
    if (!compiled)
        return shell_throw(context, exception, compiled.error().message,
                           compiled.error().code == mbun::shell::TemplateErrorCode::NullByte
                               ? "ERR_INVALID_ARG_VALUE" : nullptr);
    return make_string(context, *compiled);
}
