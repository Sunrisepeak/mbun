// standalone_exe.cppm — `bun build --compile`: the single-file executable container.
//
// bun's `--compile` copies the bun binary and appends a serialized
// StandaloneModuleGraph plus a trailer that the runtime finds by seeking to EOF
// (ref: bun-ref/src/standalone_graph/StandaloneModuleGraph.rs `inject` /
// `fromExecutable`, which write the module blob after the host image and end the
// file with `offsets` + the magic bytes). The virtual paths the embedded entry is
// exposed under are already modelled in mbun.bundler.standalone_graph.
//
// This module owns the container only: pack() takes the host image plus the
// already-bundled program and returns the bytes of the executable to write;
// unpack() takes the bytes of a running executable and returns the embedded
// program, or nullopt when the image is a plain (uncompiled) mbun. Both are pure
// string transforms so the format is unit-testable without touching the
// filesystem, and both are total — a truncated or foreign image yields nullopt
// rather than reading out of bounds, because unpack() runs on EVERY mbun start.
//
// Layout (all integers little-endian, so a bundle built on one host reads back
// identically on any little-endian target):
//
//   [ host image bytes ................................ ]
//   [ program: the bundled JS chunk .................... ] programSize bytes
//   [ metadata: entry name + exec argv ................. ] metadataSize bytes
//   [ trailer: programSize u64, metadataSize u64, MAGIC ] 24 bytes
//
// The metadata block is length-prefixed rather than JSON: it is written and read
// only here, and a binary block needs no escaping for entry names or argv values
// that contain quotes or newlines.
export module mbun.bundler.standalone_exe;

import std;

export namespace mbun::bundler::standalone_exe {

// 8 bytes at EOF. Versioned: a future layout change bumps the last byte so an
// executable produced by an older mbun is rejected instead of misread.
inline constexpr std::string_view MAGIC { "mbunSTA1" };
inline constexpr std::size_t TRAILER_SIZE { 8 + 8 + MAGIC.size() };

// The program embedded in a compiled executable.
struct Program {
    // The bundled JS chunk, run as the process entry point.
    std::string code;
    // Base name the entry is exposed under inside the virtual filesystem, e.g.
    // "entry.ts" → the runtime runs it as `/$bunfs/root/entry.ts`.
    std::string entryName;
    // `--compile-exec-argv=...`: the flags baked into the executable. They become
    // process.execArgv and are NOT visible in process.argv.
    // ref: bun-ref/src/cli/build_command.rs (compile_exec_argv).
    std::vector<std::string> execArgv;
    // `--no-compile-autoload-dotenv` and friends. Default true: a compiled bun
    // executable auto-loads .env / bunfig.toml from the cwd exactly as `bun run`
    // does unless the build turned it off.
    bool autoloadDotenv { true };
    bool autoloadBunfig { true };
    bool autoloadTsconfig { true };
    bool autoloadPackageJson { true };
};

namespace detail {

inline void put_u64(std::string& out, std::uint64_t value) {
    for (int i { 0 }; i < 8; ++i) {
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xffu));
    }
}

inline std::uint64_t get_u64(std::string_view bytes, std::size_t offset) {
    std::uint64_t value { 0 };
    for (int i { 0 }; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[offset + static_cast<std::size_t>(i)]))
              << (8 * i);
    }
    return value;
}

inline void put_string(std::string& out, std::string_view value) {
    put_u64(out, value.size());
    out.append(value);
}

// Reads a length-prefixed string, advancing `cursor`. Returns false (leaving the
// cursor unspecified) when the block is truncated, so a corrupt image is rejected
// instead of over-reading.
inline bool take_string(std::string_view bytes, std::size_t& cursor, std::string& out) {
    if (cursor + 8 > bytes.size()) {
        return false;
    }
    const std::uint64_t size { get_u64(bytes, cursor) };
    cursor += 8;
    if (size > bytes.size() - cursor) {
        return false;
    }
    out.assign(bytes.substr(cursor, static_cast<std::size_t>(size)));
    cursor += static_cast<std::size_t>(size);
    return true;
}

inline std::string encode_metadata(const Program& program) {
    std::string out;
    put_string(out, program.entryName);
    put_u64(out, program.execArgv.size());
    for (const std::string& arg : program.execArgv) {
        put_string(out, arg);
    }
    // One byte per autoload switch, appended after the argv block so an older
    // reader that stopped here still sees a well-formed prefix.
    out.push_back(program.autoloadDotenv ? '\1' : '\0');
    out.push_back(program.autoloadBunfig ? '\1' : '\0');
    out.push_back(program.autoloadTsconfig ? '\1' : '\0');
    out.push_back(program.autoloadPackageJson ? '\1' : '\0');
    return out;
}

inline bool decode_metadata(std::string_view bytes, Program& program) {
    std::size_t cursor { 0 };
    if (!take_string(bytes, cursor, program.entryName)) {
        return false;
    }
    if (cursor + 8 > bytes.size()) {
        return false;
    }
    const std::uint64_t count { get_u64(bytes, cursor) };
    cursor += 8;
    // A count larger than the remaining bytes cannot be honest: every entry costs
    // at least its 8-byte length prefix. Check before reserving so a corrupt
    // header cannot ask for an enormous allocation.
    if (count > (bytes.size() - cursor) / 8) {
        return false;
    }
    program.execArgv.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i { 0 }; i < count; ++i) {
        std::string arg;
        if (!take_string(bytes, cursor, arg)) {
            return false;
        }
        program.execArgv.push_back(std::move(arg));
    }
    if (cursor + 4 > bytes.size()) {
        return false;
    }
    program.autoloadDotenv = bytes[cursor + 0] != '\0';
    program.autoloadBunfig = bytes[cursor + 1] != '\0';
    program.autoloadTsconfig = bytes[cursor + 2] != '\0';
    program.autoloadPackageJson = bytes[cursor + 3] != '\0';
    return true;
}

} // namespace detail

// The host image with any previously embedded program removed. Compiling from an
// mbun that is itself a compiled executable would otherwise nest payloads and
// grow without bound; bun likewise re-uses only the base image.
std::string_view host_image(std::string_view executable) {
    if (executable.size() < TRAILER_SIZE) {
        return executable;
    }
    if (executable.substr(executable.size() - MAGIC.size()) != MAGIC) {
        return executable;
    }
    const std::size_t trailerStart { executable.size() - TRAILER_SIZE };
    const std::uint64_t programSize { detail::get_u64(executable, trailerStart) };
    const std::uint64_t metadataSize { detail::get_u64(executable, trailerStart + 8) };
    if (programSize > trailerStart || metadataSize > trailerStart - programSize) {
        return executable;
    }
    return executable.substr(0, trailerStart - static_cast<std::size_t>(programSize)
                                    - static_cast<std::size_t>(metadataSize));
}

// Bytes of the executable to write: the host image (stripped of any old payload)
// followed by the program, its metadata and the trailer.
std::string pack(std::string_view executable, const Program& program) {
    const std::string_view host { host_image(executable) };
    const std::string metadata { detail::encode_metadata(program) };
    std::string out;
    out.reserve(host.size() + program.code.size() + metadata.size() + TRAILER_SIZE);
    out.append(host);
    out.append(program.code);
    out.append(metadata);
    detail::put_u64(out, program.code.size());
    detail::put_u64(out, metadata.size());
    out.append(MAGIC);
    return out;
}

// Sizes read out of the final TRAILER_SIZE bytes of an executable: the program
// and metadata blocks that precede them. nullopt when those bytes are not a
// trailer of this container version.
//
// This exists so startup does NOT have to read the whole image: every mbun start
// asks "am I a compiled executable?", and the mbun binary is hundreds of
// megabytes. Reading it all to answer made every process start pay a full-image
// read — which showed up as `bun build` spawn storms timing out. The caller seeks
// to EOF, reads TRAILER_SIZE bytes, and only reads the payload when this returns.
struct TrailerSizes {
    std::uint64_t programSize { 0 };
    std::uint64_t metadataSize { 0 };
};

std::optional<TrailerSizes> read_trailer(std::string_view trailer) {
    if (trailer.size() != TRAILER_SIZE) {
        return std::nullopt;
    }
    if (trailer.substr(trailer.size() - MAGIC.size()) != MAGIC) {
        return std::nullopt;
    }
    return TrailerSizes { detail::get_u64(trailer, 0), detail::get_u64(trailer, 8) };
}

// The program held in `payload`, which must be exactly the programSize +
// metadataSize bytes that precede the trailer read by read_trailer().
std::optional<Program> read_payload(std::string_view payload, const TrailerSizes& sizes) {
    if (sizes.programSize > payload.size() || sizes.metadataSize != payload.size() - sizes.programSize) {
        return std::nullopt;
    }
    Program program;
    program.code.assign(payload.substr(0, static_cast<std::size_t>(sizes.programSize)));
    if (!detail::decode_metadata(payload.substr(static_cast<std::size_t>(sizes.programSize)), program)) {
        return std::nullopt;
    }
    return program;
}

// The program embedded in `executable`, or nullopt when there is none. Whole-image
// convenience wrapper over read_trailer/read_payload; it must never throw and
// never read out of bounds, because a foreign or truncated image reaches it too.
std::optional<Program> unpack(std::string_view executable) {
    if (executable.size() < TRAILER_SIZE) {
        return std::nullopt;
    }
    if (executable.substr(executable.size() - MAGIC.size()) != MAGIC) {
        return std::nullopt;
    }
    const std::size_t trailerStart { executable.size() - TRAILER_SIZE };
    const std::uint64_t programSize { detail::get_u64(executable, trailerStart) };
    const std::uint64_t metadataSize { detail::get_u64(executable, trailerStart + 8) };
    if (programSize > trailerStart || metadataSize > trailerStart - programSize) {
        return std::nullopt;
    }
    const std::size_t metadataStart { trailerStart - static_cast<std::size_t>(metadataSize) };
    const std::size_t programStart { metadataStart - static_cast<std::size_t>(programSize) };
    Program program;
    program.code.assign(executable.substr(programStart, static_cast<std::size_t>(programSize)));
    if (!detail::decode_metadata(executable.substr(metadataStart, static_cast<std::size_t>(metadataSize)),
                                 program)) {
        return std::nullopt;
    }
    return program;
}

} // namespace mbun::bundler::standalone_exe
