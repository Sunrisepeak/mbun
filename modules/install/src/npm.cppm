// npm.cppm — mbun.install.npm aggregator
//
// npm.rs in bun is a single 3200-line file; per AGENTS.md's ≤2000-line rule it
// is split here into a directory of submodules by concern, re-exported through
// this thin aggregator so callers can `import mbun.install.npm;` and see the
// whole surface:
//   - mbun.install.npm.negatable    os/cpu/libc allow/block-list bitsets
//   - mbun.install.npm.json         self-contained packument JSON reader
//   - mbun.install.npm.registry     registry Scope + url hashing
//   - mbun.install.npm.manifest     PackageManifest / PackageVersion / Integrity
//   - mbun.install.npm.parse        packument JSON → PackageManifest
//   - mbun.install.npm.version_map  version selection (range/dist-tag → version)
//
// DEFERRED (network / on-disk-cache layers, not pure manifest logic):
//   - HTTP fetch (get_package_metadata / whoami), the on-disk manifest cache
//     byte serializer (Serializer::write/read + its ABI-pinned struct layouts),
//     and auth-token derivation in registry::Scope::from_api.
export module mbun.install.npm;

export import mbun.install.npm.negatable;
export import mbun.install.npm.json;
export import mbun.install.npm.registry;
export import mbun.install.npm.manifest;
export import mbun.install.npm.parse;
export import mbun.install.npm.version_map;
