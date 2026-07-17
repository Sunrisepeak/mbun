import std;
import mbun.runtime_bake;

namespace {
int checks { 0 };
int failures { 0 };
void check(bool condition, std::string_view message) {
    ++checks;
    if (!condition) { ++failures; std::println("FAIL: {}", message); }
}

void test_graph_is_stable_and_deduplicated() {
    mbun::runtime_bake::ArtifactGraph graph;
    const auto source = graph.add_artifact("src/app.ts", mbun::runtime_bake::ArtifactKind::Source, 11);
    const auto same = graph.add_artifact("src/app.ts", mbun::runtime_bake::ArtifactKind::JavaScript, 12);
    check(source == same, "repeated path keeps a stable artifact id");
    check(graph.size() == 1, "repeated path does not add a second node");
    check(graph.get(source)->kind == mbun::runtime_bake::ArtifactKind::JavaScript,
          "incremental observation refreshes metadata");
}

void test_graph_order_and_cycle_error() {
    mbun::runtime_bake::ArtifactGraph graph;
    const auto source = graph.add_artifact("source", mbun::runtime_bake::ArtifactKind::Source);
    const auto chunk = graph.add_artifact("chunk", mbun::runtime_bake::ArtifactKind::JavaScript);
    const auto html = graph.add_artifact("index.html", mbun::runtime_bake::ArtifactKind::Html);
    check(graph.add_dependency(chunk, source).has_value(), "link source into chunk");
    check(graph.add_dependency(html, chunk).has_value(), "link chunk into html");
    const auto order = graph.topological_order();
    check(order.has_value() && order->at(0) == source && order->at(2) == html,
          "topological order emits dependencies before consumers");
    check(graph.add_dependency(source, html).has_value(), "cycle edge is accepted by graph storage");
    check(!graph.topological_order().has_value(), "cycle is reported at scheduling boundary");
}

void test_context_backend_seam() {
    std::string written;
    mbun::runtime_bake::BakeBackend backend {
        [](std::string_view path) -> std::expected<std::string, std::string> {
            return std::string { "source:" } + std::string { path };
        },
        [&](const mbun::runtime_bake::ArtifactOutput& output) -> std::expected<void, std::string> {
            written = output.path + "=" + output.bytes;
            return {};
        },
    };
    mbun::runtime_bake::ArtifactGraph graph;
    mbun::runtime_bake::BakeContext context {
        { .root = "/project", .output_directory = "/project/dist", .source_maps = true }, graph, backend
    };
    const auto id = context.register_artifact("entry.ts", mbun::runtime_bake::ArtifactKind::Source);
    check(id.is_valid() && context.options().source_maps, "context owns options and graph registration");
    check(backend.read_source("entry.ts").value() == "source:entry.ts", "reader is injected");
    check(context.emit("dist/entry.js", "compiled").has_value() && written == "dist/entry.js=compiled",
          "output writer is injected");
}
} // namespace

int main() {
    test_graph_is_stable_and_deduplicated();
    test_graph_order_and_cycle_error();
    test_context_backend_seam();
    std::println("runtime_bake: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}
