// chunk.cppm — mbun.bundler.chunk: an output chunk descriptor.
//
// Port of bun's `Chunk` (Chunk.rs). The heavy borrowed/arena fields —
// `entry_bits: AutoBitSet`, `renamer: ChunkRenamer`, `output_source_map:
// SourceMapPieces`, `intermediate_output`, `compile_results_for_chunk` — depend
// on the linker/renamer/sourcemap-pieces machinery and are DEFERRED(S-bundler).
// We keep the owning scalar/vector fields and the Content/Flags/ChunkImport
// value types 1:1 with the blueprint.
// ref: .mbun/bun-ref/src/bundler/Chunk.rs.
export module mbun.bundler.chunk;

import std;
import mbun.bundler.index;
import mbun.bundler.entry_points;

export namespace mbun::bundler {

// ImportKind (ast) — the subset the chunk pipeline observes.
enum class ImportKind : std::uint8_t {
    EntryPointRun = 0,
    EntryPointBuild = 1,
    Stmt = 2,
    Require = 3,
    Dynamic = 4,
    RequireResolve = 5,
    At = 6,
    Url = 7,
    Internal = 8,
};

// ChunkImport (Chunk.rs) — a cross-chunk edge for code splitting.
struct ChunkImport {
    std::uint32_t chunkIndex { 0 };
    ImportKind importKind { ImportKind::Stmt };
};

// Chunk.Content tag (Chunk.rs) — JS / CSS / HTML chunk.
enum class ContentKind : std::uint8_t { Javascript = 0, Css = 1, Html = 2 };

// Chunk.Flags bitflags (Chunk.rs).
enum class ChunkFlags : std::uint8_t {
    None = 0,
    IsExecutable = 1 << 0,
    HasHtmlChunk = 1 << 1,
    IsBrowserChunkFromServerBuild = 1 << 2,
};
constexpr bool has_flag(ChunkFlags v, ChunkFlags f) {
    return (static_cast<std::uint8_t>(v) & static_cast<std::uint8_t>(f)) != 0;
}

struct Chunk {
    // Random placeholder key for the pre-final output path (OutputPiece trick).
    std::string uniqueKey;

    // source index → bytes contributed (metafile bytesInOutput).
    std::unordered_map<IndexInt, std::size_t> filesWithPartsInChunk;

    // Final relative output path (computed from a PathTemplate).
    std::string finalRelPath;

    // Cross-chunk imports for code splitting.
    std::vector<ChunkImport> crossChunkImports;

    ContentKind content { ContentKind::Javascript };
    EntryPoint entryPoint;
    std::uint64_t isolatedHash { 0 };

    // Pre-built metafile JSON fragment for this chunk.
    std::string metafileChunkJson;

    ChunkFlags flags { ChunkFlags::None };

    // DEFERRED(S-bundler): entry_bits (AutoBitSet), template (PathTemplate),
    // output_source_map (SourceMapPieces), intermediate_output, renamer
    // (ChunkRenamer), compile_results_for_chunk (CompileResultSlots).
};

}  // namespace mbun::bundler
