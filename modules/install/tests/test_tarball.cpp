// test_tarball.cpp — mbun.install tarball extract + bin linking unit tests.
//
// Covers the ported pure-logic from src/{extract_tarball,tarball_stream,bin}.cppm:
//   - the ustar/GNU tar container parser over a hand-built 512-byte-block byte
//     buffer (header + data blocks, checksum, octal fields, prefix, long-name),
//   - strip-leading-component + path-traversal rejection + mode handling,
//   - the streaming (chunked) tar reader's WantHeader→WantData→Done machine and
//     its symlink-target safety / created-symlink-traversal defenses,
//   - the `bin` field model + normalization + CVE-2019-16775 escape gate +
//     link-plan construction.
//
// Reference: .mbun/bun-ref/src/install/{extract_tarball,TarballStream,bin}.rs.
import std;
import mbun.core.compress;
import mbun.install.extract_tarball;
import mbun.install.tarball_stream;
import mbun.install.bin;

using namespace mbun::install;

namespace {

int gChecks{0};
int gFailures{0};

void check_true(bool cond, std::string_view what) {
    ++gChecks;
    if (!cond) {
        ++gFailures;
        std::println("  FAIL {}", what);
    }
}

template <class A, class B>
void check_eq(const A& a, const B& b, std::string_view what) {
    ++gChecks;
    if (!(a == b)) {
        ++gFailures;
        if constexpr (std::is_convertible_v<A, std::string_view> &&
                      std::is_convertible_v<B, std::string_view>) {
            std::println("  FAIL {}: got \"{}\", expected \"{}\"", what,
                         std::string_view{a}, std::string_view{b});
        } else {
            std::println("  FAIL {}", what);
        }
    }
}

// ── tar byte-buffer builder ─────────────────────────────────────────────────

constexpr std::size_t BLOCK{512};

void set_str(std::vector<unsigned char>& buf, std::size_t off,
             std::string_view s, std::size_t width) {
    for (std::size_t i{0}; i < s.size() && i < width; ++i) {
        buf[off + i] = static_cast<unsigned char>(s[i]);
    }
}

void set_octal(std::vector<unsigned char>& buf, std::size_t off,
               std::size_t width, std::uint64_t value) {
    // width-1 octal digits, then a NUL terminator (accepted by parser).
    for (std::size_t i{0}; i + 1 < width; ++i) {
        std::size_t pos{off + width - 2 - i};
        buf[pos] = static_cast<unsigned char>('0' + (value & 7));
        value >>= 3;
    }
    buf[off + width - 1] = '\0';
}

// Append one tar entry (header block + NUL-padded data blocks) to `buf`.
void add_entry(std::vector<unsigned char>& buf, std::string_view name,
               char typeflag, std::string_view data, std::uint32_t mode = 0644,
               std::string_view linkname = "") {
    std::size_t base{buf.size()};
    buf.resize(base + BLOCK, 0);
    set_str(buf, base + 0, name, 100);
    set_octal(buf, base + 100, 8, mode);
    set_octal(buf, base + 108, 8, 0);              // uid
    set_octal(buf, base + 116, 8, 0);              // gid
    set_octal(buf, base + 124, 12, data.size());   // size
    set_octal(buf, base + 136, 12, 0);             // mtime
    buf[base + 156] = static_cast<unsigned char>(typeflag);
    set_str(buf, base + 157, linkname, 100);
    set_str(buf, base + 257, "ustar", 6);          // magic (NUL padded)
    buf[base + 263] = '0';                          // version
    buf[base + 264] = '0';
    // checksum: fill field with spaces, sum, write "%06o\0 ".
    for (std::size_t i{148}; i < 156; ++i) buf[base + i] = ' ';
    std::uint32_t sum{0};
    for (std::size_t i{0}; i < BLOCK; ++i) sum += buf[base + i];
    set_octal(buf, base + 148, 7, sum);            // 6 octal digits + NUL
    buf[base + 154] = '\0';
    buf[base + 155] = ' ';

    // data, padded to a block multiple.
    if (!data.empty()) {
        std::size_t dbase{buf.size()};
        std::size_t blocks{(data.size() + BLOCK - 1) / BLOCK};
        buf.resize(dbase + blocks * BLOCK, 0);
        for (std::size_t i{0}; i < data.size(); ++i) {
            buf[dbase + i] = static_cast<unsigned char>(data[i]);
        }
    }
}

void add_terminator(std::vector<unsigned char>& buf) {
    buf.resize(buf.size() + BLOCK * 2, 0);
}

std::span<const std::byte> as_bytes(const std::vector<unsigned char>& v) {
    return {reinterpret_cast<const std::byte*>(v.data()), v.size()};
}

// ── extract_tarball tests ───────────────────────────────────────────────────

void test_parse_tar() {
    std::vector<unsigned char> buf;
    add_entry(buf, "package/index.js", '0', "hello world", 0644);
    add_entry(buf, "package/lib/", '5', "", 0755);
    add_entry(buf, "package/bin/cli.js", '0', "#!/usr/bin/env node\n", 0755);
    add_terminator(buf);

    auto parsed = parse_tar(as_bytes(buf));
    check_true(parsed.has_value(), "parse_tar: ok");
    if (!parsed) return;
    check_eq(parsed->size(), std::size_t{3}, "parse_tar: 3 entries");

    const TarEntry& file{(*parsed)[0]};
    check_eq(file.name, std::string_view{"package/index.js"},
             "parse_tar: file name");
    check_true(file.type == TarEntryType::File, "parse_tar: file type");
    check_eq(file.size, std::uint64_t{11}, "parse_tar: file size");
    std::string content{reinterpret_cast<const char*>(file.data.data()),
                        file.data.size()};
    check_eq(content, std::string_view{"hello world"}, "parse_tar: file data");

    check_true((*parsed)[1].type == TarEntryType::Directory,
               "parse_tar: dir type");
    check_eq((*parsed)[2].name, std::string_view{"package/bin/cli.js"},
             "parse_tar: nested name");
}

void test_long_name() {
    // GNU 'L' record carries a >100-byte name for the following entry.
    std::string longName{"package/"};
    longName.append(200, 'x');
    longName.append("/deep.js");
    std::vector<unsigned char> buf;
    add_entry(buf, "././@LongLink", 'L', longName, 0644);
    add_entry(buf, "package/short", '0', "data", 0644);  // name overridden
    add_terminator(buf);

    auto parsed = parse_tar(as_bytes(buf));
    check_true(parsed.has_value(), "long_name: ok");
    if (!parsed) return;
    check_eq(parsed->size(), std::size_t{1}, "long_name: one real entry");
    check_eq((*parsed)[0].name, longName, "long_name: applied to next entry");
}

void test_strip_and_normalize() {
    check_eq(strip_leading_component("package/index.js"),
             std::string{"index.js"}, "strip: package/");
    check_eq(strip_leading_component("repo-abc123/src/a.js"),
             std::string{"src/a.js"}, "strip: github wrapper");
    check_eq(strip_leading_component("package/"), std::string{},
             "strip: bare wrapper -> empty");

    check_eq(normalize_entry_path("a/./b/../c").value_or("<none>"),
             std::string{"a/c"}, "normalize: collapse . and ..");
    check_true(!normalize_entry_path("../evil").has_value(),
               "normalize: reject leading ..");
    check_true(!normalize_entry_path("/etc/passwd").has_value(),
               "normalize: reject absolute");
    check_true(!normalize_entry_path("C:\\win").has_value(),
               "normalize: reject drive-absolute");
    check_eq(normalize_entry_path("a/b/c").value_or("<none>"),
             std::string{"a/b/c"}, "normalize: plain path");
}

void test_modes_and_gzip() {
    check_eq(file_mode_for(0755u), std::uint32_t{0777u & (0755u | 0666u)},
             "file_mode: mask+or");
    check_eq(file_mode_for(0644u), std::uint32_t{0666u}, "file_mode: 0644");
    check_eq(dir_mode_fix(0400u), std::uint32_t{0500u},
             "dir_mode_fix: readable->listable");
    check_eq(dir_mode_fix(0644u), std::uint32_t{0755u}, "dir_mode_fix: 0644");

    unsigned char gz[]{0x1f, 0x8b, 0x08, 0x00};
    check_true(looks_like_gzip({reinterpret_cast<std::byte*>(gz), 4}),
               "gzip: magic detected");
    unsigned char notgz[]{'u', 's', 't', 'a'};
    check_true(!looks_like_gzip({reinterpret_cast<std::byte*>(notgz), 4}),
               "gzip: non-gzip");

    // extract_tgz: truncated gzip input (magic only) fails to inflate.
    auto r = extract_tgz_to_dir({reinterpret_cast<std::byte*>(gz), 4},
                                std::filesystem::temp_directory_path());
    check_true(!r.has_value() && r.error() == TarError::GzipError,
               "extract_tgz: truncated gzip -> GzipError");
}

// End-to-end: build a tar in memory, gzip it (mbun.core.compress), and unpack
// the resulting .tgz bytes through extract_tgz_to_dir.
void test_extract_tgz_end_to_end() {
    std::vector<unsigned char> tar;
    add_entry(tar, "package/index.js", '0', "module.exports = 42;\n", 0644);
    add_entry(tar, "package/lib/util.js", '0', "// util\n", 0644);
    add_terminator(tar);

    // Real deflate path (level 6).
    std::vector<std::uint8_t> tgz{mbun::core::compress::gzip_compress(
        {tar.data(), tar.size()}, 6)};
    check_true(tgz.size() >= 2 && tgz[0] == 0x1f && tgz[1] == 0x8b,
               "tgz: output is gzip");

    std::error_code ec;
    auto dir = std::filesystem::temp_directory_path()
               / std::filesystem::path{"mbun_tgz_test"};
    std::filesystem::remove_all(dir, ec);

    auto r = extract_tgz_to_dir(
        {reinterpret_cast<const std::byte*>(tgz.data()), tgz.size()}, dir);
    check_true(r.has_value(), "tgz: extract ok");
    if (r) {
        check_eq(r->filesWritten, std::size_t{2}, "tgz: 2 files written");
        check_true(std::filesystem::exists(dir / "index.js"),
                   "tgz: index.js present (prefix stripped)");
        check_true(std::filesystem::exists(dir / "lib" / "util.js"),
                   "tgz: lib/util.js present");
        std::ifstream in{dir / "index.js", std::ios::binary};
        std::string content{std::istreambuf_iterator<char>{in}, {}};
        check_eq(content, std::string_view{"module.exports = 42;\n"},
                 "tgz: file content round-trips");
    }
    std::filesystem::remove_all(dir, ec);

    // Stored-block path (level 0) exercises the non-compressed deflate mode.
    std::vector<std::uint8_t> tgz0{mbun::core::compress::gzip_compress(
        {tar.data(), tar.size()}, 0)};
    auto dir0 = std::filesystem::temp_directory_path()
                / std::filesystem::path{"mbun_tgz_test0"};
    std::filesystem::remove_all(dir0, ec);
    auto r0 = extract_tgz_to_dir(
        {reinterpret_cast<const std::byte*>(tgz0.data()), tgz0.size()}, dir0);
    check_true(r0.has_value() && r0->filesWritten == 2,
               "tgz: stored-mode gzip extract ok");
    std::filesystem::remove_all(dir0, ec);

    // Corrupt body: flip a byte inside the deflate stream -> GzipError.
    std::vector<std::uint8_t> bad{tgz};
    if (bad.size() > 14) {
        bad[12] ^= 0xFF;
        auto rb = extract_tgz_to_dir(
            {reinterpret_cast<const std::byte*>(bad.data()), bad.size()},
            std::filesystem::temp_directory_path());
        check_true(!rb.has_value() && rb.error() == TarError::GzipError,
                   "tgz: corrupt gzip -> GzipError");
    }
}

void test_numeric() {
    unsigned char oct[]{'0', '0', '0', '0', '6', '4', '4', '\0'};
    auto v = parse_tar_numeric({reinterpret_cast<std::byte*>(oct), 8});
    check_true(v.has_value() && *v == 0644u, "numeric: octal");
    // GNU base-256 (high bit): 0x80 0x00 0x00 0x01 == 1
    unsigned char b256[]{0x80, 0x00, 0x00, 0x01};
    auto v2 = parse_tar_numeric({reinterpret_cast<std::byte*>(b256), 4});
    check_true(v2.has_value() && *v2 == 1u, "numeric: base-256");
}

void test_extract_to_dir() {
    std::vector<unsigned char> buf;
    add_entry(buf, "package/a.txt", '0', "AAA", 0644);
    add_entry(buf, "package/sub/b.txt", '0', "BBB", 0644);
    add_terminator(buf);

    std::error_code ec;
    auto dir = std::filesystem::temp_directory_path()
               / std::filesystem::path{"mbun_tar_test"};
    std::filesystem::remove_all(dir, ec);

    auto r = extract_tar_to_dir(as_bytes(buf), dir);  // depthToSkip=1 default
    check_true(r.has_value(), "extract_to_dir: ok");
    if (r) {
        check_eq(r->filesWritten, std::size_t{2}, "extract_to_dir: 2 files");
        check_true(std::filesystem::exists(dir / "a.txt"),
                   "extract_to_dir: a.txt present (prefix stripped)");
        check_true(std::filesystem::exists(dir / "sub" / "b.txt"),
                   "extract_to_dir: sub/b.txt present");
    }
    std::filesystem::remove_all(dir, ec);
}

// ── tarball_stream tests ────────────────────────────────────────────────────

void test_streaming() {
    std::vector<unsigned char> buf;
    add_entry(buf, "package/one.js", '0', "first", 0644);
    add_entry(buf, "package/two.js", '0', "second", 0644);
    add_terminator(buf);

    StreamingTarReader reader;
    // Feed in two chunks split mid-stream to exercise the resume path.
    std::size_t split{BLOCK + BLOCK};  // header + data block of first entry
    reader.feed({reinterpret_cast<const std::byte*>(buf.data()), split});

    auto e1 = reader.next();
    check_true(e1.has_value() && e1->has_value(), "stream: first entry ready");
    if (e1 && *e1) {
        check_eq((*e1)->name, std::string_view{"package/one.js"},
                 "stream: first name");
    }
    // Second entry not yet available.
    auto pending = reader.next();
    check_true(!pending.has_value(), "stream: yields when out of data");

    reader.feed({reinterpret_cast<const std::byte*>(buf.data() + split),
                 buf.size() - split});
    reader.close();
    auto e2 = reader.next();
    check_true(e2.has_value() && e2->has_value(), "stream: second entry ready");
    if (e2 && *e2) {
        check_eq((*e2)->name, std::string_view{"package/two.js"},
                 "stream: second name");
    }
    auto eof = reader.next();
    check_true(!eof.has_value() && reader.done(), "stream: reaches Done");
}

void test_symlink_safety() {
    check_true(is_symlink_target_safe("node_modules/x/link", "./file"),
               "symlink: relative target safe");
    check_true(is_symlink_target_safe("a/b/link", "../sibling"),
               "symlink: single .. within root safe");
    check_true(!is_symlink_target_safe("link", "/etc/passwd"),
               "symlink: absolute target unsafe");
    check_true(!is_symlink_target_safe("link", "../../etc/passwd"),
               "symlink: climbs above root unsafe");
    check_true(!is_symlink_target_safe("a/link", "b/../../../etc"),
               "symlink: .. after named component unsafe");

    std::vector<std::string> created{"pkg/evil"};
    check_true(path_traverses_created_symlink("pkg/evil/inner.js", created),
               "traverse: descends through created symlink");
    check_true(!path_traverses_created_symlink("pkg/good.js", created),
               "traverse: unrelated path ok");
    check_true(!path_traverses_created_symlink("pkg/evilish.js", created),
               "traverse: prefix-but-not-component ok");

    check_eq(std::string{tokenize_rest_after_first("package/index.js")},
             std::string{"index.js"}, "tokenize: rest after first");
    check_true(npm_mode_should_skip(TarEntryType::Symlink),
               "npm_mode: skip symlink");
    check_true(!npm_mode_should_skip(TarEntryType::File),
               "npm_mode: keep file");
}

// ── bin tests ───────────────────────────────────────────────────────────────

void test_bin_helpers() {
    check_true(is_safe_install_folder_name("foo"), "safe: plain");
    check_true(!is_safe_install_folder_name(".."), "safe: reject ..");
    check_true(!is_safe_install_folder_name("a:b"), "safe: reject colon");
    check_true(!is_safe_install_folder_name(""), "safe: reject empty");

    check_eq(unscoped_package_name("@scope/pkg"), std::string_view{"pkg"},
             "unscoped: scoped");
    check_eq(unscoped_package_name("plain"), std::string_view{"plain"},
             "unscoped: plain");

    check_eq(normalized_bin_name("./bin/cli.js"), std::string_view{"cli.js"},
             "normalized_bin_name: basename");
    check_eq(normalized_bin_name("a/b/../evil"), std::string_view{"evil"},
             "normalized_bin_name: takes last segment");
    check_eq(normalized_bin_name("weird:name"), std::string_view{"name"},
             "normalized_bin_name: after colon");
    check_true(normalized_bin_name("..").empty(),
               "normalized_bin_name: unsafe -> empty");
}

void test_bin_escape() {
    check_true(!bin_target_escapes_package_dir("./cli.js"),
               "escape: relative ok");
    check_true(!bin_target_escapes_package_dir("bin/cli.js"),
               "escape: nested ok");
    check_true(bin_target_escapes_package_dir("/usr/bin/evil"),
               "escape: absolute");
    check_true(bin_target_escapes_package_dir("../../etc/passwd"),
               "escape: traversal");
    check_true(bin_target_escapes_package_dir("C:evil"),
               "escape: drive-relative first component");
    check_true(!bin_target_escapes_package_dir("a/../b"),
               "escape: net-zero traversal ok");

    check_true(!bin_target_needs_resolved_containment_check("cli.js"),
               "containment: single component no check");
    check_true(bin_target_needs_resolved_containment_check("bin/cli.js"),
               "containment: nested needs check");
    check_true(bin_target_needs_resolved_containment_check("./cli.js"),
               "containment: dot-first needs check");
}

void test_bin_parse() {
    check_true(bin_parse_string("./cli.js").tag == BinTag::File,
               "parse: string -> File");
    check_true(bin_parse_string("").tag == BinTag::None,
               "parse: empty string -> None");
    check_true(bin_parse_directories("./bin").tag == BinTag::Dir,
               "parse: directories -> Dir");

    std::pair<std::string_view, std::string_view> one[]{{"babel", "./cli.js"}};
    Bin nf{bin_parse_object(one)};
    check_true(nf.tag == BinTag::NamedFile, "parse: 1-pair -> NamedFile");
    check_eq(nf.namedFile[0], std::string_view{"babel"}, "parse: named key");
    check_eq(nf.namedFile[1], std::string_view{"./cli.js"},
             "parse: named value");

    std::pair<std::string_view, std::string_view> many[]{
        {"a", "./a.js"}, {"b", "./b.js"}};
    Bin m{bin_parse_object(many)};
    check_true(m.tag == BinTag::Map, "parse: 2-pair -> Map");
    check_eq(m.map.size(), std::size_t{4}, "parse: map flat size");

    // names iterator
    auto namesFile = bin_names(bin_parse_string("./bin/tool.js"), "my-pkg");
    check_eq(namesFile.size(), std::size_t{1}, "names: file one name");
    check_eq(namesFile[0], std::string_view{"my-pkg"},
             "names: File named after package");
    auto namesMap = bin_names(m, "my-pkg");
    check_eq(namesMap.size(), std::size_t{2}, "names: map two names");
    check_eq(namesMap[0], std::string_view{"a"}, "names: map first");
}

void test_link_plan() {
    // File bin: dest = <nm>/.bin/<unscoped pkg>, target under package dir.
    Bin file{bin_parse_string("./bin/cli.js")};
    LinkPlan plan{plan_links(file, "/proj/node_modules", "/proj/node_modules",
                             "@scope/tool", "/global/bin", false)};
    check_eq(plan.actions.size(), std::size_t{1}, "plan: file one action");
    check_true(plan.error == LinkPlanError::None, "plan: no error");
    if (!plan.actions.empty()) {
        check_eq(plan.actions[0].dest,
                 std::string{"/proj/node_modules/.bin/tool"},
                 "plan: dest = .bin/<unscoped>");
        check_eq(plan.actions[0].target,
                 std::string{"/proj/node_modules/@scope/tool/bin/cli.js"},
                 "plan: target under package dir");
        check_true(plan.actions[0].needsResolvedContainmentCheck,
                   "plan: nested target needs containment check");
    }

    // Escaping target produces no action.
    Bin evil{bin_parse_string("../../../etc/passwd")};
    LinkPlan ep{plan_links(evil, "/proj/node_modules", "/proj/node_modules",
                           "tool", "/g", false)};
    check_eq(ep.actions.size(), std::size_t{0}, "plan: escaping target skipped");

    // Global bin path.
    LinkPlan gp{plan_links(file, "/proj/node_modules", "/proj/node_modules",
                           "tool", "/global/bin", true)};
    if (!gp.actions.empty()) {
        check_eq(gp.actions[0].dest, std::string{"/global/bin/tool"},
                 "plan: global dest uses global bin path");
    }

    // Dir tag defers to a directory listing seam.
    Bin dir{bin_parse_directories("./bin")};
    LinkPlan dp{plan_links(dir, "/proj/node_modules", "/proj/node_modules",
                           "tool", "/g", false)};
    check_true(dp.needsDirListing, "plan: dir tag needs listing seam");
    check_eq(dp.dirTarget, std::string{"/proj/node_modules/tool/bin"},
             "plan: dir target path");
}

}  // namespace

int main() {
    test_parse_tar();
    test_long_name();
    test_strip_and_normalize();
    test_modes_and_gzip();
    test_extract_tgz_end_to_end();
    test_numeric();
    test_extract_to_dir();
    test_streaming();
    test_symlink_safety();
    test_bin_helpers();
    test_bin_escape();
    test_bin_parse();
    test_link_plan();

    std::println("test_tarball: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
