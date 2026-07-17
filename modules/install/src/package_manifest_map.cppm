// package_manifest_map.cppm — mbun.install.package_manifest_map
//
// The in-memory manifest cache map — port of
//   .mbun/bun-ref/src/install/PackageManifestMap.rs
// A `PackageNameHash → Value` map where Value is one of Manifest / Expired /
// NotFound (the NotFound entry avoids re-hitting the filesystem for a package
// already known to be absent).
//
// bun's map threads a `DiskCacheCtx` so the disk-fallback arm can re-open the
// on-disk manifest cache without borrowing `&mut PackageManager`. That disk
// fallback (Serializer::load_by_file_id) is the on-disk-cache layer (DEFERRED);
// here we port the pure in-memory state machine: memory lookup, insert, and the
// extended-manifest demote (a plain manifest is demoted to Expired when an
// extended manifest is required).
export module mbun.install.package_manifest_map;

import std;
import mbun.install.npm;

namespace mbun::install {

export using PackageNameHash = std::uint64_t;

export enum class ManifestState : std::uint8_t { Manifest, Expired, NotFound };

// A cache entry: the state tag plus (for Manifest/Expired) the manifest itself.
export struct ManifestValue {
    ManifestState state{ManifestState::NotFound};
    npm::PackageManifest manifest;
};

export enum class CacheBehavior : std::uint8_t {
    LoadFromMemory,
    LoadFromMemoryFallbackToDisk,
};

export class PackageManifestMap {
public:
    PackageManifestMap() = default;

    // Insert / replace a live manifest for a package.
    void insert(PackageNameHash nameHash, npm::PackageManifest manifest) {
        map_[nameHash] = ManifestValue{ManifestState::Manifest, std::move(manifest)};
    }

    // Mark a package as known-absent (bun's Value::NotFound).
    void insert_not_found(PackageNameHash nameHash) {
        map_[nameHash] = ManifestValue{ManifestState::NotFound, npm::PackageManifest{}};
    }

    // Convenience: hash the name with the registry hash function and insert.
    void insert_by_name(std::string_view name, npm::PackageManifest manifest) {
        insert(npm::registry::string_hash(name), std::move(manifest));
    }

    // Memory-only lookup (bun `by_name_hash_in_memory`): returns the live
    // manifest, or nullptr for Expired / NotFound / missing.
    npm::PackageManifest* by_name_hash_in_memory(PackageNameHash nameHash) {
        auto it{map_.find(nameHash)};
        if (it == map_.end() || it->second.state != ManifestState::Manifest) {
            return nullptr;
        }
        return &it->second.manifest;
    }

    npm::PackageManifest* by_name_in_memory(std::string_view name) {
        return by_name_hash_in_memory(npm::registry::string_hash(name));
    }

    // Port of `by_name_hash_allow_expired` memory arm. `isExpired` (optional
    // out-param) is set to true when an Expired entry is returned; without it,
    // Expired entries yield nullptr. `needsExtendedManifest` demotes a plain
    // (non-extended) live manifest to Expired, matching bun.
    npm::PackageManifest* by_name_hash_allow_expired(PackageNameHash nameHash,
                                                     bool needsExtendedManifest = false,
                                                     bool* isExpired = nullptr) {
        auto it{map_.find(nameHash)};
        if (it == map_.end()) {
            return nullptr;
        }
        ManifestValue& v{it->second};
        if (v.state == ManifestState::Manifest) {
            if (needsExtendedManifest && !v.manifest.has_extended_manifest) {
                v.state = ManifestState::Expired;  // demote
            } else {
                return &v.manifest;
            }
        }
        if (v.state == ManifestState::Expired) {
            if (isExpired != nullptr) {
                *isExpired = true;
                return &v.manifest;
            }
        }
        return nullptr;
    }

    bool contains(PackageNameHash nameHash) const {
        return map_.contains(nameHash);
    }

    std::size_t size() const {
        return map_.size();
    }

private:
    std::unordered_map<PackageNameHash, ManifestValue> map_;
};

}  // namespace mbun::install
