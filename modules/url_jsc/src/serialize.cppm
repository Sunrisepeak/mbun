// serialize.cppm — stable href/component serialization seam.
export module mbun.url_jsc.serialize;

import std;
import mbun.url_jsc.parse;

namespace mbun::url_jsc {

export std::string serialize(const UrlObject& url) {
    if (!url.href.empty()) return url.href;
    std::string out;
    if (!url.protocol.empty()) out += url.protocol + "://";
    out += url.host;
    out += url.pathname.empty() ? "/" : url.pathname;
    out += url.search;
    out += url.hash;
    return out;
}

export std::string serialize_origin(const UrlObject& url) { return url.origin; }

} // namespace mbun::url_jsc
