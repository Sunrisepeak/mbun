// binding.cppm — JSC-independent URL object binding seam.
//
// Rust keeps URL__fromJS/hrefFromJS in bun_jsc; Zig exposes urlFromJS and maps
// a Dead href to error.InvalidURL. HrefFromJs is the equivalent narrow seam:
// a real JavaScriptCore adapter can be supplied without changing parse/tests.
export module mbun.url_jsc.binding;

import std;
import mbun.url_jsc.error;
import mbun.url_jsc.parse;
import mbun.url_jsc.serialize;

namespace mbun::url_jsc {

export using HrefFromJs = std::function<std::expected<std::string, Error>(std::string_view)>;

export class UrlBinding {
public:
    static std::expected<UrlObject, Error> from_js(std::string_view value, const HrefFromJs& href_from_js) {
        if (!href_from_js) return std::unexpected{Error::JsValueRejected};
        auto href{href_from_js(value)};
        if (!href) return std::unexpected{href.error()};
        return parse(*href);
    }

    static std::expected<std::string, Error> href_from_js(std::string_view value, const HrefFromJs& href_from_js) {
        auto object{from_js(value, href_from_js)};
        if (!object) return std::unexpected{object.error()};
        return serialize(*object);
    }
};

export inline HrefFromJs string_href_adapter() {
    return [](std::string_view value) -> std::expected<std::string, Error> {
        if (value.empty()) return std::unexpected{Error::EmptyInput};
        return std::string(value);
    };
}

} // namespace mbun::url_jsc
