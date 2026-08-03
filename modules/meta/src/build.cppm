export module mbun.meta.build;

import std;

namespace mbun::meta {

// Stable build identity seam. Revision generation and dirty-tree detection are
// intentionally deferred until the Bun Rust/Zig metadata implementations are
// available for comparison.
export struct BuildMetadata {
    std::string version;
    std::string revision;
    bool dirty {};
};

} // namespace mbun::meta
