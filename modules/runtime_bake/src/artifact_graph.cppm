// Artifact graph for runtime/bake.  The graph is intentionally independent of
// bundler/JSC: those integrations consume this stable, index-based surface.
// Ref: bun runtime/bake/dev_server/incremental_graph.rs and
// runtime/bake/production.rs (artifact/output index ownership).
export module mbun.runtime_bake.artifact_graph;

import std;

export namespace mbun::runtime_bake {

enum class ArtifactKind : std::uint8_t { Source, JavaScript, Css, Asset, Html };

class ArtifactId {
    std::uint32_t value_ { INVALID_VALUE };

public:
    static constexpr std::uint32_t INVALID_VALUE { 0xFFFF'FFFFu };

    constexpr ArtifactId() = default;
    explicit constexpr ArtifactId(std::uint32_t value) : value_ { value } {}
    [[nodiscard]] constexpr bool is_valid() const { return value_ != INVALID_VALUE; }
    [[nodiscard]] constexpr std::uint32_t value() const { return value_; }
    friend constexpr bool operator==(ArtifactId, ArtifactId) = default;
};

struct Artifact {
    std::string path;
    ArtifactKind kind { ArtifactKind::Source };
    std::uint64_t content_hash { 0 };
    std::vector<ArtifactId> dependencies;
    std::vector<ArtifactId> dependents;
};

class ArtifactGraph {
    std::vector<Artifact> artifacts_;
    std::unordered_map<std::string, ArtifactId> by_path_;

    [[nodiscard]] bool contains_(ArtifactId id) const {
        return id.is_valid() && id.value() < artifacts_.size();
    }

public:
    // Bun keeps graph nodes in stable indexed storage.  Returning the existing
    // id makes repeated scanner observations idempotent and avoids duplicate
    // edges during incremental rebuilds.
    ArtifactId add_artifact(std::string_view path, ArtifactKind kind,
                            std::uint64_t content_hash = 0) {
        if (const auto found = by_path_.find(std::string { path }); found != by_path_.end()) {
            auto& artifact = artifacts_[found->second.value()];
            artifact.kind = kind;
            artifact.content_hash = content_hash;
            return found->second;
        }
        const auto id = ArtifactId { static_cast<std::uint32_t>(artifacts_.size()) };
        artifacts_.push_back(Artifact { .path = std::string { path }, .kind = kind,
                                        .content_hash = content_hash });
        by_path_.emplace(artifacts_.back().path, id);
        return id;
    }

    [[nodiscard]] std::expected<void, std::string> add_dependency(ArtifactId from,
                                                                    ArtifactId to) {
        if (!contains_(from) || !contains_(to))
            return std::unexpected { "artifact graph edge references an invalid id" };
        auto& dependencies = artifacts_[from.value()].dependencies;
        if (std::ranges::find(dependencies, to) == dependencies.end()) {
            dependencies.push_back(to);
            artifacts_[to.value()].dependents.push_back(from);
        }
        return {};
    }

    [[nodiscard]] std::expected<std::vector<ArtifactId>, std::string> topological_order() const {
        std::vector<std::size_t> remaining(artifacts_.size());
        std::vector<ArtifactId> order;
        for (std::size_t i { 0 }; i < artifacts_.size(); ++i)
            remaining[i] = artifacts_[i].dependencies.size();

        std::deque<ArtifactId> ready;
        for (std::size_t i { 0 }; i < remaining.size(); ++i)
            if (remaining[i] == 0) ready.emplace_back(static_cast<std::uint32_t>(i));
        while (!ready.empty()) {
            const auto id = ready.front();
            ready.pop_front();
            order.push_back(id);
            for (const auto dependent : artifacts_[id.value()].dependents)
                if (--remaining[dependent.value()] == 0) ready.push_back(dependent);
        }
        if (order.size() != artifacts_.size())
            return std::unexpected { "artifact graph contains a dependency cycle" };
        return order;
    }

    [[nodiscard]] const Artifact* get(ArtifactId id) const {
        return contains_(id) ? &artifacts_[id.value()] : nullptr;
    }
    [[nodiscard]] std::size_t size() const { return artifacts_.size(); }
};

} // namespace mbun::runtime_bake
