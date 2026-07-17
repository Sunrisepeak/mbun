// text_writer.cppm — serialize the text-model Lockfile back to `bun.lock`.
//
// Mechanical port of bun's text lockfile Stringifier:
//   ref: bun-ref/src/install/lockfile/bun.lock.rs
//     - save_from_binary_inner (top-level section order + JSONC style)
//     - write_workspace_deps (workspace entry layout)
//     - write_package_info_object (single-line INFO object)
//     - version_to_write (never bump an existing version; floor v0 to v1;
//       stamp v2 only when every entry passes the v2 parse checks)
//
// Output style facts verified against bun v1.4.0 output (fixtures in
// docs/design/20260713-cli-install-file-dependency-slice.md lineage):
//   - 2-space indent, trailing commas inside `workspaces` entries and after
//     every package tuple (JSONC), file ends "}\n".
//   - package entries are separated by a blank line (",\n\n").
//   - npm tuples always carry 4 elements (registry "" when under the default
//     registry, integrity "" when absent); workspace tuples carry exactly 1.
export module mbun.install.lockfile.text_writer;

import std;
import mbun.install;

namespace mbun::install::lockfile::text_writer {

// Current text lockfile version (bun.lock.rs Version::CURRENT = V2).
export constexpr std::uint32_t CURRENT_VERSION{2};
// ConfigVersion::CURRENT (config_version.rs) — stamped on fresh lockfiles.
export constexpr std::uint32_t CURRENT_CONFIG_VERSION{1};

// The parser-shared predicates (url_is_under_registry, is_safe_resolved_tag,
// integrity_is_supported) live in mbun.install, exactly as bun shares them
// between parse_into_binary_lockfile and the Stringifier.

bool url_is_under_default_registry(std::string_view url) {
    return mbun::install::url_is_under_registry(url, mbun::install::DEFAULT_REGISTRY_URL);
}

// ref: bun.lock.rs version_to_write. `loadedVersion` empty = no lockfile
// previously existed (fresh install / migration) — the only case that is a
// candidate for the current version, and even then only when every package
// satisfies the v2 parse invariants (checked against the DEFAULT registry
// only, never the writer's scoped-registry config, so the stamp does not
// depend on the writer's ~/.npmrc).
export std::uint32_t version_to_write(const Lockfile& lf,
                                      std::optional<std::uint32_t> loadedVersion) {
    if (loadedVersion && *loadedVersion < CURRENT_VERSION) {
        return std::max<std::uint32_t>(*loadedVersion, 1);
    }
    for (const Package& pkg : lf.packages) {
        switch (pkg.tag) {
            case Resolution::Npm:
                if (!integrity_is_supported(pkg.integrity) && !pkg.registry.empty() &&
                    !url_is_under_default_registry(pkg.registry)) {
                    return 1;
                }
                break;
            case Resolution::Git:
                // github tags are rejected at parse time for every version, so
                // only the git arm can force a downgrade (bun.lock.rs:271-282).
                if (!is_safe_resolved_tag(pkg.bunTag)) {
                    return 1;
                }
                break;
            default:
                break;
        }
    }
    return CURRENT_VERSION;
}

namespace detail {

void write_json_string(std::string& out, std::string_view s) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += std::format("\\u{:04x}", static_cast<unsigned char>(c));
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

void write_indent(std::string& out, std::uint32_t indent) {
    for (std::uint32_t i{0}; i < indent; ++i) {
        out += "  ";
    }
}

struct GroupSpec {
    std::string_view name;
    DepKind kind;
};

// ref: bun.lock.rs WORKSPACE_DEPENDENCY_GROUPS (order is part of the format).
constexpr std::array GROUPS{GroupSpec{"dependencies", DepKind::Prod},
                            GroupSpec{"devDependencies", DepKind::Dev},
                            GroupSpec{"optionalDependencies", DepKind::Optional},
                            GroupSpec{"peerDependencies", DepKind::Peer}};

std::vector<const Dep*> sorted_group(const std::vector<Dep>& deps, DepKind kind) {
    std::vector<const Dep*> out;
    for (const Dep& dep : deps) {
        if (dep.kind == kind) {
            out.push_back(&dep);
        }
    }
    std::ranges::sort(out, [](const Dep* a, const Dep* b) { return a->name < b->name; });
    return out;
}

// ref: bun.lock.rs write_workspace_deps. Ends with "}," and no newline; the
// caller owns entry separators exactly like bun's writer.
void write_workspace_entry(std::string& out, std::uint32_t& indent, const Workspace& ws) {
    bool any{false};
    if (ws.path.empty()) {
        out += "\"\": {";
        if (!ws.name.empty()) {
            out.push_back('\n');
            ++indent;
            write_indent(out, indent);
            out += "\"name\": ";
            write_json_string(out, ws.name);
            // bun does not save the root version (bun.lock.rs:1260 TODO).
            any = true;
        }
    } else {
        write_json_string(out, ws.path);
        out += ": {";
        out.push_back('\n');
        ++indent;
        write_indent(out, indent);
        out += "\"name\": ";
        write_json_string(out, ws.name);
        if (!ws.version.empty()) {
            out += ",\n";
            write_indent(out, indent);
            out += "\"version\": ";
            write_json_string(out, ws.version);
        }
        any = true;
    }

    for (const auto& group : GROUPS) {
        const auto members{sorted_group(ws.deps, group.kind)};
        bool first{true};
        for (const Dep* dep : members) {
            if (first) {
                if (any) {
                    out.push_back(',');
                }
                out.push_back('\n');
                if (!any) {
                    ++indent;
                }
                write_indent(out, indent);
                out.push_back('"');
                out += group.name;
                out += "\": {\n";
                ++indent;
                write_indent(out, indent);
                any = true;
                first = false;
            } else {
                out += ",\n";
                write_indent(out, indent);
            }
            write_json_string(out, dep->name);
            out += ": ";
            write_json_string(out, dep->version);
        }
        if (!first) {
            out += ",\n";
            --indent;
            write_indent(out, indent);
            out.push_back('}');
        }
    }

    if (any) {
        out += ",\n";
        --indent;
        write_indent(out, indent);
    }
    out += "},";
}

void write_platform_list(std::string& out, const std::vector<std::string_view>& values) {
    if (values.size() == 1) {
        write_json_string(out, values.front());
        return;
    }
    out.push_back('[');
    for (std::size_t i{0}; i < values.size(); ++i) {
        if (i != 0) {
            out += ", ";
        }
        write_json_string(out, values[i]);
    }
    out.push_back(']');
}

// ref: bun.lock.rs write_package_info_object — single-line INFO:
// { "dependencies": { "a": "1" }, "optionalPeers": [...], "os": ..., "cpu": ... }
void write_info_object(std::string& out, const Package& pkg) {
    out.push_back('{');
    bool any{false};
    for (const auto& group : GROUPS) {
        const auto members{sorted_group(pkg.deps, group.kind)};
        bool first{true};
        for (const Dep* dep : members) {
            if (first) {
                if (any) {
                    out.push_back(',');
                }
                out += " \"";
                out += group.name;
                out += "\": { ";
                any = true;
                first = false;
            } else {
                out += ", ";
            }
            write_json_string(out, dep->name);
            out += ": ";
            write_json_string(out, dep->version);
        }
        if (!first) {
            out += " }";
        }
    }
    if (!pkg.optionalPeers.empty()) {
        out += ", \"optionalPeers\": [";
        for (std::size_t i{0}; i < pkg.optionalPeers.size(); ++i) {
            if (i != 0) {
                out.push_back(' ');
            }
            write_json_string(out, pkg.optionalPeers[i]);
            if (i + 1 != pkg.optionalPeers.size()) {
                out.push_back(',');
            }
        }
        out.push_back(']');
    }
    if (pkg.bundled) {
        if (any) {
            out.push_back(',');
        }
        any = true;
        out += " \"bundled\": true";
    }
    if (!pkg.os.empty()) {
        if (any) {
            out.push_back(',');
        }
        any = true;
        out += " \"os\": ";
        write_platform_list(out, pkg.os);
    }
    if (!pkg.cpu.empty()) {
        if (any) {
            out.push_back(',');
        }
        any = true;
        out += " \"cpu\": ";
        write_platform_list(out, pkg.cpu);
    }
    if (any) {
        out.push_back(' ');
    }
    out.push_back('}');
}

// The tree key is "<relative_path>/<dep_name>"; a scoped name ("@scope/pkg")
// is one logical component. Depth 0 = hoisted root entry.
std::size_t key_depth(std::string_view key) {
    std::size_t components{0};
    std::size_t i{0};
    while (i < key.size()) {
        const bool scoped{key[i] == '@'};
        std::size_t slash{key.find('/', i)};
        if (scoped && slash != std::string_view::npos) {
            slash = key.find('/', slash + 1);
        }
        ++components;
        if (slash == std::string_view::npos) {
            break;
        }
        i = slash + 1;
    }
    return components == 0 ? 0 : components - 1;
}

// One package tuple. ref: bun.lock.rs tuple layouts (comment at :773-781).
void write_package_tuple(std::string& out, const Package& pkg) {
    out.push_back('[');
    std::string resInfo;
    resInfo.append(pkg.name);
    resInfo.push_back('@');
    switch (pkg.tag) {
        case Resolution::Root: resInfo += "root:"; break;
        case Resolution::Npm: resInfo.append(pkg.version); break;
        case Resolution::Workspace:
            resInfo += "workspace:";
            resInfo.append(pkg.resolution);
            break;
        case Resolution::Folder:
            resInfo += "file:";
            resInfo.append(pkg.resolution);
            break;
        case Resolution::Symlink:
            resInfo += "link:";
            resInfo.append(pkg.resolution);
            break;
        default: resInfo.append(pkg.resolution); break;
    }
    write_json_string(out, resInfo);

    switch (pkg.tag) {
        case Resolution::Workspace:
            // workspace tuples are the single-element v1+ form.
            break;
        case Resolution::Npm:
            out += ", ";
            write_json_string(out, url_is_under_default_registry(pkg.registry)
                                       ? std::string_view{}
                                       : pkg.registry);
            out += ", ";
            write_info_object(out, pkg);
            out += ", ";
            write_json_string(out, pkg.integrity);
            break;
        case Resolution::Git:
        case Resolution::Github:
            out += ", ";
            write_info_object(out, pkg);
            out += ", ";
            write_json_string(out, pkg.bunTag);
            if (integrity_is_supported(pkg.integrity)) {
                out += ", ";
                write_json_string(out, pkg.integrity);
            }
            break;
        case Resolution::LocalTarball:
        case Resolution::RemoteTarball:
            out += ", ";
            write_info_object(out, pkg);
            if (integrity_is_supported(pkg.integrity)) {
                out += ", ";
                write_json_string(out, pkg.integrity);
            }
            break;
        default:
            out += ", ";
            write_info_object(out, pkg);
            break;
    }
    out.push_back(']');
}

}  // namespace detail

// Serialize `lf` at the given versions. Workspaces: root ("") first, the rest
// sorted by path. Packages: sorted by (depth, key) — the observable order of
// bun's (depth, relative_path, dep name) tree sort.
export std::string write_text(const Lockfile& lf, std::uint32_t lockfileVersion,
                              std::uint32_t configVersion) {
    std::string out;
    out.reserve(1024);
    std::uint32_t indent{0};

    out += "{\n";
    ++indent;
    detail::write_indent(out, indent);
    out += std::format("\"lockfileVersion\": {},\n", lockfileVersion);
    detail::write_indent(out, indent);
    out += std::format("\"configVersion\": {},\n", configVersion);
    detail::write_indent(out, indent);

    out += "\"workspaces\": {\n";
    ++indent;
    {
        std::vector<const Workspace*> ordered;
        const Workspace* root{nullptr};
        for (const Workspace& ws : lf.workspaces) {
            if (ws.path.empty()) {
                root = &ws;
            } else {
                ordered.push_back(&ws);
            }
        }
        std::ranges::sort(ordered,
                          [](const Workspace* a, const Workspace* b) { return a->path < b->path; });
        detail::write_indent(out, indent);
        if (root != nullptr) {
            detail::write_workspace_entry(out, indent, *root);
        } else {
            out += "\"\": {},";
        }
        for (const Workspace* ws : ordered) {
            out.push_back('\n');
            detail::write_indent(out, indent);
            detail::write_workspace_entry(out, indent, *ws);
        }
    }
    out.push_back('\n');
    --indent;
    detail::write_indent(out, indent);
    out += "},\n";

    if (!lf.trustedDependencies.empty()) {
        auto trusted{lf.trustedDependencies};
        std::ranges::sort(trusted);
        detail::write_indent(out, indent);
        out += "\"trustedDependencies\": [\n";
        ++indent;
        for (std::string_view name : trusted) {
            detail::write_indent(out, indent);
            detail::write_json_string(out, name);
            out += ",\n";
        }
        --indent;
        detail::write_indent(out, indent);
        out += "],\n";
    }

    if (!lf.patchedDependencies.empty()) {
        auto patched{lf.patchedDependencies};
        std::ranges::sort(patched);
        detail::write_indent(out, indent);
        out += "\"patchedDependencies\": {\n";
        ++indent;
        for (const auto& [key, value] : patched) {
            detail::write_indent(out, indent);
            detail::write_json_string(out, key);
            out += ": ";
            detail::write_json_string(out, value);
            out += ",\n";
        }
        --indent;
        detail::write_indent(out, indent);
        out += "},\n";
    }

    if (!lf.overrides.empty()) {
        auto overrides{lf.overrides};
        std::ranges::sort(overrides);
        detail::write_indent(out, indent);
        out += "\"overrides\": {\n";
        ++indent;
        for (const auto& [name, value] : overrides) {
            detail::write_indent(out, indent);
            detail::write_json_string(out, name);
            out += ": ";
            detail::write_json_string(out, value);
            out += ",\n";
        }
        --indent;
        detail::write_indent(out, indent);
        out += "},\n";
    }

    detail::write_indent(out, indent);
    out += "\"packages\": {";
    {
        std::vector<const Package*> ordered;
        ordered.reserve(lf.packages.size());
        for (const Package& pkg : lf.packages) {
            if (pkg.tag == Resolution::Uninitialized) {
                continue;
            }
            ordered.push_back(&pkg);
        }
        std::ranges::sort(ordered, [](const Package* a, const Package* b) {
            const std::size_t depthA{detail::key_depth(a->key)};
            const std::size_t depthB{detail::key_depth(b->key)};
            if (depthA != depthB) {
                return depthA < depthB;
            }
            return a->key < b->key;
        });
        bool first{true};
        for (const Package* pkg : ordered) {
            if (first) {
                first = false;
                out.push_back('\n');
                ++indent;
                detail::write_indent(out, indent);
            } else {
                out += ",\n\n";
                detail::write_indent(out, indent);
            }
            detail::write_json_string(out, pkg->key);
            out += ": ";
            detail::write_package_tuple(out, *pkg);
        }
        if (!first) {
            out += ",\n";
            --indent;
            detail::write_indent(out, indent);
        }
    }
    out += "}\n";
    --indent;
    out += "}\n";
    return out;
}

}  // namespace mbun::install::lockfile::text_writer
