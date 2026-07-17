// zlib_stream.inc — native bridge for node:zlib streaming Transform classes.
//
// Exposes the stateful, handle-based incremental codecs (mbun.compress.stream)
// to the JS layer as extra methods on globalThis.__mbunZlibNative:
//   streamOpen(kind, windowBits, level, memLevel, strategy, quality, lgwin, mode) → handle
//   streamProcess(handle, dataB64, flush) → { b64, consumed, streamEnd, ok, message }
//   streamDict(handle, dictB64) → bool
//   streamReset(handle) → bool
//   streamClose(handle)
//
// `kind` is the StreamKind ordinal: 0 deflate, 1 inflate, 2 brotli_enc,
// 3 brotli_dec, 4 zstd_enc, 5 zstd_dec. `flush`: 0 none, 1 sync, 2 finish.
// Binary payloads cross the boundary as base64 (same convention as the one-shot
// __mbunZlibNative methods). The JS Transform classes in js_builtins own the
// stream.Transform wiring, option parsing, chunkSize slicing and error events.

// streamOpen(kind, windowBits, level, memLevel, strategy, quality, lgwin, mode)
JSValueRef zlibs_open_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                         const JSValueRef args[], JSValueRef*) {
    const int kind{zlibn_arg_int(ctx, argc, args, 0, 0)};
    const int windowBits{zlibn_arg_int(ctx, argc, args, 1, 15)};
    const int level{zlibn_arg_int(ctx, argc, args, 2, -1)};
    const int memLevel{zlibn_arg_int(ctx, argc, args, 3, 8)};
    const int strategy{zlibn_arg_int(ctx, argc, args, 4, 0)};
    const int quality{zlibn_arg_int(ctx, argc, args, 5, -1)};
    const int lgwin{zlibn_arg_int(ctx, argc, args, 6, 0)};
    const int mode{zlibn_arg_int(ctx, argc, args, 7, 0)};
    const int id{mbun::compress::stream_open(static_cast<mbun::compress::StreamKind>(kind),
                                             windowBits, level, memLevel, strategy,
                                             quality, lgwin, mode)};
    return JSValueMakeNumber(ctx, static_cast<double>(id));
}

// streamProcess(handle, dataB64, flush) → { b64, consumed, streamEnd, ok, message }
JSValueRef zlibs_process_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                            const JSValueRef args[], JSValueRef*) {
    const int handle{zlibn_arg_int(ctx, argc, args, 0, 0)};
    std::vector<std::uint8_t> data{zlib_b64_decode(arg_string(ctx, argc, args, 1))};
    const int flush{zlibn_arg_int(ctx, argc, args, 2, 0)};
    auto r{mbun::compress::stream_process(handle, mbun::compress::ByteView{data},
                                          static_cast<mbun::compress::StreamFlush>(flush))};
    JSObjectRef out{JSObjectMake(ctx, nullptr, nullptr)};
    set_prop(ctx, out, "b64", make_string(ctx, zlib_b64_encode(r.output)));
    set_prop(ctx, out, "consumed", JSValueMakeNumber(ctx, static_cast<double>(r.consumed)));
    set_prop(ctx, out, "streamEnd", JSValueMakeBoolean(ctx, r.stream_end));
    set_prop(ctx, out, "ok", JSValueMakeBoolean(ctx, r.ok));
    set_prop(ctx, out, "message", make_string(ctx, r.message));
    return out;
}

// streamDict(handle, dictB64) → bool
JSValueRef zlibs_dict_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                         const JSValueRef args[], JSValueRef*) {
    const int handle{zlibn_arg_int(ctx, argc, args, 0, 0)};
    std::vector<std::uint8_t> dict{zlib_b64_decode(arg_string(ctx, argc, args, 1))};
    return JSValueMakeBoolean(ctx, mbun::compress::stream_inflate_set_dictionary(
                                       handle, mbun::compress::ByteView{dict}));
}

// streamReset(handle) → bool
JSValueRef zlibs_reset_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                          const JSValueRef args[], JSValueRef*) {
    const int handle{zlibn_arg_int(ctx, argc, args, 0, 0)};
    return JSValueMakeBoolean(ctx, mbun::compress::stream_reset(handle));
}

// streamClose(handle)
JSValueRef zlibs_close_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                          const JSValueRef args[], JSValueRef*) {
    const int handle{zlibn_arg_int(ctx, argc, args, 0, 0)};
    mbun::compress::stream_close(handle);
    return JSValueMakeUndefined(ctx);
}

// Attach the streaming methods onto the existing __mbunZlibNative object.
void install_zlib_stream_native(JSContextRef ctx, JSObjectRef zn) {
    set_fn(ctx, zn, "streamOpen", zlibs_open_cb);
    set_fn(ctx, zn, "streamProcess", zlibs_process_cb);
    set_fn(ctx, zn, "streamDict", zlibs_dict_cb);
    set_fn(ctx, zn, "streamReset", zlibs_reset_cb);
    set_fn(ctx, zn, "streamClose", zlibs_close_cb);
}
