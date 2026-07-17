export module mbun.meta.target;

import std;

namespace mbun::meta {

// Target fields are strings so cross-compilation and unknown vendor fields can
// pass through without platform-specific conditionals in this metadata layer.
export struct TargetMetadata {
    std::string os;
    std::string arch;
    std::string environment;
    std::string targetTriple;

    [[nodiscard]] std::string_view triple() const { return targetTriple; }
};

} // namespace mbun::meta
