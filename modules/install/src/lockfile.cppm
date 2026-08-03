// lockfile.cppm — mbun.install.lockfile (aggregator)
//
// The fuller port of bun's install lockfile (src/install/lockfile.rs and its
// submodules): the in-memory arena model + the bun.lockb binary read/write +
// the default trusted-dependency list. Split by concern into submodules to stay
// under the 2000-line/file rule; this thin aggregator re-exports them so callers
// can `import mbun.install.lockfile;`.
//
// Sibling module `mbun.install` (install.cppm) owns the bun.lock *text* (JSONC)
// parser and a lighter Package/Dep model; this module owns the binary format and
// the complete on-disk model. Overlapping concepts (Resolution tag, Package,
// Dependency) are intentionally defined in the `mbun::install::lockfile`
// namespace here and reconciled with `mbun::install` at wire-time.
export module mbun.install.lockfile;

export import mbun.install.lockfile.model;
export import mbun.install.lockfile.migrate_v2;
export import mbun.install.lockfile.binary;
export import mbun.install.lockfile.decode;
export import mbun.install.lockfile.adapt;
export import mbun.install.lockfile.trusted;
export import mbun.install.lockfile.tree;
