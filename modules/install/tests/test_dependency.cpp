// test_dependency.cpp — unit tests for the install dependency data model
// (mbun.install.dependency / resolution / versioned_url / config_version /
// external_slice). Vectors are drawn from the parse/infer edge cases encoded in
// bun src/install/dependency.rs (Tag::infer, split_name_and_maybe_version,
// parse_with_tag) and resolution.rs (from_text_lockfile).
import std;
import mbun.install.dependency;
import mbun.install.resolution;
import mbun.install.versioned_url;
import mbun.install.config_version;
import mbun.install.external_slice;

namespace dep = mbun::install::dependency;
namespace res = mbun::install::resolution;
using mbun::install::ConfigVersion;
using mbun::install::ExternalSlice;

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

void test_split() {
    {
        auto [n, v] = dep::split_name_and_maybe_version("foo@1.1.1");
        check(n == "foo" && v && *v == "1.1.1", "split foo@1.1.1");
    }
    {
        auto [n, v] = dep::split_name_and_maybe_version("@foo/bar@1.1.1");
        check(n == "@foo/bar" && v && *v == "1.1.1", "split @foo/bar@1.1.1");
    }
    {
        auto [n, v] = dep::split_name_and_maybe_version("foo");
        check(n == "foo" && !v, "split foo (no version)");
    }
    {
        auto [n, v] = dep::split_name_and_version_or_latest("foo");
        check(n == "foo" && v == "latest", "split_or_latest foo");
    }
    check(dep::unscoped_package_name("@foo/bar") == "bar", "unscoped @foo/bar");
    check(dep::unscoped_package_name("foo") == "foo", "unscoped foo");
    check(dep::is_scoped_package_name("@a/b") == std::optional<bool>{true}, "scoped @a/b");
    check(dep::is_scoped_package_name("foo") == std::optional<bool>{false}, "scoped foo -> false");
    check(!dep::is_scoped_package_name("").has_value(), "scoped empty -> err");
    check(dep::without_build_tag("1.2.3+build") == "1.2.3", "without_build_tag");
    check(dep::is_safe_install_folder_name("@a/b"), "safe @a/b");
    check(!dep::is_safe_install_folder_name("../x"), "unsafe ../x");
    check(!dep::is_safe_install_folder_name("a/../b"), "unsafe a/../b");
}

void test_infer() {
    using dep::infer_tag;
    using dep::Tag;
    check(infer_tag("") == Tag::DistTag, "infer empty -> dist_tag");
    check(infer_tag("^1.2.3") == Tag::Npm, "infer ^1.2.3 -> npm");
    check(infer_tag(">=1.0.0") == Tag::Npm, "infer >=1.0.0 -> npm");
    check(infer_tag("1.2.3") == Tag::Npm, "infer 1.2.3 -> npm");
    check(infer_tag("~1.2.3") == Tag::Npm, "infer ~1.2.3 -> npm");
    check(infer_tag("foo.tgz") == Tag::Tarball, "infer foo.tgz -> tarball");
    check(infer_tag("bar.tar.gz") == Tag::Tarball, "infer bar.tar.gz -> tarball");
    check(infer_tag("file:./local") == Tag::Folder, "infer file:./local -> folder");
    check(infer_tag("file:pkg.tgz") == Tag::Tarball, "infer file:pkg.tgz -> tarball");
    check(infer_tag("./path/to/foo") == Tag::Folder, "infer ./path -> folder");
    check(infer_tag("link:../x") == Tag::Symlink, "infer link: -> symlink");
    check(infer_tag("workspace:*") == Tag::Workspace, "infer workspace: -> workspace");
    check(infer_tag("catalog:default") == Tag::Catalog, "infer catalog: -> catalog");
    check(infer_tag("npm:react@18") == Tag::Npm, "infer npm:react@18 -> npm");
    check(infer_tag("user/repo") == Tag::Github, "infer user/repo -> github");
    check(infer_tag("user/repo#main") == Tag::Github, "infer user/repo#main -> github");
    check(infer_tag("https://github.com/user/repo") == Tag::Github, "infer github url -> github");
    check(infer_tag("beta") == Tag::DistTag, "infer beta -> dist_tag");
    check(infer_tag("latest") == Tag::DistTag, "infer latest -> dist_tag");
    check(infer_tag("v1.2.3") == Tag::Npm, "infer v1.2.3 -> npm");
    check(dep::tag_from_bytes("github") == std::optional<Tag>{Tag::Github}, "tag_from_bytes github");
    check(!dep::tag_from_bytes("bogus").has_value(), "tag_from_bytes bogus -> none");
}

void test_parse() {
    using dep::Tag;
    {
        auto v = dep::parse("react", "^1.2.3");
        check(v && v->tag == Tag::Npm && v->npm.version == "^1.2.3" && !v->npm.is_alias,
              "parse npm ^1.2.3");
    }
    {
        auto v = dep::parse("myalias", "npm:react@^18.0.0");
        check(v && v->tag == Tag::Npm && v->npm.is_alias && v->npm.name == "react" &&
                  v->npm.version == "^18.0.0",
              "parse npm alias");
    }
    {
        // The leading-`v` strip applies once the tag is Npm ("vx" -> "x").
        // (Bare "vx" *infers* as DistTag; force the Npm branch here.)
        auto v = dep::parse_with_tag("react", "vx", Tag::Npm);
        check(v && v->tag == Tag::Npm && v->npm.version == "x", "parse_with_tag npm vx -> x");
    }
    {
        auto v = dep::parse("react", "latest");
        check(v && v->tag == Tag::DistTag && v->dist_tag.name == "react" &&
                  v->dist_tag.tag == "latest",
              "parse dist_tag latest");
    }
    {
        auto v = dep::parse("pkg", "git+https://example.com/x.git#v1");
        check(v && v->tag == Tag::Git && v->git.repo == "https://example.com/x.git" &&
                  v->git.committish == "v1",
              "parse git+ with committish");
    }
    {
        auto v = dep::parse("pkg", "user/repo#branch");
        check(v && v->tag == Tag::Github && v->github.owner == "user" &&
                  v->github.repo == "repo" && v->github.committish == "branch",
              "parse github shorthand");
    }
    {
        auto v = dep::parse("pkg", "https://example.com/x.tgz");
        check(v && v->tag == Tag::Tarball && v->tarball.uri.kind == dep::URI::Kind::Remote &&
                  v->tarball.uri.value == "https://example.com/x.tgz",
              "parse remote tarball");
    }
    {
        auto v = dep::parse("pkg", "file:./x.tgz");
        check(v && v->tag == Tag::Tarball && v->tarball.uri.kind == dep::URI::Kind::Local &&
                  v->tarball.uri.value == "./x.tgz",
              "parse local file: tarball");
    }
    {
        auto v = dep::parse("pkg", "file:../local");
        check(v && v->tag == Tag::Folder && v->folder == "../local", "parse folder file:../local");
    }
    {
        auto v = dep::parse("pkg", "link:../sibling");
        check(v && v->tag == Tag::Symlink && v->symlink == "../sibling", "parse symlink");
    }
    {
        auto v = dep::parse("pkg", "workspace:^1.0.0");
        check(v && v->tag == Tag::Workspace && v->workspace == "^1.0.0", "parse workspace");
    }
    {
        auto v = dep::parse("pkg", "catalog:  react17 ");
        check(v && v->tag == Tag::Catalog && v->catalog == "react17", "parse catalog trimmed");
    }
    {
        auto v = dep::parse("pkg", "  ^1.0.0");  // leading ws trimmed
        check(v && v->tag == Tag::Npm, "parse trims leading whitespace");
    }
}

void test_behavior() {
    using dep::Behavior;
    Behavior prod{Behavior::PROD};
    Behavior dev{Behavior::DEV};
    Behavior opt{Behavior::OPTIONAL};
    Behavior peer{Behavior::PEER};
    Behavior ws{Behavior::WORKSPACE};
    Behavior optpeer{static_cast<std::uint8_t>(Behavior::OPTIONAL | Behavior::PEER)};
    check(prod.is_prod() && prod.is_required(), "behavior prod");
    check(opt.is_optional() && !opt.is_required(), "behavior optional");
    check(!optpeer.is_optional() && optpeer.is_optional_peer(), "behavior optional-peer");
    check(dev.is_dev() && peer.is_peer() && ws.is_workspace(), "behavior dev/peer/ws");
    // Sort order: workspace < dev < optional < prod < peer.
    check(ws.cmp(dev) < 0, "cmp ws < dev");
    check(dev.cmp(opt) < 0, "cmp dev < opt");
    check(opt.cmp(prod) < 0, "cmp opt < prod");
    check(prod.cmp(peer) < 0, "cmp prod < peer");
    check(prod.cmp(prod) == 0, "cmp prod == prod");
    Behavior toggled{prod.with(Behavior::OPTIONAL, true)};
    check(toggled.is_prod() && toggled.contains(Behavior::OPTIONAL), "behavior with()");
}

void test_github_shorthand() {
    check(dep::is_github_shorthand("user/repo"), "shorthand user/repo");
    check(dep::is_github_shorthand("user/repo#main"), "shorthand user/repo#main");
    check(!dep::is_github_shorthand("./local"), "not shorthand ./local");
    check(!dep::is_github_shorthand("norepo"), "not shorthand norepo (no slash)");
    check(!dep::is_github_shorthand("user/repo/"), "not shorthand trailing slash");
    check(!dep::is_github_shorthand("a@b/c"), "not shorthand @ before hash");
}

void test_resolution() {
    {
        auto r = res::from_text_lockfile("root:");
        check(r && r->tag == res::Tag::Root, "res root:");
    }
    {
        auto r = res::from_text_lockfile("link:../x");
        check(r && r->tag == res::Tag::Symlink && r->symlink == "../x", "res link:");
    }
    {
        auto r = res::from_text_lockfile("workspace:pkgs/a");
        check(r && r->tag == res::Tag::Workspace && r->workspace == "pkgs/a", "res workspace:");
    }
    {
        auto r = res::from_text_lockfile("file:./local");
        check(r && r->tag == res::Tag::Folder && r->folder == "./local", "res file:");
    }
    {
        auto r = res::from_text_lockfile("1.2.3");
        check(r && r->tag == res::Tag::Npm && r->npm.version == "1.2.3", "res npm 1.2.3");
    }
    {
        auto r = res::from_text_lockfile("1.2");  // missing patch -> error
        check(!r.has_value() && r.error() == res::FromTextLockfileError::UnexpectedResolution,
              "res npm 1.2 -> error (no patch)");
    }
    {
        auto r = res::from_text_lockfile("https://example.com/x.tgz");
        check(r && r->tag == res::Tag::RemoteTarball, "res remote tarball");
    }
    {
        auto r = res::from_text_lockfile("user/repo#main");
        check(r && r->tag == res::Tag::Github && r->github.owner == "user" &&
                  r->github.repo == "repo" && r->github.committish == "main",
              "res github");
    }
    // satisfies_dependency_version: npm range against exact resolution.
    {
        auto rr = res::from_text_lockfile("1.2.3");
        auto dv = dep::parse("pkg", "^1.0.0");
        check(rr && dv && rr->satisfies_dependency_version(*dv), "satisfies ^1.0.0 by 1.2.3");
        auto dv2 = dep::parse("pkg", "^2.0.0");
        check(rr && dv2 && !rr->satisfies_dependency_version(*dv2), "not satisfies ^2.0.0 by 1.2.3");
    }
    check(res::tag_name(res::Tag::LocalTarball) == std::optional<std::string_view>{"local_tarball"},
          "res tag_name local_tarball");
    check(res::tag_can_enqueue_install_task(res::Tag::Npm), "res npm can enqueue");
    check(!res::tag_can_enqueue_install_task(res::Tag::Folder), "res folder cannot enqueue");
}

void test_config_and_slice() {
    using mbun::install::config_version_from_int;
    using mbun::install::config_version_from_number;
    check(config_version_from_int(0) == std::optional<ConfigVersion>{ConfigVersion::V0}, "cfg int 0");
    check(config_version_from_int(1) == std::optional<ConfigVersion>{ConfigVersion::V1}, "cfg int 1");
    check(config_version_from_int(99) == std::optional<ConfigVersion>{ConfigVersion::V1},
          "cfg int 99 clamps to CURRENT");
    check(config_version_from_number(1.0) == std::optional<ConfigVersion>{ConfigVersion::V1},
          "cfg num 1.0");
    check(!config_version_from_number(0.5).has_value(), "cfg num 0.5 -> none");

    std::array<int, 5> buf{10, 11, 12, 13, 14};
    ExternalSlice<int> s{1, 3};
    auto got = s.get(std::span<const int>{buf});
    check(got.size() == 3 && got[0] == 11 && got[2] == 13, "external_slice get");
    check(s.begin() == 1 && s.end() == 4, "external_slice begin/end");
    check(s.contains(1) && s.contains(3) && !s.contains(4), "external_slice contains");
    check(ExternalSlice<int>::invalid().is_invalid(), "external_slice invalid");
}

}  // namespace

int main() {
    test_split();
    test_infer();
    test_parse();
    test_behavior();
    test_github_shorthand();
    test_resolution();
    test_config_and_slice();

    std::println("test_dependency: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
