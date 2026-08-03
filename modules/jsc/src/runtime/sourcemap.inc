// bun:internal-for-testing internalSourceMap JSC bridge.
// ref: bun src/sourcemap_jsc/internal_jsc.rs and internal_jsc.zig.

JSValueRef source_map_error(JSContextRef ctx, JSValueRef* exception, std::string_view message) {
    if (exception != nullptr) {
        JSValueRef text{make_string(ctx, message)};
        *exception = JSObjectMakeError(ctx, 1, &text, nullptr);
    }
    return JSValueMakeUndefined(ctx);
}

std::optional<std::span<const std::uint8_t>> source_map_bytes(JSContextRef ctx, JSValueRef value) {
    if (value == nullptr || !JSValueIsObject(ctx, value))
        return std::nullopt;
    JSValueRef exception{nullptr};
    if (JSValueGetTypedArrayType(ctx, value, &exception) != kJSTypedArrayTypeUint8Array ||
        exception != nullptr) {
        return std::nullopt;
    }
    JSObjectRef object{JSValueToObject(ctx, value, &exception)};
    if (object == nullptr || exception != nullptr)
        return std::nullopt;
    // BytesPtr answers the BUFFER base, ByteLength is view-relative: without the
    // offset a subarray is read from the wrong bytes.
    void* data{JSObjectGetTypedArrayBytesPtr(ctx, object, &exception)};
    if (data != nullptr)
        data = static_cast<char*>(data) + JSObjectGetTypedArrayByteOffset(ctx, object, &exception);
    const std::size_t size{JSObjectGetTypedArrayByteLength(ctx, object, &exception)};
    if (exception != nullptr || (data == nullptr && size != 0))
        return std::nullopt;
    return std::span<const std::uint8_t>{static_cast<const std::uint8_t*>(data), size};
}

JSValueRef source_map_from_vlq_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                  const JSValueRef args[], JSValueRef* exception) {
    JsStrArg input{ctx, argc > 0 ? args[0] : nullptr};
    auto blob{mbun::sourcemap_jsc::internal_map_from_vlq(input.view())};
    if (!blob) {
        return source_map_error(ctx, exception, "InternalSourceMap.fromVLQ: invalid VLQ input");
    }
    JSObjectRef array{
        JSObjectMakeTypedArray(ctx, kJSTypedArrayTypeUint8Array, blob->size(), exception)};
    if (array == nullptr || (exception != nullptr && *exception != nullptr)) {
        return JSValueMakeUndefined(ctx);
    }
    if (!blob->empty()) {
        void* destination{JSObjectGetTypedArrayBytesPtr(ctx, array, exception)};
        if (destination == nullptr)
            return JSValueMakeUndefined(ctx);
        std::copy(blob->begin(), blob->end(), static_cast<std::uint8_t*>(destination));
    }
    return array;
}

JSValueRef source_map_to_vlq_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                const JSValueRef args[], JSValueRef* exception) {
    auto bytes{source_map_bytes(ctx, argc > 0 ? args[0] : nullptr)};
    if (!bytes) {
        return source_map_error(ctx, exception, "InternalSourceMap.toVLQ: expected Uint8Array");
    }
    auto vlq{mbun::sourcemap_jsc::internal_map_to_vlq(*bytes)};
    if (!vlq)
        return source_map_error(ctx, exception, "InternalSourceMap.toVLQ: invalid blob");
    return make_string(ctx, *vlq);
}

JSValueRef source_map_find_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                              const JSValueRef args[], JSValueRef* exception) {
    auto bytes{source_map_bytes(ctx, argc > 0 ? args[0] : nullptr)};
    if (!bytes) {
        return source_map_error(ctx, exception, "InternalSourceMap.find: expected Uint8Array");
    }
    JSValueRef conversionException{nullptr};
    auto coordinate = [&](std::size_t index) -> std::optional<std::int32_t> {
        if (conversionException != nullptr)
            return std::nullopt;
        if (argc <= index)
            return 0;
        const double number{JSValueToNumber(ctx, args[index], &conversionException)};
        if (conversionException != nullptr || !std::isfinite(number) ||
            std::trunc(number) != number || number < std::numeric_limits<std::int32_t>::min() ||
            number > std::numeric_limits<std::int32_t>::max()) {
            return std::nullopt;
        }
        return static_cast<std::int32_t>(number);
    };
    const auto line{coordinate(1)};
    const auto column{coordinate(2)};
    if (conversionException != nullptr) {
        if (exception != nullptr)
            *exception = conversionException;
        return JSValueMakeUndefined(ctx);
    }
    if (!line || !column) {
        return source_map_error(ctx, exception,
                                "InternalSourceMap.find: expected int32 line and column");
    }
    auto found{mbun::sourcemap_jsc::internal_map_find(*bytes, *line, *column)};
    if (!found)
        return source_map_error(ctx, exception, "InternalSourceMap.find: invalid blob");
    if (!*found)
        return JSValueMakeNull(ctx);

    JSObjectRef object{JSObjectMake(ctx, nullptr, nullptr)};
    set_prop(ctx, object, "generatedLine", JSValueMakeNumber(ctx, (*found)->generatedLine));
    set_prop(ctx, object, "generatedColumn", JSValueMakeNumber(ctx, (*found)->generatedColumn));
    set_prop(ctx, object, "originalLine", JSValueMakeNumber(ctx, (*found)->originalLine));
    set_prop(ctx, object, "originalColumn", JSValueMakeNumber(ctx, (*found)->originalColumn));
    set_prop(ctx, object, "sourceIndex", JSValueMakeNumber(ctx, (*found)->sourceIndex));
    return object;
}
