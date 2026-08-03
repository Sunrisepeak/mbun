// test_package_manager.cpp — unit tests for the wave-2 install port:
// mbun.install.network_task (request descriptors, retry classification,
// dedupe), mbun.install.package_manager (Options::load flag machine,
// UpdateRequest parse, resolution walker) and
// mbun.install.resolvers.folder_resolver (path logic + collision-safe cache).
// Vectors follow bun src/install/NetworkTask.rs, PackageManager/
// {PackageManagerOptions,UpdateRequest,runTasks,PackageManagerEnqueue}.rs and
// resolvers/folder_resolver.rs.
import std;
import mbun.install.network_task;
import mbun.install.package_manager;
import mbun.install.npm.registry;
import mbun.install.npm.manifest;
import mbun.install.dependency;
import mbun.install.resolvers.folder_resolver;

namespace net = mbun::install::network_task;
namespace pm = mbun::install::package_manager;
namespace registry = mbun::install::npm::registry;
namespace npm = mbun::install::npm;
namespace dep = mbun::install::dependency;
namespace fr = mbun::install::resolvers::folder_resolver;

namespace {

int gChecks{0};
int gFailures{0};

void check(bool cond, std::string_view what) {
    ++gChecks;
    if (!cond) {
        ++gFailures;
        std::println("  FAIL {}", what);
    }
}

bool has_header(const net::RequestDescriptor& r, std::string_view name, std::string_view value) {
    for (const auto& h : r.headers) {
        if (h.name == name && h.value == value) {
            return true;
        }
    }
    return false;
}

bool has_header_named(const net::RequestDescriptor& r, std::string_view name) {
    for (const auto& h : r.headers) {
        if (h.name == name) {
            return true;
        }
    }
    return false;
}

// ── network_task ────────────────────────────────────────────────────────────

void test_manifest_request() {
    registry::Scope scope{};  // default https://registry.npmjs.org/
    {
        auto r{net::for_manifest("lodash", scope, "", "", false)};
        check(r.has_value(), "manifest lodash ok");
        check(r->url == "https://registry.npmjs.org/lodash", "manifest url lodash");
        check(has_header(*r, "Accept", std::string{net::ACCEPT_HEADER_VALUE}),
              "manifest Accept header");
        check(!has_header_named(*r, "Authorization"), "no auth without token");
    }
    {
        // Scoped names are %2f-encoded like the npm CLI.
        auto r{net::for_manifest("@storybook/addons", scope, "", "", false)};
        check(r.has_value() && r->url == "https://registry.npmjs.org/@storybook%2faddons",
              "scoped name %2f-encoded");
    }
    {
        // ETag wins over Last-Modified.
        auto r{net::for_manifest("lodash", scope, "W/\"abc\"", "Wed, 21 Oct 2015", false)};
        check(r.has_value() && has_header(*r, "If-None-Match", "W/\"abc\"") &&
                  !has_header_named(*r, "If-Modified-Since"),
              "etag beats last-modified");
    }
    {
        auto r{net::for_manifest("lodash", scope, "", "Wed, 21 Oct 2015", false)};
        check(r.has_value() && has_header(*r, "If-Modified-Since", "Wed, 21 Oct 2015"),
              "last-modified fallback");
    }
    {
        // Extended manifest uses the plain-json Accept value.
        auto r{net::for_manifest("lodash", scope, "", "", true)};
        check(r.has_value() &&
                  has_header(*r, "Accept", std::string{net::ACCEPT_HEADER_VALUE_EXTENDED}),
              "extended Accept header");
    }
    {
        registry::Scope tok{scope};
        tok.token = "sekret";
        auto r{net::for_manifest("lodash", tok, "", "", false)};
        check(r.has_value() && has_header(*r, "Authorization", "Bearer sekret") &&
                  has_header(*r, "npm-auth-type", "legacy"),
              "bearer token + npm-auth-type");
    }
    {
        registry::Scope bad{registry::Scope::for_url("", "ftp://registry.example.com/")};
        auto r{net::for_manifest("lodash", bad, "", "", false)};
        check(!r.has_value() && r.error().code == net::NetworkErrorCode::InvalidURL,
              "non-http registry rejected");
    }
    {
        // A package name must not escape the registry directory ("/"-containing
        // names are %2f-encoded, so the escape vector is dot segments).
        registry::Scope deep{registry::Scope::for_url("", "https://host.example.com/sub/registry/")};
        auto ok{net::for_manifest("lodash", deep, "", "", false)};
        check(ok.has_value() && ok->url == "https://host.example.com/sub/registry/lodash",
              "deep registry join");
        auto r{net::for_manifest("..", deep, "", "", false)};
        check(!r.has_value() && r.error().code == net::NetworkErrorCode::InvalidURL,
              "origin-escaping name rejected");
    }
}

void test_tarball_request() {
    registry::Scope scope{};
    scope.token = "sekret";
    check(net::build_tarball_url("https://registry.npmjs.org/", "@scope/pkg", 1, 2, 3, "", "") ==
              "https://registry.npmjs.org/@scope/pkg/-/pkg-1.2.3.tgz",
          "build_tarball_url scoped");
    check(net::build_tarball_url("https://r.example.com", "foo", 1, 0, 0, "beta.1", "meta") ==
              "https://r.example.com/foo/-/foo-1.0.0-beta.1+meta.tgz",
          "build_tarball_url pre+build");
    {
        // Same-origin tarball keeps credentials; default :443 spelling matches.
        auto r{net::for_tarball("https://registry.npmjs.org:443/foo/-/foo-1.0.0.tgz", "foo", scope,
                                net::Authorization::AllowAuthorization)};
        check(r.has_value() && has_header(*r, "Authorization", "Bearer sekret"),
              "same-origin tarball keeps auth (default port normalized)");
    }
    {
        // Cross-origin tarball must NOT receive the registry credentials.
        auto r{net::for_tarball("https://evil.example.com/foo.tgz", "foo", scope,
                                net::Authorization::AllowAuthorization)};
        check(r.has_value() && !has_header_named(*r, "Authorization"),
              "cross-origin tarball drops auth");
    }
    {
        auto r{net::for_tarball("file:///tmp/foo.tgz", "foo", scope,
                                net::Authorization::NoAuthorization)};
        check(!r.has_value(), "non-http tarball url rejected");
    }
}

void test_response_classification() {
    net::RetryPolicy policy{};  // max_retry_count = 5
    using V = net::ResponseVerdict;
    check(net::classify_manifest_response(false, 0, 0, policy).verdict == V::Retry,
          "network error retries");
    check(net::classify_manifest_response(false, 0, 5, policy).verdict == V::Fail,
          "network error exhausts retries");
    check(net::classify_manifest_response(false, 0, 5, policy).error_name == "HTTPError",
          "exhausted retries -> HTTPError");
    check(net::classify_manifest_response(true, 503, 2, policy).verdict == V::Retry,
          "5xx retries");
    check(net::classify_manifest_response(true, 404, 0, policy).verdict == V::Fail &&
              net::classify_manifest_response(true, 404, 0, policy).error_name ==
                  "PackageManifestHTTP404",
          "404 fails without retry");
    check(net::classify_manifest_response(true, 429, 0, policy).error_name ==
              "PackageManifestHTTP4xx",
          "429 -> 4xx bucket");
    check(net::classify_manifest_response(true, 304, 0, policy).verdict == V::NotModified,
          "304 -> manifest cache hit");
    check(net::classify_manifest_response(true, 200, 0, policy).verdict == V::Success,
          "200 -> success");
    check(net::classify_tarball_response(true, 403, 0, policy).error_name == "TarballHTTP403",
          "tarball 403 name");
    check(net::reduce_max_simultaneous_requests(64, 4) == 32, "halve request budget");
    check(net::reduce_max_simultaneous_requests(4, 4) == 4, "budget floor");
}

void test_dedupe_map() {
    net::DedupeMap map{};
    std::uint64_t id{net::task_id::for_manifest("lodash")};
    check(map.is_network_task_required(id), "missing entry defaults required");
    check(!map.has_created_network_task(id, false), "first create returns false");
    check(map.has_created_network_task(id, false), "second create returns true");
    check(!map.is_network_task_required(id), "optional stays optional");
    check(map.has_created_network_task(id, true), "third create still exists");
    check(map.is_network_task_required(id), "required consumer upgrades entry");
    map.remove(id);
    check(map.is_network_task_required(id), "removed entry defaults required");
    check(net::task_id::for_manifest("a") != net::task_id::for_tarball("a"),
          "manifest/tarball ids namespaced");
    check((net::task_id::for_git_clone("u") >> 61) == 4, "git clone tag bits");
    check((net::task_id::for_git_checkout("u", "r") >> 61) == 5, "git checkout tag bits");
}

// ── package_manager.options ─────────────────────────────────────────────────

void test_options_load() {
    using pm::Do;
    using pm::Enable;
    pm::LoadEnvironment env{};

    {
        pm::Options o{};
        o.load(env, nullptr, nullptr, pm::Subcommand::Install);
        check(o.do_.contains(Do::SAVE_LOCKFILE) && o.do_.contains(Do::INSTALL_PACKAGES) &&
                  o.do_.contains(Do::RUN_SCRIPTS),
              "defaults: save/install/scripts on");
        check(o.enable.contains(Enable::MANIFEST_CACHE) && o.enable.contains(Enable::CACHE),
              "defaults: caches on");
        check(!o.did_override_default_scope, "default scope not overridden");
        check(o.max_retry_count == 5, "default retry count");
    }
    {
        pm::CommandLineArguments cli{};
        cli.production = true;
        pm::Options o{};
        o.load(env, &cli, nullptr, pm::Subcommand::Install);
        check(!o.local_package_features.dev_dependencies, "--production drops devDeps");
        check(o.enable.contains(Enable::FROZEN_LOCKFILE) && o.enable.contains(Enable::FAIL_EARLY),
              "--production implies frozen + fail-early");
        check(!o.do_.contains(Do::SAVE_LOCKFILE), "frozen lockfile is never saved");
    }
    {
        pm::CommandLineArguments cli{};
        cli.no_save = true;
        pm::Options o{};
        o.load(env, &cli, nullptr, pm::Subcommand::Add);
        check(!o.do_.contains(Do::SAVE_LOCKFILE) && !o.do_.contains(Do::WRITE_PACKAGE_JSON),
              "--no-save skips lockfile + package.json");
    }
    {
        pm::CommandLineArguments cli{};
        cli.dry_run = true;
        pm::Options o{};
        o.load(env, &cli, nullptr, pm::Subcommand::Install);
        check(o.dry_run && !o.do_.contains(Do::INSTALL_PACKAGES) &&
                  !o.do_.contains(Do::SAVE_LOCKFILE) && !o.do_.contains(Do::WRITE_PACKAGE_JSON),
              "--dry-run disables all writes");
    }
    {
        pm::CommandLineArguments cli{};
        cli.omit = pm::Omit{.dev = true, .optional = true, .peer = false};
        pm::Options o{};
        o.load(env, &cli, nullptr, pm::Subcommand::Install);
        check(!o.local_package_features.dev_dependencies, "--omit=dev");
        check(!o.local_package_features.optional_dependencies &&
                  !o.remote_package_features.optional_dependencies,
              "--omit=optional both feature sets");
        check(o.local_package_features.peer_dependencies, "peer untouched");
    }
    {
        // bun update never reads the manifest cache.
        pm::Options o{};
        o.load(env, nullptr, nullptr, pm::Subcommand::Update);
        check(!o.enable.contains(Enable::MANIFEST_CACHE) &&
                  !o.enable.contains(Enable::MANIFEST_CACHE_CONTROL),
              "update disables manifest cache");
    }
    {
        pm::LoadEnvironment env2{};
        env2.get = [](std::string_view key) -> std::optional<std::string_view> {
            if (key == "BUN_CONFIG_HTTP_RETRY_COUNT") {
                return "9";
            }
            if (key == "BUN_CONFIG_SKIP_SAVE_LOCKFILE") {
                return "1";
            }
            return std::nullopt;
        };
        pm::Options o{};
        o.load(env2, nullptr, nullptr, pm::Subcommand::Install);
        check(o.max_retry_count == 9, "BUN_CONFIG_HTTP_RETRY_COUNT");
        check(!o.do_.contains(Do::SAVE_LOCKFILE), "BUN_CONFIG_SKIP_SAVE_LOCKFILE");
    }
    {
        pm::BunInstallConfig cfg{};
        cfg.frozen_lockfile = true;
        pm::Options o{};
        o.load(env, nullptr, &cfg, pm::Subcommand::Install);
        check(o.enable.contains(Enable::FROZEN_LOCKFILE) && !o.do_.contains(Do::SAVE_LOCKFILE),
              "bunfig frozenLockfile");
    }
    {
        // Scoped registry lookup is hash-collision-safe by name compare.
        pm::Options o{};
        o.load(env, nullptr, nullptr, pm::Subcommand::Install);
        o.registries.insert_or_assign(registry::string_hash("myorg"),
                                      registry::Scope::for_url("myorg", "https://r.corp.local/"));
        check(o.scope_for_package_name("@myorg/tool").url == "https://r.corp.local/",
              "scoped registry hit");
        check(o.scope_for_package_name("@other/tool").url == registry::DEFAULT_URL,
              "unscoped falls back to default");
        check(o.scope_for_package_name("plain").url == registry::DEFAULT_URL,
              "plain name uses default scope");
    }
    {
        pm::CommandLineArguments cli{};
        cli.registry = "https://mirror.example.com/npm/";
        pm::Options o{};
        o.load(env, &cli, nullptr, pm::Subcommand::Install);
        check(o.did_override_default_scope, "--registry overrides default scope");
    }
}

void test_workspace_filter() {
    using WF = pm::WorkspaceFilter;
    check(WF::init("*", "/repo").kind == WF::Kind::All, "filter *");
    check(WF::init("**", "/repo").kind == WF::Kind::All, "filter **");
    {
        auto f{WF::init("pkg-*", "/repo")};
        check(f.kind == WF::Kind::Name && f.pattern == "pkg-*", "name filter");
    }
    {
        auto f{WF::init("./packages/a", "/repo")};
        check(f.kind == WF::Kind::Path && f.pattern == "/repo/packages/a", "path filter joined");
    }
    {
        auto f{WF::init("!!pkg", "/repo")};
        check(f.kind == WF::Kind::Name && f.pattern == "pkg", "double negation cancels");
    }
    {
        auto f{WF::init("!pkg", "/repo")};
        check(f.kind == WF::Kind::Name && f.pattern == "!pkg", "single negation kept");
    }
}

// ── package_manager.update_request ──────────────────────────────────────────

void test_update_request_parse() {
    using pm::parse_update_requests;
    {
        std::vector<std::string_view> pos{"lodash"};
        auto r{parse_update_requests(pos, pm::Subcommand::Add)};
        check(r.has_value() && r->size() == 1, "parse lodash");
        if (r.has_value() && !r->empty()) {
            check(r->front().is_aliased && r->front().name == "lodash", "bare name is alias");
        }
    }
    {
        std::vector<std::string_view> pos{"lodash@4.17.21"};
        auto r{parse_update_requests(pos, pm::Subcommand::Add)};
        check(r.has_value() && r->size() == 1, "parse lodash@4.17.21");
        if (r.has_value() && !r->empty()) {
            const auto& u{r->front()};
            check(u.is_aliased && u.name == "lodash", "name@version alias");
            check(u.version.tag == dep::Tag::Npm && u.version.npm.version == "4.17.21",
                  "npm version payload");
        }
    }
    {
        std::vector<std::string_view> pos{"@scope/pkg@^2.0.0"};
        auto r{parse_update_requests(pos, pm::Subcommand::Add)};
        check(r.has_value() && r->size() == 1 && r->front().name == "@scope/pkg",
              "scoped name@range");
    }
    {
        // Alias to another package: name@npm:real@version.
        std::vector<std::string_view> pos{"my-alias@npm:lodash@4.0.0"};
        auto r{parse_update_requests(pos, pm::Subcommand::Add)};
        check(r.has_value() && r->size() == 1, "parse npm: alias");
        if (r.has_value() && !r->empty()) {
            check(r->front().name == "my-alias" && r->front().version.tag == dep::Tag::Npm,
                  "alias name kept");
        }
    }
    {
        // link/unlink wraps bare names as name@link:name.
        std::vector<std::string_view> pos{"mypkg"};
        auto r{parse_update_requests(pos, pm::Subcommand::Link)};
        check(r.has_value() && r->size() == 1, "parse link positional");
        if (r.has_value() && !r->empty()) {
            check(r->front().version.tag == dep::Tag::Symlink, "link: wraps to symlink tag");
        }
    }
    {
        // Duplicates (same name hash + length) collapse.
        std::vector<std::string_view> pos{"lodash", "lodash"};
        auto r{parse_update_requests(pos, pm::Subcommand::Add)};
        check(r.has_value() && r->size() == 1, "duplicate positionals dedup");
    }
    {
        // Whitespace trims; backslashes normalize to posix.
        std::vector<std::string_view> pos{"  lodash@1.0.0 \n"};
        auto r{parse_update_requests(pos, pm::Subcommand::Add)};
        check(r.has_value() && r->size() == 1 && r->front().name == "lodash",
              "whitespace trimmed");
    }
    check(pm::is_npm_package_name("@scope/pkg") && pm::is_npm_package_name("foo-bar") &&
              !pm::is_npm_package_name("@scope") && !pm::is_npm_package_name("") &&
              !pm::is_npm_package_name("bad name"),
          "is_npm_package_name vectors");
}

// ── package_manager.enqueue (dependency walk) ───────────────────────────────

npm::PackageManifest make_manifest(std::string name,
                                   std::vector<std::pair<std::string, npm::DepMap>> versions) {
    npm::PackageManifest m{};
    m.name = std::move(name);
    for (auto& [ver, deps] : versions) {
        npm::PackageVersion pv{};
        pv.version = ver;
        pv.dependencies = std::move(deps);
        m.releases.push_back(std::move(pv));
    }
    if (!m.releases.empty()) {
        m.dist_tags.emplace_back("latest", m.releases.back().version);
    }
    return m;
}

void test_resolution_walker() {
    // Graph: app -> a@^1.0.0 -> b@^2.0.0 ; a also at 0.9.0 (must not win).
    npm::PackageManifest ma{make_manifest(
        "a", {{"0.9.0", {}}, {"1.4.0", {{"b", "^2.0.0"}}}})};
    npm::PackageManifest mb{make_manifest("b", {{"2.3.1", {}}})};

    {
        // All manifests in memory: the walk resolves transitively, no network.
        pm::ResolutionWalker w{};
        w.add_manifest(&ma);
        w.add_manifest(&mb);

        dep::Dependency root{};
        root.name = "a";
        root.version = *dep::parse("a", "^1.0.0");
        pm::DependencyID root_id{w.append_dependency(root)};
        w.enqueue_dependency(root_id, true);

        check(w.is_done(), "in-memory walk completes");
        check(w.pending_manifest_requests.empty(), "no network tasks needed");
        const pm::ResolvedPackage* pa{w.resolution_of(root_id)};
        check(pa != nullptr && pa->name == "a" && pa->version == "1.4.0",
              "a resolves to best 1.4.0");
        check(w.packages.size() == 2, "transitive b resolved (2 packages)");
        check(w.dependencies.size() == 2, "b's dependency row appended");
        if (w.dependencies.size() == 2) {
            const pm::ResolvedPackage* pb{w.resolution_of(1)};
            check(pb != nullptr && pb->name == "b" && pb->version == "2.3.1",
                  "b resolves to 2.3.1");
        }
    }
    {
        // Missing manifests park dependencies; on_manifest_downloaded drains.
        pm::ResolutionWalker w{};
        dep::Dependency d1{};
        d1.name = "a";
        d1.version = *dep::parse("a", "^1.0.0");
        pm::DependencyID id1{w.append_dependency(d1)};
        // A second consumer of the same manifest must NOT create a second task.
        dep::Dependency d2{};
        d2.name = "a";
        d2.version = *dep::parse("a", "1.4.0");
        pm::DependencyID id2{w.append_dependency(d2)};

        w.enqueue_dependency(id1, true);
        w.enqueue_dependency(id2, false);

        check(!w.is_done(), "walk waits on manifest");
        check(w.pending_manifest_requests.size() == 1 && w.pending_manifest_requests[0] == "a",
              "one deduped manifest request for a");
        check(w.task_queue.size() == 1 &&
                  w.task_queue.begin()->second.size() == 2,
              "both dependencies parked on one task");

        w.on_manifest_downloaded(&ma);
        check(w.pending_manifest_requests.size() == 2, "b's manifest requested next");
        w.on_manifest_downloaded(&mb);
        check(w.is_done(), "walk completes after downloads");
        check(w.resolution_of(id1) != nullptr && w.resolution_of(id1)->version == "1.4.0",
              "parked dep 1 resolved");
        check(w.resolution_of(id2) != nullptr && w.resolution_of(id2)->version == "1.4.0",
              "parked dep 2 resolved to same package");
        check(w.resolution_of(id1) == w.resolution_of(id2), "package deduped");
    }
    {
        // Optional-vs-required upgrade travels through the dedupe map.
        pm::ResolutionWalker w{};
        dep::Dependency opt{};
        opt.name = "a";
        opt.version = *dep::parse("a", "^1.0.0");
        opt.behavior = dep::Behavior{dep::Behavior::OPTIONAL};
        pm::DependencyID oid{w.append_dependency(opt)};
        w.enqueue_dependency(oid, false);
        std::uint64_t tid{net::task_id::for_manifest("a")};
        check(!w.network_dedupe_map.is_network_task_required(tid), "optional dep task optional");

        dep::Dependency req{};
        req.name = "a";
        req.version = *dep::parse("a", "1.4.0");
        pm::DependencyID rid{w.append_dependency(req)};
        w.enqueue_dependency(rid, false);
        check(w.network_dedupe_map.is_network_task_required(tid),
              "required consumer upgrades task");
    }
}

// ── resolvers.folder_resolver ───────────────────────────────────────────────

void test_folder_resolver() {
    using GR = fr::GlobalOrRelative;
    {
        auto p{fr::normalize_package_json_path(GR::Relative, "", "/repo", "./packages/a/")};
        check(p.abs == "/repo/packages/a/package.json", "relative abs path");
        check(p.rel == "packages/a", "relative rel path");
    }
    {
        auto p{fr::normalize_package_json_path(GR::Relative, "", "/repo", "../side")};
        check(p.abs == "/side/package.json" && p.rel == "../side", "dot-dot escapes root");
    }
    {
        auto p{fr::normalize_package_json_path(GR::Global, "/home/x/.bun/links", "/repo", "pkg")};
        check(p.abs == "/home/x/.bun/links/pkg/package.json", "global prefix joined");
    }

    fr::Map folders{};
    int calls{0};
    fr::ResolveFn ok{[&calls](std::string_view, std::string_view)
                         -> std::expected<fr::PackageID, std::string> {
        ++calls;
        return 7;
    }};
    auto r1{fr::get_or_put(folders, GR::Relative, "", "/repo", "./a", ok)};
    check(r1.kind == fr::FolderResolution::Kind::NewPackageId && r1.package_id == 7,
          "first resolve is NewPackageId");
    auto r2{fr::get_or_put(folders, GR::Relative, "", "/repo", "./a", ok)};
    check(r2.kind == fr::FolderResolution::Kind::PackageId && r2.package_id == 7,
          "cached resolve is PackageId");
    check(calls == 1, "resolver called once");

    fr::ResolveFn missing{[](std::string_view, std::string_view)
                              -> std::expected<fr::PackageID, std::string> {
        return std::unexpected(std::string{"ENOENT"});
    }};
    auto r3{fr::get_or_put(folders, GR::Relative, "", "/repo", "./missing", missing)};
    check(r3.kind == fr::FolderResolution::Kind::Err && r3.error == "MissingPackageJSON",
          "ENOENT maps to MissingPackageJSON");
    auto r4{fr::get_or_put(folders, GR::Relative, "", "/repo", "./missing", ok)};
    check(r4.kind == fr::FolderResolution::Kind::Err, "error result cached");
}

}  // namespace

int main() {
    test_manifest_request();
    test_tarball_request();
    test_response_classification();
    test_dedupe_map();
    test_options_load();
    test_workspace_filter();
    test_update_request_parse();
    test_resolution_walker();
    test_folder_resolver();

    std::println("test_package_manager: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
