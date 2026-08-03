// bundle.cppm — mbun.bundler.bundle: the top-level BundleV2 driver facade.
//
// Port of bun's `BundleV2` (bundle_v2.rs) top-level shape: it owns the module
// Graph, the entry-point list, the produced chunks and output files, and drives
// the bundle in phases (enqueue entry points → scan/parse → link → compute
// chunks → generate output). Every phase body that needs the real
// parser/resolver/thread-pool/linker/JSC bindings is DEFERRED(S-bundler) and
// left as a documented stub so the driver compiles and the control flow matches
// the blueprint.
// ref: .mbun/bun-ref/src/bundler/bundle_v2.rs (BundleV2, enqueue_entry_points_*,
//      generate_from_cli), transpiler.rs (Transpiler).
export module mbun.bundler.bundle;

import std;
import mbun.bundler.index;
import mbun.bundler.source;
import mbun.bundler.options;
import mbun.bundler.graph;
import mbun.bundler.entry_points;
import mbun.bundler.chunk;
import mbun.bundler.output_file;

export namespace mbun::bundler {

// Result of a BundleV2 pass. Named for its driver rather than the generic
// "BundleResult": mbun.bundler.vertical_slice exports its own, differently
// shaped BundleResult, and bundler.cppm re-exports both into this namespace.
// Two same-named entities attached to different named modules is ill-formed --
// GCC 16 happens to accept it, Clang correctly rejects the whole module.
struct BundleV2Result {
    std::vector<OutputFile> outputFiles;
    bool ok { true };
    std::string error;  // non-empty when a DEFERRED phase blocked completion.
};

// BundleV2 — the top-level bundler driver (bundle_v2.rs::BundleV2).
class BundleV2 {
private:
    BundleOptions options_;
    Graph graph_;
    std::vector<EntryPoint> entryPoints_;
    std::vector<Chunk> chunks_;
    std::uint64_t uniqueKey_ { 0 };
    bool hasAnyTopLevelAwaitModules_ { false };

public:
    BundleV2() = default;
    explicit BundleV2(BundleOptions opts) : options_ { std::move(opts) } {}

    [[nodiscard]] const BundleOptions& options() const { return options_; }
    [[nodiscard]] BundleOptions& options() { return options_; }
    [[nodiscard]] const Graph& graph() const { return graph_; }
    [[nodiscard]] Graph& graph() { return graph_; }
    [[nodiscard]] const std::vector<Chunk>& chunks() const { return chunks_; }

    // bundle_v2.rs::enqueue_entry_points_normal — register a user entry point.
    // Real path resolution + parse-task scheduling is DEFERRED(S-bundler);
    // here we record the entry as a source row and bump the scan counter.
    IndexInt enqueue_entry_point(std::string_view absPath, std::string_view contents = {}) {
        InputFile file;
        file.source = Source::init_path_string(absPath, contents);
        auto idx = graph_.add_input_file(std::move(file));
        graph_.entryPoints.push_back(Index::source(idx));
        graph_.pendingItems += 1;
        EntryPoint ep;
        ep.sourceIndex = idx;
        ep.outputPath = std::string { absPath };
        entryPoints_.push_back(std::move(ep));
        return idx;
    }

    // ── phase 1: scan + parse (bundle_v2.rs::waitForParse) ──
    // DEFERRED(S-bundler): drives the ThreadPool of ParseTasks, populating the
    // AST column and discovering import records. Requires mbun.js_parser +
    // resolver + a thread pool.
    void scan_and_parse_() {
        // DEFERRED(S-bundler)
        graph_.pendingItems = 0;
    }

    // ── phase 2: link (linker.rs / LinkerContext.rs) ──
    // DEFERRED(S-bundler): scanImportsAndExports, tree-shaking, symbol renaming,
    // wrapper generation. The single largest ported surface (LinkerContext.rs).
    void link_() {
        // DEFERRED(S-bundler)
    }

    // ── phase 3: compute chunks (linker_context/computeChunks.rs) ──
    // DEFERRED(S-bundler): entry-bit partitioning of parts into chunks + cross
    // chunk dependency computation.
    void compute_chunks_() {
        // DEFERRED(S-bundler)
    }

    // ── phase 4: generate output (generateChunksInParallel + writeOutput) ──
    // DEFERRED(S-bundler): parallel codegen + sourcemaps + disk write.
    BundleV2Result generate_output_() {
        BundleV2Result r;
        r.ok = false;
        r.error = "bundle pipeline DEFERRED(S-bundler): parse/link/chunk/codegen "
                  "require js_parser + resolver + thread pool + JSC bindings";
        return r;
    }

    // bundle_v2.rs::generate_from_cli — run the full pass. Sequences the phases;
    // each is a documented DEFERRED stub until the lower tiers land.
    BundleV2Result run() {
        scan_and_parse_();
        link_();
        compute_chunks_();
        return generate_output_();
    }

    // bundle_v2.rs::generate_unique_key.
    static std::uint64_t generate_unique_key() {
        // DEFERRED(S-bundler): bun seeds from a CSPRNG. Deterministic stub.
        static std::uint64_t counter { 0x9E37'79B9'7F4A'7C15ull };
        counter = counter * 6364136223846793005ull + 1442695040888963407ull;
        return counter;
    }
};

}  // namespace mbun::bundler
