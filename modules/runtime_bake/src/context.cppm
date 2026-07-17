// Per-bake configuration and dependency injection boundary.
// Ref: bun runtime/bake/mod.rs (Mode/Side) and bake_body.rs (UserOptions).
export module mbun.runtime_bake.context;

import std;
import mbun.runtime_bake.artifact_graph;
import mbun.runtime_bake.backend;

export namespace mbun::runtime_bake {

enum class Mode : std::uint8_t { Development, ProductionDynamic, ProductionStatic };
enum class Side : std::uint8_t { Server, Client, Ssr };

struct BakeOptions {
    std::string root;
    std::string output_directory;
    bool source_maps { false };
    bool hot_reload { false };
};

class BakeContext {
    BakeOptions options_;
    ArtifactGraph* graph_;
    BakeBackend* backend_;

public:
    BakeContext(BakeOptions options, ArtifactGraph& graph, BakeBackend& backend)
        : options_ { std::move(options) }, graph_ { &graph }, backend_ { &backend } {}

    [[nodiscard]] const BakeOptions& options() const { return options_; }
    [[nodiscard]] ArtifactGraph& graph() const { return *graph_; }
    [[nodiscard]] BakeBackend& backend() const { return *backend_; }

    ArtifactId register_artifact(std::string_view path, ArtifactKind kind,
                                 std::uint64_t content_hash = 0) {
        return graph_->add_artifact(path, kind, content_hash);
    }

    [[nodiscard]] std::expected<void, std::string> link(ArtifactId from, ArtifactId to) {
        return graph_->add_dependency(from, to);
    }

    [[nodiscard]] std::expected<void, std::string> emit(std::string_view path,
                                                          std::string_view bytes) const {
        return backend_->write_output(ArtifactOutput { .path = std::string { path },
                                                        .bytes = std::string { bytes } });
    }
};

} // namespace mbun::runtime_bake
