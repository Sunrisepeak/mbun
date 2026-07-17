export module mbun.sys_bindings.boringssl;

import std;
import mbun.sys_bindings.boringssl_sys;

namespace mbun::boringssl {

// Ref: bun-ref/src/boringssl/lib.rs and bun-zig-src/src/boringssl/boringssl.zig.
export constexpr bool is_safe_alt_name(std::string_view name, bool utf8) {
    for (const unsigned char character : name) {
        if (character == '"' || character == '\\' || character == ',' || character == '\'') {
            return false;
        }
        if (character < ' ' || character == 0x7f || (!utf8 && character > '~')) {
            return false;
        }
    }
    return true;
}

export class Initialization {
private:
    boringssl_sys::Api* api_ {};

public:
    explicit Initialization(boringssl_sys::Api* api) : api_ {api} {}
    int load() const { return api_ == nullptr ? -1 : api_->crypto_library_init(); }
};

} // namespace mbun::boringssl
