// output_file.cppm — mbun.bundler.output_file: a produced build artifact.
//
// Port of bun's `OutputFile` (OutputFile.rs). The `value` union in bun holds a
// saved-to-disk path, an in-memory buffer, or a "move" handle; here we model it
// as a buffer + optional dest path. Real fs `Path` handle and the disk-save
// variant are DEFERRED(S-bundler).
// ref: .mbun/bun-ref/src/bundler/OutputFile.rs, bundler/lib.rs (OutputKind).
export module mbun.bundler.output_file;

import std;
import mbun.bundler.index;
import mbun.bundler.options;

export namespace mbun::bundler {

// OutputFile.Value — bun's saved/buffer/move union (buffer arm ported).
struct OutputValue {
    enum class Kind : std::uint8_t { Buffer, SavedFile, Move } kind { Kind::Buffer };
    std::string buffer;       // Kind::Buffer
    std::string savedPath;    // Kind::SavedFile / Kind::Move dest
};

// Which side of a server-components build (bake_types::Side).
enum class Side : std::uint8_t { Server = 0, Client = 1 };

struct OutputFile {
    Loader loader { Loader::File };
    Loader inputLoader { Loader::File };
    std::string srcPathText;         // bun: fs::Path<'static> + owned text
    OutputValue value;
    std::size_t size { 0 };
    std::size_t sizeWithoutSourcemap { 0 };
    std::uint64_t hash { 0 };
    bool isExecutable { false };
    std::uint32_t sourceMapIndex { INVALID };
    std::uint32_t bytecodeIndex { INVALID };
    std::uint32_t moduleInfoIndex { INVALID };
    OutputKind outputKind { OutputKind::Chunk };
    std::string destPath;            // relative
    std::optional<Side> side;
    std::optional<std::uint32_t> entryPointIndex;
    std::vector<Index> referencedCssChunks;
    Index sourceIndex { Index::invalid() };

    static constexpr std::uint32_t INVALID { 0xFFFF'FFFFu };
};

}  // namespace mbun::bundler
