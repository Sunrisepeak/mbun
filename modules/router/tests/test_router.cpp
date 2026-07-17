// Pure-logic vectors from compat/bun/test/js/bun/util/filesystem_router.test.ts.
// The injected filesystem keeps this test independent from JSC and OS APIs.
import std;
import mbun.router;

namespace {

using mbun::router::FileSystem;
using mbun::router::FileSystemRouter;
using mbun::router::Options;

FileSystem make_fs(std::vector<std::string> files) {
    return FileSystem{[files = std::move(files)] { return files; }};
}

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
}

void test_routes_and_precedence() {
    FileSystemRouter router{make_fs({
                                "index.tsx",
                                "posts.tsx",
                                "posts/[id].tsx",
                                "posts/hey.tsx",
                                "posts/[...rest].tsx",
                                "posts/[[...optional]].tsx",
                                "node_modules/ignored.tsx",
                                ".hidden.tsx",
                                "ignored.js",
                            }),
                            Options{"pages", {".tsx"}}};

    require(router.routes().at("/") == "pages/index.tsx", "index route");
    require(router.routes().at("/posts") == "pages/posts.tsx", "file route");
    require(router.routes().at("/posts/[id]") == "pages/posts/[id].tsx", "dynamic route");
    require(!router.routes().contains("/node_modules/ignored"), "node_modules excluded");
    require(!router.routes().contains("/.hidden"), "hidden route excluded");
    require(!router.routes().contains("/ignored"), "extension filtered");

    auto exact{router.match("/posts/hey")};
    require(exact && exact->name == "/posts/hey", "static wins over dynamic");
    auto dynamic{router.match("/posts/hello-world")};
    require(dynamic && dynamic->name == "/posts/[id]", "dynamic route match");
    require(dynamic->params.at("id") == "hello-world", "dynamic parameter");
    auto catch_all{router.match("/posts/a/b")};
    require(catch_all && catch_all->name == "/posts/[...rest]", "catch-all match");
    require(catch_all->params.at("rest") == "a/b", "catch-all parameter");
    auto optional{router.match("/posts")};
    require(optional && optional->name == "/posts", "optional catch-all does not shadow exact");
}

void test_url_decoding_and_query() {
    FileSystemRouter router{make_fs({"posts/[id].tsx"}), Options{"pages", {"tsx"}}};
    auto match{router.match("/posts/a%20b?hello=world&second=2")};
    require(match && match->pathname == "/posts/a b", "pathname percent decode");
    require(match->params.at("id") == "a b", "parameter percent decode");
    require(match->query.at("hello") == "world", "query value");
    require(match->query.at("second") == "2", "second query value");
}

void test_index_normalization() {
    // Mirrors bun's "should support index routes": "/index", "/posts/index"
    // and trailing slashes all collapse to their parent route.
    FileSystemRouter router{make_fs({"index.tsx", "posts/[id].tsx", "posts.tsx", "posts/hey.tsx"}),
                            Options{"pages", {".tsx"}}};

    for (std::string_view route : {"/", "/index", "/index/", "/index/index"}) {
        auto m{router.match(route)};
        require(m && m->name == "/", "index collapses to /");
        require(m->params.empty(), "index route has no params");
    }
    for (std::string_view route : {"/posts", "/posts/index", "/posts/"}) {
        auto m{router.match(route)};
        require(m && m->name == "/posts", "posts index collapses to /posts");
        require(m->file_path == "pages/posts.tsx", "posts file path");
        require(m->params.empty(), "posts route has no params");
    }
}

void test_nested_dynamic_routes() {
    // Deep nesting from bun's "should find files": index/dynamic at every level.
    FileSystemRouter router{make_fs({
                                "index.tsx",
                                "[id].tsx",
                                "abc/index.tsx",
                                "abc/[id].tsx",
                                "abc/def/[id].tsx",
                                "abc/def/ghi/index.tsx",
                                "abc/def/ghi/[id].tsx",
                            }),
                            Options{"pages", {".tsx"}}};

    require(router.routes().at("/") == "pages/index.tsx", "root index");
    require(router.routes().at("/[id]") == "pages/[id].tsx", "root dynamic");
    require(router.routes().at("/abc") == "pages/abc/index.tsx", "nested index");
    require(router.routes().at("/abc/def/ghi") == "pages/abc/def/ghi/index.tsx", "deep index");

    auto deep{router.match("/abc/def/ghi/hello")};
    require(deep && deep->name == "/abc/def/ghi/[id]", "deep dynamic match");
    require(deep->params.at("id") == "hello", "deep dynamic param");

    auto shallow{router.match("/wat")};
    require(shallow && shallow->name == "/[id]", "root dynamic match");
    require(shallow->params.at("id") == "wat", "root dynamic param");
}

void test_optional_catch_all() {
    // bun's "should support optional catch-all routes": the optional catch-all
    // only wins when nothing more specific (static/dynamic) matches.
    FileSystemRouter router{make_fs({"index.tsx", "posts/[id].tsx", "posts.tsx",
                                     "posts/hey.tsx", "posts/[[...id]].tsx"}),
                            Options{"pages", {".tsx"}}};

    for (std::string_view route : {"/posts/123", "/posts/hey", "/posts/zorp", "/posts", "/index", "/posts/"}) {
        auto m{router.match(route)};
        require(m && m->name != "/posts/[[...id]]", "optional catch-all is not shadowed in");
    }
    for (std::string_view route : {"/posts/hey/there", "/posts/hey/there/you", "/posts/zorp/123"}) {
        auto m{router.match(route)};
        require(m && m->name == "/posts/[[...id]]", "optional catch-all deeper match");
        require(m->file_path == "pages/posts/[[...id]].tsx", "optional catch-all file");
    }
    require(router.match("/posts/hey/there")->params.at("id") == "hey/there", "optional catch-all param");
    require(router.match("/posts/zorp/123")->params.at("id") == "zorp/123", "optional catch-all param 2");
}

void test_catch_all_params() {
    // bun's "should support catch-all routes".
    FileSystemRouter router{make_fs({"index.tsx", "posts/[id].tsx", "posts.tsx", "posts/hey.tsx",
                                     "posts/[...id].tsx", "posts/wow/[[...id]].tsx"}),
                            Options{"pages", {".tsx"}}};

    for (std::string_view route : {"/posts/123", "/posts/hey", "/posts/zorp", "/posts", "/index", "/posts/"}) {
        auto m{router.match(route)};
        require(!m || m->name != "/posts/[...id]", "catch-all not shadowing specific");
    }
    for (std::string_view route : {"/posts/hey/there", "/posts/hey/there/you", "/posts/zorp/123", "/posts/wow/hey/there"}) {
        auto m{router.match(route)};
        require(m && m->name == "/posts/[...id]", "catch-all deeper match");
    }
    require(router.match("/posts/hey/there/you")->params.at("id") == "hey/there/you", "catch-all param");
}

void test_app_dir_and_style() {
    // The "pages" vs "app" distinction in bun is purely the `dir` option; only
    // the "nextjs" style is supported.
    FileSystemRouter router{make_fs({"index.tsx", "users/[id].tsx"}),
                            Options{.dir = "app", .file_extensions = {".tsx"}, .style = "nextjs"}};

    require(router.routes().at("/") == "app/index.tsx", "app dir index");
    auto m{router.match("/users/42")};
    require(m && m->name == "/users/[id]", "app dir dynamic");
    require(m->file_path == "app/users/[id].tsx", "app dir dynamic file");
    require(m->params.at("id") == "42", "app dir dynamic param");
}

void test_malformed_route_ignored() {
    // A filename missing its closing bracket is not a valid route.
    FileSystemRouter router{make_fs({"index.tsx", "[foo.tsx"}), Options{"pages", {".tsx"}}};
    require(router.routes().contains("/"), "valid index kept");
    require(!router.routes().contains("/[foo"), "malformed route dropped");
    require(router.routes().size() == 1, "only the valid route survives");
}

void test_query_merge_with_params() {
    // bun's ".query works with dynamic routes, including params": params and
    // query coexist; query is parsed separately from the route params.
    FileSystemRouter router{make_fs({"posts/[id].tsx"}), Options{"pages", {".tsx"}}};
    auto m{router.match("/posts/123?hello=world&second=2&third=3")};
    require(m && m->name == "/posts/[id]", "dynamic route with query");
    require(m->params.at("id") == "123", "param alongside query");
    require(m->query.at("hello") == "world" && m->query.at("second") == "2"
                && m->query.at("third") == "3",
            "all query pairs parsed");
    require(m->params.size() == 1 && m->query.size() == 3, "params and query kept distinct");

    auto none{router.match("/posts/123")};
    require(none && none->query.empty(), "no query yields empty map");
}

}

int main() {
    test_routes_and_precedence();
    test_url_decoding_and_query();
    test_index_normalization();
    test_nested_dynamic_routes();
    test_optional_catch_all();
    test_catch_all_params();
    test_app_dir_and_style();
    test_malformed_route_ignored();
    test_query_merge_with_params();
    std::println("router checks passed");
}
