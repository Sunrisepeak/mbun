// test_lockfile.cpp — mbun.install.lockfile (binary bun.lockb model + format).
//
// Exercises the fuller lockfile port (src/lockfile/*.cppm): the flat arena model
// and the bun.lockb binary read/write. Format/layout is pinned to bun's real
// source (see the `ref:` annotations in the modules):
//   - .mbun/bun-ref/src/install/lockfile/bun.lockb.rs   save/load, section tags,
//                                                       HEADER_BYTES, VERSION
//   - .mbun/bun-ref/src/install/lockfile/Buffers.rs     write_array/read_array,
//                                                       Buffers::save/load, Aligner
//   - .mbun/bun-ref/src/install/lockfile/Package.rs     serializer::save/load,
//                                                       PackageField column order
//   - .mbun/bun-ref/src/install/lockfile/Tree.rs        Tree <-> 20-byte External
//   - .mbun/bun-ref/src/install/padding_checker.rs      on-disk element sizes
//   - .mbun/bun-ref/src/install/default-trusted-dependencies.txt
//
// This suite asserts the format's own invariants: a write->read->compare
// round-trip over a lockfile exercising every section, plus the
// header/version/tag/alignment contracts.
//
// A round-trip only proves `save` and `load` agree with each other — it cannot
// catch both of them disagreeing with bun, which is exactly how the v2 stride
// bug survived here. Reading real bun-generated lockfiles is
// test_lockfile_real.cpp; keep new format coverage there unless it is genuinely
// about our own writer.
import std;
import mbun.install.lockfile;

namespace {

using namespace mbun::install::lockfile;

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
        std::println("  FAIL {}", what);
    }
}

// Build a small but representative lockfile touching every serialized section.
Lockfile make_sample() {
    Lockfile lf;
    lf.format = FormatVersion::current();
    for (std::size_t i{0}; i < lf.metaHash.size(); ++i) {
        lf.metaHash[i] = static_cast<std::uint8_t>(i * 3 + 1);
    }

    // Two packages (root + one npm dep).
    lf.packages.resize(2);
    lf.packages.nameHash[0] = 0xAABB'CCDD'1122'3344ull;
    lf.packages.nameHash[1] = 0x0102'0304'0506'0708ull;
    lf.packages.name[1].bytes = {'l', 'o', 'd', 'a', 's', 'h', 0, 0};
    lf.packages.dependencies[0] = Slice{0, 1};
    lf.packages.resolutions[0] = Slice{0, 1};
    lf.packages.resolution[1].bytes[0] = 1;  // Npm tag (valid discriminant)
    lf.packages.meta[1].bytes[0] = 7;
    lf.packages.bin[1].bytes[0] = 2;

    // Buffers: one tree, one hoisted dep, one resolution, one dependency, one
    // extern string, some string bytes.
    lf.buffers.trees.push_back(Tree{0, ROOT_DEP_ID, INVALID_TREE_ID, Slice{0, 1}});
    lf.buffers.hoistedDependencies.push_back(0);
    lf.buffers.resolutions.push_back(1);
    DependencyExternal dep{};
    dep.bytes[0] = 42;
    dep.bytes[25] = 99;
    lf.buffers.dependencies.push_back(dep);
    SemverExternalString ext{};
    ext.hash = 0xDEAD'BEEF'F00D'BA11ull;
    ext.value.bytes = {'x', 'y', 'z', 0, 0, 0, 0, 0};
    lf.buffers.externStrings.push_back(ext);
    lf.buffers.stringBytes = {'l', 'o', 'd', 'a', 's', 'h', '@', '1'};

    // Workspace versions/paths.
    lf.workspaceVersionKeys.push_back(0x1111'2222'3333'4444ull);
    SemverVersion ver{};
    ver.bytes[0] = 1;
    ver.bytes[8] = 2;
    lf.workspaceVersionVals.push_back(ver);
    lf.workspacePathKeys.push_back(0x1111'2222'3333'4444ull);
    SemverString wpath{};
    wpath.bytes = {'p', 'k', 'g', 's', '/', 'a', 0, 0};
    lf.workspacePathVals.push_back(wpath);

    // Trusted dependencies (non-empty).
    lf.trustedDependencies = std::vector<TruncatedPackageNameHash>{0xCAFEu, 0xF00Du};

    // Overrides.
    lf.overrideKeys.push_back(0x5555'6666'7777'8888ull);
    DependencyExternal ovr{};
    ovr.bytes[1] = 5;
    lf.overrideVals.push_back(ovr);

    // Patched dependencies.
    lf.patchedKeys.push_back(0x9999'AAAA'BBBB'CCCCull);
    PatchedDepExternal pd{};
    pd.path.bytes = {'p', '.', 'p', 'a', 't', 'c', 'h', 0};
    pd.patchfileHashIsNull = 0;
    pd.patchfileHash = 0x1234'5678'9ABC'DEF0ull;
    lf.patchedVals.push_back(pd);

    // Catalogs: a default entry + one named group.
    SemverString cn{};
    cn.bytes = {'r', 'e', 'a', 'c', 't', 0, 0, 0};
    lf.catalogDefaultNames.push_back(cn);
    lf.catalogDefaultDeps.push_back(DependencyExternal{});
    SemverString gname{};
    gname.bytes = {'d', 'e', 'v', 0, 0, 0, 0, 0};
    lf.catalogGroupNames.push_back(gname);
    CatalogGroup grp;
    grp.depNames.push_back(cn);
    grp.deps.push_back(DependencyExternal{});
    lf.catalogGroups.push_back(std::move(grp));

    lf.savedConfigVersion = 1;
    return lf;
}

void test_header_and_version() {
    Lockfile lf{make_sample()};
    std::vector<std::uint8_t> bytes{save(lf, 1)};
    check_true(is_binary_lockfile(std::as_bytes(std::span{bytes})),
               "saved buffer has the bun.lockb magic header");
    check_eq(VERSION, std::string_view{"bun-lockfile-format-v0\n"}, "VERSION string matches bun");
    // Not-a-lockfile input is rejected.
    std::array<std::uint8_t, 4> junk{'n', 'o', 'p', 'e'};
    check_true(!is_binary_lockfile(std::as_bytes(std::span{junk})),
               "short/garbage input is not a binary lockfile");
    auto bad{load(std::span{junk})};
    check_true(!bad.has_value(), "loading garbage fails");
}

void test_array_alignment() {
    // Every array payload's [start,end) must be 8-byte aligned and delimited;
    // round-trip a lockfile and confirm the total-buffer-size backpatch lands.
    Lockfile lf{make_sample()};
    std::vector<std::uint8_t> bytes{save(lf, 1)};
    // The u64 at offset (header + u32 format + 32-byte metahash) is total size.
    std::size_t tbsOff{std::string_view{"#!/usr/bin/env bun\nbun-lockfile-format-v0\n"}.size() + 4 +
                       32};
    std::uint64_t tbs{0};
    for (std::size_t i{0}; i < 8; ++i) {
        tbs |= static_cast<std::uint64_t>(bytes[tbsOff + i]) << (8 * i);
    }
    check_true(tbs > 0 && tbs <= bytes.size(), "total-buffer-size backpatch is in range");
    // 144 trailing alignment bytes are appended after totalSize.
    check_eq(bytes.size(), static_cast<std::size_t>(tbs) + 144,
             "trailer is exactly 144 alignment bytes past totalSize");
}

void test_round_trip() {
    Lockfile in{make_sample()};
    std::vector<std::uint8_t> bytes{save(in, 1)};
    auto outR{load(std::span{bytes})};
    check_true(outR.has_value(), "round-trip load succeeds");
    if (!outR) {
        return;
    }
    const Lockfile& out{*outR};

    check_eq(out.format.value, in.format.value, "format version preserved");
    check_true(out.metaHash == in.metaHash, "meta hash preserved");

    // Packages (SoA columns).
    check_eq(out.packages.len(), in.packages.len(), "package count preserved");
    check_eq(out.packages.nameHash[0], in.packages.nameHash[0], "package[0] name hash");
    check_eq(out.packages.nameHash[1], in.packages.nameHash[1], "package[1] name hash");
    check_true(out.packages.name[1].bytes == in.packages.name[1].bytes, "package[1] name string");
    check_eq(out.packages.dependencies[0].len, in.packages.dependencies[0].len,
             "package[0] dependency slice len");
    check_eq(out.packages.resolution[1].bytes[0], std::uint8_t{1}, "package[1] resolution tag");

    // Buffers.
    check_eq(out.buffers.trees.size(), in.buffers.trees.size(), "tree count");
    check_eq(out.buffers.trees[0].dependencyId, ROOT_DEP_ID, "tree[0] dependency id (ROOT_DEP_ID)");
    check_eq(out.buffers.trees[0].dependencies.len, std::uint32_t{1}, "tree[0] dep slice len");
    check_eq(out.buffers.hoistedDependencies.size(), std::size_t{1}, "hoisted deps count");
    check_eq(out.buffers.resolutions[0], std::uint32_t{1}, "resolutions[0]");
    check_true(out.buffers.dependencies[0].bytes == in.buffers.dependencies[0].bytes,
               "dependency external bytes");
    check_eq(out.buffers.externStrings[0].hash, in.buffers.externStrings[0].hash,
             "extern string hash");
    check_true(out.buffers.stringBytes == in.buffers.stringBytes, "string bytes buffer");

    // Workspace versions/paths.
    check_eq(out.workspaceVersionKeys.size(), std::size_t{1}, "workspace version count");
    check_eq(out.workspaceVersionKeys[0], in.workspaceVersionKeys[0], "workspace version key");
    check_true(out.workspaceVersionVals[0].bytes == in.workspaceVersionVals[0].bytes,
               "workspace version value");
    check_true(out.workspacePathVals[0].bytes == in.workspacePathVals[0].bytes,
               "workspace path value");

    // Trusted / overrides / patched.
    check_true(out.trustedDependencies.has_value() && out.trustedDependencies->size() == 2,
               "trusted dependencies preserved");
    check_eq((*out.trustedDependencies)[0], (*in.trustedDependencies)[0], "trusted hash[0]");
    check_eq(out.overrideKeys[0], in.overrideKeys[0], "override key");
    check_eq(out.patchedKeys[0], in.patchedKeys[0], "patched key");
    check_eq(out.patchedVals[0].patchfileHash, in.patchedVals[0].patchfileHash, "patched hash");
    check_true(out.patchedVals[0].path.bytes == in.patchedVals[0].path.bytes, "patched path");

    // Catalogs.
    check_eq(out.catalogDefaultNames.size(), std::size_t{1}, "catalog default entry count");
    check_eq(out.catalogGroups.size(), std::size_t{1}, "catalog group count");
    check_true(out.catalogGroupNames[0].bytes == in.catalogGroupNames[0].bytes,
               "catalog group name");
    check_eq(out.catalogGroups[0].depNames.size(), std::size_t{1}, "catalog group dep count");

    // Config version.
    check_true(out.savedConfigVersion.has_value() && *out.savedConfigVersion == 1,
               "saved config version");
}

void test_empty_trusted_distinct() {
    // present-but-empty trustedDependencies must round-trip as Some([]), not None.
    Lockfile lf{make_sample()};
    lf.trustedDependencies = std::vector<TruncatedPackageNameHash>{};
    std::vector<std::uint8_t> emptyBytes{save(lf, 1)};
    auto outR{load(std::span{emptyBytes})};
    check_true(outR.has_value(), "empty-trusted round-trip loads");
    if (outR) {
        check_true(outR->trustedDependencies.has_value() && outR->trustedDependencies->empty(),
                   "empty trusted set stays Some([]) (distinct from absent)");
    }
    // absent trustedDependencies stays absent.
    Lockfile lf2{make_sample()};
    lf2.trustedDependencies = std::nullopt;
    std::vector<std::uint8_t> absentBytes{save(lf2, 1)};
    auto out2{load(std::span{absentBytes})};
    check_true(out2.has_value() && !out2->trustedDependencies.has_value(),
               "absent trusted set stays None");
}

void test_trusted_list() {
    check_true(DEFAULT_TRUSTED_DEPENDENCIES.size() == 367,
               "default trusted dependency count matches the upstream .txt");
    check_true(is_default_trusted("@anthropic-ai/claude-code"),
               "known default-trusted name is recognized");
    check_true(!is_default_trusted("definitely-not-a-real-package-xyz"),
               "unknown name is not default-trusted");
}

}  // namespace

int main() {
    test_header_and_version();
    test_array_alignment();
    test_round_trip();
    test_empty_trusted_distinct();
    test_trusted_list();

    std::println("test_lockfile: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
