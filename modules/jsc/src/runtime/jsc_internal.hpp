#pragma once

// JSC's C++ headers contain TU-local templates. Keep these helpers in the one
// compiled runtime module unit; exporting them from a partition would require
// non-portable permissive flags on GCC.
class JsStrArg {
public:
    JsStrArg() = default;

    JsStrArg(JSContextRef ctx, JSValueRef value) {
        if (value == nullptr) return;
        if (JSValueIsString(ctx, value)) {
            JSC::JSGlobalObject* global{toJS(ctx)};
            init_(global, JSC::asString(toJS(global, value)));
            return;
        }
        owned_ = val_to_string(ctx, value);
        view_ = owned_;
    }

    JsStrArg(JSC::JSGlobalObject* global, JSC::JSValue value) {
        if (!value) return;
        init_(global, value.isString() ? JSC::asString(value) : value.toString(global));
    }

    JsStrArg(const JsStrArg&) = delete;
    JsStrArg& operator=(const JsStrArg&) = delete;
    JsStrArg(JsStrArg&&) = delete;
    JsStrArg& operator=(JsStrArg&&) = delete;

    std::string_view view() const { return view_; }

private:
    void init_(JSC::JSGlobalObject* global, JSC::JSString* string) {
        scope_ = string->view(global);
        WTF::StringView stringView{scope_};
        if (stringView.is8Bit()) {
            const std::span<const unsigned char> bytes{stringView.span8()};
            if (is_ascii_(bytes)) {
                view_ = {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
                return;
            }
            transcode_latin1_(bytes);
        } else {
            transcode_utf16_(stringView.span16());
        }
        view_ = owned_;
    }

    static bool is_ascii_(std::span<const unsigned char> bytes) {
        for (const unsigned char byte : bytes)
            if (byte & 0x80U) return false;
        return true;
    }

    void transcode_latin1_(std::span<const unsigned char> bytes) {
        owned_.clear();
        owned_.reserve(bytes.size() + bytes.size() / 2);
        for (const unsigned char byte : bytes) {
            if (byte < 0x80U) {
                owned_.push_back(static_cast<char>(byte));
            } else {
                owned_.push_back(static_cast<char>(0xC0U | (byte >> 6)));
                owned_.push_back(static_cast<char>(0x80U | (byte & 0x3FU)));
            }
        }
    }

    void transcode_utf16_(std::span<const char16_t> codeUnits) {
        owned_.clear();
        owned_.reserve(codeUnits.size() + codeUnits.size() / 2);
        for (std::size_t index{0}; index < codeUnits.size(); ++index) {
            char32_t codePoint{codeUnits[index]};
            if (codePoint >= 0xD800 && codePoint <= 0xDBFF && index + 1 < codeUnits.size()
                && codeUnits[index + 1] >= 0xDC00 && codeUnits[index + 1] <= 0xDFFF) {
                codePoint = 0x10000 + ((codePoint - 0xD800) << 10)
                            + (codeUnits[++index] - 0xDC00);
            }
            if (codePoint < 0x80) {
                owned_.push_back(static_cast<char>(codePoint));
            } else if (codePoint < 0x800) {
                owned_.push_back(static_cast<char>(0xC0U | (codePoint >> 6)));
                owned_.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
            } else if (codePoint < 0x10000) {
                owned_.push_back(static_cast<char>(0xE0U | (codePoint >> 12)));
                owned_.push_back(static_cast<char>(0x80U | ((codePoint >> 6) & 0x3FU)));
                owned_.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
            } else {
                owned_.push_back(static_cast<char>(0xF0U | (codePoint >> 18)));
                owned_.push_back(static_cast<char>(0x80U | ((codePoint >> 12) & 0x3FU)));
                owned_.push_back(static_cast<char>(0x80U | ((codePoint >> 6) & 0x3FU)));
                owned_.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
            }
        }
    }

    JSC::GCOwnedDataScope<WTF::StringView> scope_{};
    std::string owned_{};
    std::string_view view_{};
};

inline JSC::JSValue native_arg(JSC::CallFrame* frame, unsigned index) {
    return index < frame->argumentCount() ? frame->argument(index) : JSC::JSValue{};
}

// Throw a plain `Error` (not a TypeError) with `msg` and return the empty value,
// for host functions whose bun counterpart uses `global.throw(format_args!(..))`
// — that produces a bare Error with no `code`, which JSC::throwTypeError would
// not match. JSObjectMakeError is the C API constructor for exactly that shape.
inline JSC::EncodedJSValue throw_plain_error(JSC::JSGlobalObject* global, std::string_view msg) {
    JSC::VM& vm{global->vm()};
    auto scope{DECLARE_THROW_SCOPE(vm)};
    JSContextRef ctx{toRef(global)};
    JSValueRef message{make_string(ctx, msg)};
    JSValueRef error{JSObjectMakeError(ctx, 1, &message, nullptr)};
    scope.throwException(global, toJS(global, error));
    return JSC::encodedJSValue();
}

inline void set_native_fn(JSContextRef ctx, JSObjectRef object, const char* name,
                          unsigned length, JSC::NativeFunction function) {
    JSC::JSGlobalObject* global{toJS(ctx)};
    JSC::JSFunction* jsFunction{JSC::JSFunction::create(
        global->vm(), global, length, WTF::String::fromLatin1(name), function,
        JSC::ImplementationVisibility::Public)};
    JSStringRef property{JSStringCreateWithUTF8CString(name)};
    JSObjectSetProperty(ctx, object, property, toRef(jsFunction), kJSPropertyAttributeNone, nullptr);
    JSStringRelease(property);
}
