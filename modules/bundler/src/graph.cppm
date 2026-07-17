// graph.cppm — mbun.bundler.graph: the scan-phase module graph.
//
// Mechanical port of bun's `Graph` + `InputFile` (the SoA `input_files` /
// `ast` columns become plain std::vector rows here; the MultiArrayList SoA
// optimization is DEFERRED as a perf pass). ref:
// .mbun/bun-ref/src/bundler/Graph.rs.
//
// DEFERRED(S-bundler): ThreadPool back-pointer, per-target build_graphs
// PathToSourceIndexMap, server_component_boundaries, the arena (`heap`) and the
// parsed-AST column — all reference members not yet available (parser/arena/
// thread-pool). Kept as fields/notes so the shape matches the blueprint.
export module mbun.bundler.graph;

import std;
import mbun.bundler.index;
import mbun.bundler.source;
import mbun.bundler.options;

export namespace mbun::bundler {

// InputFileFlags (bitflags, Graph.rs).
enum class InputFileFlags : std::uint8_t {
    None = 0,
    IsPluginFile = 1 << 0,
    IsExportStarTarget = 1 << 1,
};
constexpr InputFileFlags operator|(InputFileFlags a, InputFileFlags b) {
    return static_cast<InputFileFlags>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
constexpr bool has_flag(InputFileFlags v, InputFileFlags f) {
    return (static_cast<std::uint8_t>(v) & static_cast<std::uint8_t>(f)) != 0;
}

// `additional_files` entries — either a source index or an output-file index.
struct AdditionalFile {
    enum class Kind : std::uint8_t { SourceIndex, OutputFile } kind { Kind::SourceIndex };
    std::uint32_t value { 0 };
};

// InputFile (Graph.rs) — one row per source index.
struct InputFile {
    Source source;
    std::string secondaryPath;
    Loader loader { Loader::File };
    SideEffects sideEffects { SideEffects::HasSideEffects };
    std::vector<AdditionalFile> additionalFiles;
    std::string uniqueKeyForAdditionalFile;
    std::uint64_t contentHashForAdditionalFile { 0 };
    InputFileFlags flags { InputFileFlags::None };
};

// html_imports (Graph.rs::HtmlImports).
struct HtmlImports {
    std::vector<IndexInt> serverSourceIndices;
    std::vector<IndexInt> htmlSourceIndices;
};

// The scan-phase module graph.
struct Graph {
    // User-specified entry points → their source Index.
    std::vector<Index> entryPoints;

    // One InputFile per source index (bun: SoA MultiArrayList<InputFile>).
    std::vector<InputFile> inputFiles;

    // Remaining scan/parse tasks; scan ends and linking begins at 0.
    std::uint32_t pendingItems { 0 };
    // Count "moved" out of pendingItems by onLoad .defer().
    std::uint32_t deferredPending { 0 };

    HtmlImports htmlImports;

    std::size_t estimatedFileLoaderCount { 0 };
    std::size_t cssFileCount { 0 };

    bool kitReferencedServerData { false };
    bool kitReferencedClientData { false };
    bool hasAnySecondaryPaths { false };

    // Append an input file, returning its assigned source index.
    IndexInt add_input_file(InputFile file) {
        auto idx = static_cast<IndexInt>(inputFiles.size());
        file.source.index = Index::source(idx);
        inputFiles.push_back(std::move(file));
        return idx;
    }

    // Graph.rs::drain_deferred_tasks (thread-pool scheduling DEFERRED).
    bool drain_deferred_tasks() {
        if (deferredPending > 0) {
            pendingItems += deferredPending;
            deferredPending = 0;
            // DEFERRED(S-bundler): transpiler.drain_defer_task.schedule()
            return true;
        }
        return false;
    }
};

}  // namespace mbun::bundler
