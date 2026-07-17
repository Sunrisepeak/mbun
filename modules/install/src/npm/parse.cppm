// npm/parse.cppm — mbun.install.npm.parse
//
// Parse an npm registry packument (abbreviated or full metadata) into a
// PackageManifest — port of `PackageManifest::parse` in
//   .mbun/bun-ref/src/install/npm.rs
// bun's parse walks the JSON twice (a counting pass to size flat arenas, then a
// build pass writing ExternalString indices) because it targets a zero-copy
// on-disk cache. That two-pass arena machinery is an optimization for its
// serialization ABI (DEFERRED); the *semantics* we reproduce faithfully:
//   - iterate `versions` in document order, split release vs prerelease
//   - per version: dist (tarball/integrity/shasum/fileCount/unpackedSize),
//     bin (string | map | directories.bin), os/cpu/libc, hasInstallScript,
//     dependencies / optionalDependencies / peerDependencies (+ meta-only
//     optional peers synthesised as "*"), bundleDependencies, engines
//   - dist-tags map, `time` publish timestamps (extended manifest)
//   - sort releases + prereleases ascending by semver order
export module mbun.install.npm.parse;

import std;
import mbun.semver;
import mbun.install.npm.json;
import mbun.install.npm.manifest;
import mbun.install.npm.negatable;

namespace mbun::install::npm {

namespace detail {

// Collect a JSON string array (or bare string) into tokens for os/cpu/libc.
inline std::vector<std::string_view> collect_tokens(const json::Value& v) {
    std::vector<std::string_view> out;
    if (v.is_array()) {
        for (const auto& item : v.items) {
            if (auto s{item->as_str()}) {
                out.push_back(*s);
            }
        }
    } else if (auto s{v.as_str()}) {
        out.push_back(*s);
    }
    return out;
}

// Read a name→range dependency object into a DepMap (document order).
inline DepMap read_dep_map(const json::Value* obj) {
    DepMap out;
    if (obj == nullptr || !obj->is_object()) {
        return out;
    }
    out.reserve(obj->members.size());
    for (const auto& m : obj->members) {
        std::string_view val{};
        if (auto s{m.value->as_str()}) {
            val = *s;
        }
        out.emplace_back(std::string{m.key}, std::string{val});
    }
    return out;
}

// Parse the `bin` / `directories.bin` fields for one version.
inline Bin parse_bin(const json::Value& versionObj) {
    Bin bin{};
    if (const json::Value* b{versionObj.get("bin")}) {
        if (b->is_object()) {
            const auto& props{b->members};
            if (props.size() == 1) {
                std::string_view path{};
                if (auto s{props[0].value->as_str()}) {
                    path = *s;
                }
                bin.kind = BinKind::NamedFile;
                bin.entries = {std::string{props[0].key}, std::string{path}};
                return bin;
            }
            if (props.size() > 1) {
                bin.kind = BinKind::Map;
                bin.entries.reserve(props.size() * 2);
                for (const auto& p : props) {
                    std::string_view path{};
                    if (auto s{p.value->as_str()}) {
                        path = *s;
                    }
                    bin.entries.push_back(std::string{p.key});
                    bin.entries.push_back(std::string{path});
                }
                return bin;
            }
            // empty object => fall through to directories.bin
        } else if (auto s{b->as_str()}) {
            if (!s->empty()) {
                bin.kind = BinKind::File;
                bin.entries = {std::string{*s}};
                return bin;
            }
        }
    }
    if (const json::Value* dirs{versionObj.get("directories")}) {
        if (const json::Value* db{dirs->get("bin")}) {
            if (auto s{db->as_str()}) {
                if (!s->empty()) {
                    bin.kind = BinKind::Dir;
                    bin.entries = {std::string{*s}};
                    return bin;
                }
            }
        }
    }
    return bin;
}

// npm ES5 date -> epoch ms (best-effort). bun uses bun_core::wtf::parse_es5_date;
// packument `time` values are ISO-8601 ("2021-05-04T00:00:00.000Z"). We parse
// that shape; anything else yields nullopt (timestamp stays 0 / unknown).
inline std::optional<double> parse_iso_ms(std::string_view s) {
    // YYYY-MM-DDTHH:MM:SS(.mmm)?Z
    auto num{[&](std::size_t off, std::size_t len) -> std::optional<long long> {
        if (off + len > s.size()) {
            return std::nullopt;
        }
        long long v{0};
        auto sub{s.substr(off, len)};
        auto res{std::from_chars(sub.data(), sub.data() + sub.size(), v)};
        if (res.ec != std::errc{}) {
            return std::nullopt;
        }
        return v;
    }};
    if (s.size() < 19 || s[4] != '-' || s[7] != '-' || (s[10] != 'T' && s[10] != ' ')) {
        return std::nullopt;
    }
    auto year{num(0, 4)};
    auto mon{num(5, 2)};
    auto day{num(8, 2)};
    auto hour{num(11, 2)};
    auto min{num(14, 2)};
    auto sec{num(17, 2)};
    if (!year || !mon || !day || !hour || !min || !sec) {
        return std::nullopt;
    }
    long long ms{0};
    if (s.size() >= 23 && s[19] == '.') {
        if (auto m{num(20, 3)}) {
            ms = *m;
        }
    }
    // days since epoch via civil calendar (Howard Hinnant's algorithm).
    long long y{*year};
    long long m{*mon};
    long long d{*day};
    y -= (m <= 2) ? 1 : 0;
    long long era{(y >= 0 ? y : y - 399) / 400};
    long long yoe{y - era * 400};
    long long doy{(153 * ((m > 2) ? (m - 3) : (m + 9)) + 2) / 5 + d - 1};
    long long doe{yoe * 365 + yoe / 4 - yoe / 100 + doy};
    long long days{era * 146097 + doe - 719468};
    long long secs{days * 86400 + *hour * 3600 + *min * 60 + *sec};
    return static_cast<double>(secs * 1000 + ms);
}

}  // namespace detail

// Parse a packument. Returns nullopt on malformed JSON or an `{"error": ...}`
// body (mirrors bun's parse returning Ok(None)). `sourceKeepAlive` note: the
// returned manifest owns all its strings (std::string), so `jsonSource` need
// not outlive it.
export std::optional<PackageManifest> parse_manifest(std::string_view jsonSource,
                                                     std::string_view expectedName,
                                                     std::string_view lastModified = {},
                                                     std::string_view etag = {},
                                                     std::uint32_t publicMaxAge = 0,
                                                     bool isExtendedManifest = false) {
    auto doc{json::parse(jsonSource)};
    if (!doc || !doc->root || !doc->root->is_object()) {
        return std::nullopt;
    }
    const json::Value& root{*doc->root};

    if (const json::Value* err{root.get("error")}) {
        if (err->as_str()) {
            return std::nullopt;  // npm error body
        }
    }

    PackageManifest result{};
    // Use expectedName (custom registries may name the package differently).
    result.name = std::string{expectedName};
    result.last_modified = std::string{lastModified};
    result.etag = std::string{etag};
    result.public_max_age = publicMaxAge;
    result.has_extended_manifest = isExtendedManifest;

    if (const json::Value* mod{root.get("modified")}) {
        if (auto s{mod->as_str()}) {
            result.modified = std::string{*s};
        }
    }

    const json::Value* timeObj{root.get("time")};

    // ── versions ────────────────────────────────────────────────────────────
    if (const json::Value* versions{root.get("versions")}; versions && versions->is_object()) {
        for (const auto& vm : versions->members) {
            std::string_view versionName{vm.key};
            const json::Value& versionObj{*vm.value};
            if (!versionObj.is_object()) {
                continue;
            }

            PackageVersion pv{};
            pv.version = std::string{versionName};

            // os / cpu / libc
            if (const json::Value* cpu{versionObj.get("cpu")}) {
                auto toks{detail::collect_tokens(*cpu)};
                pv.cpu = negatable_from_tokens<Architecture>(toks);
            }
            if (const json::Value* os{versionObj.get("os")}) {
                auto toks{detail::collect_tokens(*os)};
                pv.os = negatable_from_tokens<OperatingSystem>(toks);
            }
            if (const json::Value* libc{versionObj.get("libc")}) {
                auto toks{detail::collect_tokens(*libc)};
                pv.libc = negatable_from_tokens<Libc>(toks);
            }

            if (const json::Value* his{versionObj.get("hasInstallScript")}) {
                if (auto b{his->as_bool()}) {
                    pv.has_install_script = *b;
                }
            }

            pv.bin = detail::parse_bin(versionObj);

            // dist: tarball / integrity / shasum / fileCount / unpackedSize
            if (const json::Value* dist{versionObj.get("dist")}; dist && dist->is_object()) {
                if (const json::Value* tb{dist->get("tarball")}) {
                    if (auto s{tb->as_str()}) {
                        if (!s->empty()) {
                            pv.tarball_url = std::string{*s};
                        }
                    }
                }
                if (const json::Value* fc{dist->get("fileCount")}) {
                    if (auto n{fc->as_number()}) {
                        pv.file_count = static_cast<std::uint32_t>(*n);
                    }
                }
                if (const json::Value* us{dist->get("unpackedSize")}) {
                    if (auto n{us->as_number()}) {
                        pv.unpacked_size = static_cast<std::uint32_t>(*n);
                    }
                }
                bool haveIntegrity{false};
                if (const json::Value* intg{dist->get("integrity")}) {
                    if (auto s{intg->as_str()}) {
                        pv.integrity = Integrity::parse(*s);
                        haveIntegrity = pv.integrity.is_supported();
                    }
                }
                if (!haveIntegrity) {
                    if (const json::Value* sh{dist->get("shasum")}) {
                        if (auto s{sh->as_str()}) {
                            pv.integrity = Integrity::parse_sha_sum(*s);
                        }
                    }
                }
            }

            // bundleDependencies / bundledDependencies
            if (const json::Value* bd{versionObj.get("bundleDependencies")}) {
                if (auto b{bd->as_bool()}) {
                    pv.bundle_all_deps = *b;
                } else if (bd->is_array()) {
                    for (const auto& item : bd->items) {
                        if (auto s{item->as_str()}) {
                            pv.bundled_dependencies.emplace_back(*s);
                        }
                    }
                }
            } else if (const json::Value* bd2{versionObj.get("bundledDependencies")}) {
                if (auto b{bd2->as_bool()}) {
                    pv.bundle_all_deps = *b;
                } else if (bd2->is_array()) {
                    for (const auto& item : bd2->items) {
                        if (auto s{item->as_str()}) {
                            pv.bundled_dependencies.emplace_back(*s);
                        }
                    }
                }
            }

            pv.dependencies = detail::read_dep_map(versionObj.get("dependencies"));
            pv.optional_dependencies = detail::read_dep_map(versionObj.get("optionalDependencies"));
            pv.engines = detail::read_dep_map(versionObj.get("engines"));

            // peerDependencies + peerDependenciesMeta (optional peers hoisted to
            // the front; meta-only optional peers synthesised as "*").
            {
                pv.peer_dependencies = detail::read_dep_map(versionObj.get("peerDependencies"));
                const json::Value* meta{versionObj.get("peerDependenciesMeta")};
                // Collect the set of optional peer names.
                std::vector<std::string_view> optionalNames;
                if (meta != nullptr && meta->is_object()) {
                    for (const auto& mp : meta->members) {
                        if (const json::Value* opt{mp.value->get("optional")}) {
                            if (auto b{opt->as_bool()}; b && *b) {
                                optionalNames.push_back(mp.key);
                            }
                        }
                    }
                    // Synthesise meta-only optional peers as "*".
                    for (std::string_view name : optionalNames) {
                        bool present{false};
                        for (const auto& kv : pv.peer_dependencies) {
                            if (kv.first == name) {
                                present = true;
                                break;
                            }
                        }
                        if (!present) {
                            pv.peer_dependencies.emplace_back(std::string{name}, "*");
                        }
                    }
                }
                // Move optional peers to the front, tracking the split point.
                if (!optionalNames.empty()) {
                    auto isOptional{[&](std::string_view n) {
                        for (std::string_view o : optionalNames) {
                            if (o == n) {
                                return true;
                            }
                        }
                        return false;
                    }};
                    std::stable_partition(
                        pv.peer_dependencies.begin(), pv.peer_dependencies.end(),
                        [&](const auto& kv) { return isOptional(kv.first); });
                    std::uint32_t count{0};
                    for (const auto& kv : pv.peer_dependencies) {
                        if (isOptional(kv.first)) {
                            ++count;
                        }
                    }
                    pv.non_optional_peer_dependencies_start = count;
                }
            }

            // publish time (extended manifest)
            if (timeObj != nullptr && timeObj->is_object()) {
                if (const json::Value* t{timeObj->get(versionName)}) {
                    if (auto s{t->as_str()}) {
                        if (auto ms{detail::parse_iso_ms(*s)}) {
                            pv.publish_timestamp_ms = *ms;
                        }
                    }
                }
            }

            // release vs prerelease split (a '-' pre-release tag).
            bool isPre{semver::order(versionName, versionName) == 0 &&
                       versionName.find('-') != std::string_view::npos};
            // Use semver to decide prerelease precisely: a version compares
            // equal to its own release form iff it has no pre-release. Compare
            // against the major.minor.patch prefix.
            {
                auto dash{versionName.find('-')};
                auto plus{versionName.find('+')};
                std::size_t cut{std::min(dash, plus)};
                isPre = dash != std::string_view::npos &&
                        (plus == std::string_view::npos || dash < plus);
                (void)cut;
            }

            if (isPre) {
                result.prereleases.push_back(std::move(pv));
            } else {
                result.releases.push_back(std::move(pv));
            }
        }
    }

    // ── dist-tags ─────────────────────────────────────────────────────────
    if (const json::Value* dt{root.get("dist-tags")}; dt && dt->is_object()) {
        for (const auto& m : dt->members) {
            if (auto s{m.value->as_str()}) {
                result.dist_tags.emplace_back(std::string{m.key}, std::string{*s});
            }
        }
    }

    // Sort releases + prereleases ascending by semver order (bun sorts at
    // serialization time; selection walks the list from the back).
    auto byVersion{[](const PackageVersion& a, const PackageVersion& b) {
        return semver::order(a.version, b.version) < 0;
    }};
    std::stable_sort(result.releases.begin(), result.releases.end(), byVersion);
    std::stable_sort(result.prereleases.begin(), result.prereleases.end(), byVersion);

    return result;
}

}  // namespace mbun::install::npm
