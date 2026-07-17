import std;
import mbun.http_types;

namespace {

int checks{0};
int failures{0};

template <typename A, typename B>
void eq(const A& actual, const B& expected, std::string_view name) {
    ++checks;
    if (!(actual == expected)) {
        ++failures;
        std::println("FAIL {}", name);
    }
}

void truth(bool value, std::string_view name) { eq(value, true, name); }

void test_encoding() {
    using enum mbun::http_types::Encoding;
    truth(mbun::http_types::can_use_lib_deflate(gzip), "encoding.gzip.deflate");
    truth(mbun::http_types::can_use_lib_deflate(deflate), "encoding.deflate.deflate");
    eq(mbun::http_types::can_use_lib_deflate(brotli), false, "encoding.brotli.no_deflate");
    truth(mbun::http_types::is_compressed(gzip), "encoding.gzip.compressed");
    truth(mbun::http_types::is_compressed(zstd), "encoding.zstd.compressed");
    eq(mbun::http_types::is_compressed(identity), false, "encoding.identity.uncompressed");
    eq(mbun::http_types::is_compressed(chunked), false, "encoding.chunked.uncompressed");
}

void test_fetch() {
    using namespace mbun::http_types;
    eq(to_string(FetchCacheMode::only_if_cached), "only-if-cached", "fetch.cache.string");
    eq(fetch_cache_mode_from_string("no-cache"), FetchCacheMode::no_cache, "fetch.cache.parse");
    eq(fetch_cache_mode_from_string("No-Cache"), std::nullopt, "fetch.cache.case");
    eq(to_string(FetchRedirect::manual), "manual", "fetch.redirect.string");
    eq(fetch_redirect_from_string("error"), FetchRedirect::error, "fetch.redirect.parse");
    eq(fetch_redirect_from_string("follow "), std::nullopt, "fetch.redirect.ows");
    eq(to_string(FetchRequestMode::same_origin), "same-origin", "fetch.mode.string");
    eq(fetch_request_mode_from_string("navigate"), FetchRequestMode::navigate, "fetch.mode.parse");
    eq(fetch_request_mode_from_string("corss"), std::nullopt, "fetch.mode.unknown");
}

void test_etag() {
    using namespace mbun::http_types;
    const EntityTag weak{parse_entity_tag("  W/  \"abc\" \t")};
    eq(weak.tag, "abc", "etag.parse.weak.value");
    truth(weak.is_weak, "etag.parse.weak.flag");
    const EntityTag strong{parse_entity_tag("\"abc\"")};
    truth(!strong.is_weak, "etag.parse.strong.flag");
    eq(strong.tag, "abc", "etag.parse.strong.value");
    truth(if_none_match("\"abc\"", "W/\"abc\""), "etag.match.weak");
    truth(if_none_match("W/\"abc\"", "\"abc\", \"other\""), "etag.match.list");
    truth(if_none_match("\"ab,cd\"", "\"ab,cd\", \"other\""), "etag.match.comma.quote");
    truth(if_none_match("\"abc\"", "  *  "), "etag.match.wildcard");
    eq(if_none_match("\"abc\"", "\"other\""), false, "etag.no_match");
    eq(format_entity_tag(0xabcULL), "\"0000000000000abc\"", "etag.format.zero_pad");
}

} // namespace

int main() {
    test_encoding();
    test_fetch();
    test_etag();
    std::println("test_http_types: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}
