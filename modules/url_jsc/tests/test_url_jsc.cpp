#include <cassert>

import std;
import mbun.url_jsc;

int main() {
    using namespace mbun::url_jsc;
    auto parsed{parse("https://user:pass@example.com:8443/a?x=1#frag")};
    assert(parsed.has_value());
    assert(parsed->protocol == "https");
    assert(parsed->username == "user");
    assert(parsed->password == "pass");
    assert(parsed->hostname == "example.com");
    assert(parsed->port == "8443");
    assert(parsed->pathname == "/a");
    assert(parsed->search == "?x=1");
    assert(parsed->hash == "#frag");
    assert(parsed->origin == "https://example.com:8443");
    assert(serialize(*parsed) == "https://user:pass@example.com:8443/a?x=1#frag");

    auto href{UrlBinding::href_from_js("https://localhost/", string_href_adapter())};
    assert(href.has_value() && *href == "https://localhost/");
    assert(!UrlBinding::from_js("", string_href_adapter()).has_value());
    assert(!UrlBinding::from_js("not a url", string_href_adapter()).has_value());

    assert(!can_parse("https://"));
    assert(!can_parse("1://host"));
    assert(!can_parse("https://example.com:99999"));
    assert(!can_parse("//host"));
    assert(!can_parse("relative"));
    assert(can_parse("mailto:user@example.com"));
    assert(can_parse(" https://example.com "));
    assert(!can_parse("https://exa%mple.com"));
    assert(can_parse("https://exa%6dple.com"));
    return 0;
}
