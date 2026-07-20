// extract_tarball.cppm — mbun.install.extract_tarball
//
// Port of bun's `src/install/extract_tarball.rs` (the npm-package tarball
// extractor used by `bun install`). This module reproduces the PURE-LOGIC
// core of that file:
//   - the ustar/GNU tar container parser (512-byte header blocks; name/size/
//     typeflag/mode/linkname; GNU long-name ('L') and long-link ('K')
//     extensions; ustar `prefix` field; base-256 large-size encoding),
//   - stripping the leading path component (npm tarballs wrap every entry in
//     `package/`; GitHub tarballs wrap in `<repo>-<sha>/`) — bun uses
//     libarchive's `depth_to_skip = 1`,
//   - path-traversal safety (reject `..` escapes and absolute paths so an
//     extracted entry can never land outside the destination root),
//   - the file/dir mode handling (bun masks file perms to `0o777` then ORs
//     `0o666`; the node-tar "if readable then listable" dir mode-fix).
//
// SEAMS (documented — see AGENTS.md 移植三段法):
//   1. GZIP INFLATE (WIRED). bun's `extract()` gunzips the `.tgz` (libdeflate,
//      then zlib fallback) before handing raw tar bytes to libarchive. Here
//      `extract_tgz_to_dir()` detects the gzip magic (0x1f 0x8b), inflates via
//      `mbun.core.compress::gzip_decompress`, and forwards the plain tar bytes
//      to `extract_tar_to_dir()`. Corrupt/truncated gzip input maps to
//      `TarError::GzipError`. Callers that already hold plain tar bytes use
//      `parse_tar()` / `extract_tar_to_dir()` directly.
//   2. FILESYSTEM WRITES. bun writes into an fd-relative temp dir via raw
//      `openat`/`mkdirat`/`symlinkat`, then renames into the cache. Here the
//      write path is a thin `std::filesystem` wrapper (`extract_tar_to_dir`);
//      the fd-relative + atomic-rename-into-cache machinery is the syscall seam.
//
// The parser itself is zero-copy: `TarEntry::data` is a `std::span` view into
// the caller's inflated buffer.
export module mbun.install.extract_tarball;

import std;
import mbun.core.compress;

namespace mbun::install {

// ── errors ──────────────────────────────────────────────────────────────────

export enum class TarError {
    Truncated,        // header/data ran past the end of the buffer
    BadChecksum,      // ustar header checksum mismatch
    BadOctalField,    // a numeric field held a non-octal byte
    NameTooLong,      // a GNU long-name record exceeded a sane cap
    GzipError,        // gzip (.tgz) input failed to inflate (corrupt/truncated)
    GzipTooLarge,     // gzip inflated past MAX_DECOMPRESSED_TARBALL_SIZE (bomb guard)
    PathEscape,       // an entry path escaped the destination root
    IoError,          // filesystem write failed (extract_tar_to_dir only)
};

// A downloaded registry tarball's gzip stream is fully attacker-controlled, so
// its inflated size is capped. ref: bun MAX_DECOMPRESSED_TARBALL_SIZE = 2 GiB
// (extract_tarball.rs:26), applied as zlib_entry.max_output_size (:343).
export inline constexpr std::size_t MAX_DECOMPRESSED_TARBALL_SIZE{
    std::size_t{2} * 1024 * 1024 * 1024};

// ── entry model ─────────────────────────────────────────────────────────────

export enum class TarEntryType {
    File,
    Directory,
    Symlink,
    HardLink,
    Other,   // char/block/fifo/etc — npm mode skips these
};

export struct TarEntry {
    std::string name;                    // full path (ustar prefix + name), pre-strip
    std::string linkname;                // symlink / hardlink target
    std::uint64_t size{0};
    std::uint32_t mode{0};               // permission bits from the header
    TarEntryType type{TarEntryType::Other};
    std::span<const std::byte> data;     // zero-copy view into the inflated input
};

// ── numeric field parsing ───────────────────────────────────────────────────

// Parse a tar numeric header field. Fields are ASCII octal, space/NUL padded.
// GNU uses base-256 for values that don't fit (indicated by the high bit of the
// first byte). Returns nullopt only on a malformed octal digit.
export inline std::optional<std::uint64_t>
parse_tar_numeric(std::span<const std::byte> field) {
    if (field.empty()) {
        return std::uint64_t{0};
    }
    // GNU base-256: top bit of first byte set.
    if ((std::to_integer<unsigned>(field[0]) & 0x80u) != 0) {
        std::uint64_t value{0};
        // first byte: mask off the flag bit
        value = std::to_integer<unsigned>(field[0]) & 0x7fu;
        for (std::size_t i{1}; i < field.size(); ++i) {
            value = (value << 8) | std::to_integer<unsigned>(field[i]);
        }
        return value;
    }
    std::uint64_t value{0};
    bool sawDigit{false};
    for (std::byte b : field) {
        char c{static_cast<char>(std::to_integer<unsigned>(b))};
        if (c == '\0' || c == ' ') {
            if (sawDigit) {
                break;  // trailing padding
            }
            continue;   // leading padding
        }
        if (c < '0' || c > '7') {
            return std::nullopt;
        }
        value = (value << 3) | static_cast<std::uint64_t>(c - '0');
        sawDigit = true;
    }
    return value;
}

// ── low-level header layout ─────────────────────────────────────────────────

namespace tar_detail {

constexpr std::size_t BLOCK{512};

// Trim a fixed-width, NUL-padded C string field to its logical content.
inline std::string_view cstr_field(std::span<const std::byte> field) {
    std::size_t len{0};
    while (len < field.size()
           && std::to_integer<unsigned>(field[len]) != 0) {
        ++len;
    }
    return std::string_view{reinterpret_cast<const char*>(field.data()), len};
}

inline std::span<const std::byte>
sub(std::span<const std::byte> block, std::size_t off, std::size_t n) {
    return block.subspan(off, n);
}

// True when a 512-byte block is entirely zero (archive terminator).
inline bool is_zero_block(std::span<const std::byte> block) {
    for (std::byte b : block) {
        if (std::to_integer<unsigned>(b) != 0) {
            return false;
        }
    }
    return true;
}

// ustar header checksum: sum of all bytes with the 8-byte chksum field treated
// as spaces. Accept both signed and unsigned interpretations (historical tars).
inline bool checksum_ok(std::span<const std::byte> block) {
    auto stored = parse_tar_numeric(sub(block, 148, 8));
    if (!stored) {
        return false;
    }
    std::uint32_t uSum{0};
    std::int32_t sSum{0};
    for (std::size_t i{0}; i < BLOCK; ++i) {
        unsigned v{(i >= 148 && i < 156)
                       ? 0x20u
                       : std::to_integer<unsigned>(block[i])};
        uSum += v;
        sSum += static_cast<std::int32_t>(static_cast<signed char>(
            (i >= 148 && i < 156) ? 0x20 : std::to_integer<unsigned>(block[i])));
    }
    auto want = static_cast<std::uint32_t>(*stored);
    return uSum == want || static_cast<std::uint32_t>(sSum) == want;
}

inline TarEntryType type_from_flag(char flag) {
    switch (flag) {
        case '0':
        case '\0':
        case '7':  // contiguous file, treat as file
            return TarEntryType::File;
        case '5':
            return TarEntryType::Directory;
        case '2':
            return TarEntryType::Symlink;
        case '1':
            return TarEntryType::HardLink;
        default:
            return TarEntryType::Other;
    }
}

}  // namespace tar_detail

// ── the tar container parser (already-inflated bytes) ───────────────────────

// Parse a plain (uncompressed) tar buffer into its entries. Zero-copy: each
// `TarEntry::data` is a view into `bytes`. GNU long-name / long-link records
// are consumed and applied to the following real entry. Pax ('x'/'g') extended
// headers are skipped (their data is not needed for npm extraction).
export inline std::expected<std::vector<TarEntry>, TarError>
parse_tar(std::span<const std::byte> bytes) {
    using namespace tar_detail;
    std::vector<TarEntry> entries;

    std::string pendingLongName;
    std::string pendingLongLink;
    bool haveLongName{false};
    bool haveLongLink{false};

    std::size_t pos{0};
    while (pos + BLOCK <= bytes.size()) {
        auto block = bytes.subspan(pos, BLOCK);
        if (is_zero_block(block)) {
            // A single zero block marks end-of-archive (two is canonical, but
            // real tars from libarchive/npm sometimes emit trailing junk we
            // don't need; stop at the first terminator).
            break;
        }
        pos += BLOCK;

        char typeflag{static_cast<char>(
            std::to_integer<unsigned>(block[156]))};

        // ustar magic check gates the checksum: pre-ustar "old tar" images in
        // the wild sometimes have loose checksums, but npm/GitHub always emit
        // ustar. Verify only when the magic says ustar.
        auto magic = cstr_field(sub(block, 257, 6));
        bool ustar{magic == "ustar" || magic.substr(0, 5) == "ustar"};
        if (ustar && !checksum_ok(block)) {
            return std::unexpected(TarError::BadChecksum);
        }

        auto sizeField = parse_tar_numeric(sub(block, 124, 12));
        if (!sizeField) {
            return std::unexpected(TarError::BadOctalField);
        }
        std::uint64_t size{*sizeField};
        std::size_t dataBlocks{(size + BLOCK - 1) / BLOCK};

        if (pos + dataBlocks * BLOCK < pos) {  // overflow guard
            return std::unexpected(TarError::Truncated);
        }
        if (pos + size > bytes.size()) {
            return std::unexpected(TarError::Truncated);
        }
        auto data = bytes.subspan(pos, static_cast<std::size_t>(size));

        // GNU long-name / long-link: the "name" of the *next* entry lives in
        // this record's data.
        if (typeflag == 'L') {
            if (size > (1u << 20)) {
                return std::unexpected(TarError::NameTooLong);
            }
            pendingLongName.assign(
                reinterpret_cast<const char*>(data.data()), data.size());
            // strip trailing NUL that GNU appends
            while (!pendingLongName.empty() && pendingLongName.back() == '\0') {
                pendingLongName.pop_back();
            }
            haveLongName = true;
            pos += dataBlocks * BLOCK;
            continue;
        }
        if (typeflag == 'K') {
            if (size > (1u << 20)) {
                return std::unexpected(TarError::NameTooLong);
            }
            pendingLongLink.assign(
                reinterpret_cast<const char*>(data.data()), data.size());
            while (!pendingLongLink.empty() && pendingLongLink.back() == '\0') {
                pendingLongLink.pop_back();
            }
            haveLongLink = true;
            pos += dataBlocks * BLOCK;
            continue;
        }
        if (typeflag == 'x' || typeflag == 'g') {
            // pax extended header — not needed for npm layout; skip its data.
            pos += dataBlocks * BLOCK;
            continue;
        }

        TarEntry entry;
        if (haveLongName) {
            entry.name = std::move(pendingLongName);
            pendingLongName.clear();
            haveLongName = false;
        } else {
            auto prefix = cstr_field(sub(block, 345, 155));
            auto name = cstr_field(sub(block, 0, 100));
            if (!prefix.empty()) {
                entry.name.reserve(prefix.size() + 1 + name.size());
                entry.name.append(prefix);
                entry.name.push_back('/');
                entry.name.append(name);
            } else {
                entry.name.assign(name);
            }
        }
        if (haveLongLink) {
            entry.linkname = std::move(pendingLongLink);
            pendingLongLink.clear();
            haveLongLink = false;
        } else {
            entry.linkname = std::string{cstr_field(sub(block, 157, 100))};
        }

        auto modeField = parse_tar_numeric(sub(block, 100, 8));
        if (!modeField) {
            return std::unexpected(TarError::BadOctalField);
        }
        entry.mode = static_cast<std::uint32_t>(*modeField);
        entry.size = size;
        entry.type = type_from_flag(typeflag);
        entry.data = data;

        entries.push_back(std::move(entry));
        pos += dataBlocks * BLOCK;
    }

    return entries;
}

// ── path handling (strip prefix + traversal safety) ─────────────────────────

// Strip the leading path component (`depth_to_skip = 1` in bun). npm tarballs
// wrap every entry in `package/`; GitHub tarballs wrap in `<repo>-<sha>/`.
// Returns the remainder after the first `/` (leading separators collapsed), or
// an empty string when the entry *is* just the wrapper directory.
export inline std::string strip_leading_component(std::string_view name) {
    std::size_t i{0};
    while (i < name.size() && (name[i] == '/' || name[i] == '\\')) {
        ++i;  // skip leading separators
    }
    while (i < name.size() && name[i] != '/' && name[i] != '\\') {
        ++i;  // skip the first component
    }
    while (i < name.size() && (name[i] == '/' || name[i] == '\\')) {
        ++i;  // skip separators after it
    }
    return std::string{name.substr(i)};
}

// Normalize an entry path and reject anything that would escape the extraction
// root. Collapses `.` and interior `..`; a `..` that would climb above the root
// (leading `..`) or an absolute path is rejected (nullopt). Mirrors the belt-
// and-braces check bun applies on top of the integrity gate.
export inline std::optional<std::string>
normalize_entry_path(std::string_view path) {
    if (path.empty()) {
        return std::string{};
    }
    // Absolute paths never allowed.
    if (path.front() == '/' || path.front() == '\\') {
        return std::nullopt;
    }
    // Windows drive-absolute (`C:\`, `C:/`).
    if (path.size() >= 2 && path[1] == ':') {
        return std::nullopt;
    }

    std::vector<std::string_view> stack;
    std::size_t start{0};
    auto flush = [&](std::size_t end) -> bool {
        std::string_view comp{path.substr(start, end - start)};
        if (comp.empty() || comp == ".") {
            return true;
        }
        if (comp == "..") {
            if (stack.empty()) {
                return false;  // escapes root
            }
            stack.pop_back();
            return true;
        }
        stack.push_back(comp);
        return true;
    };

    for (std::size_t i{0}; i < path.size(); ++i) {
        if (path[i] == '/' || path[i] == '\\') {
            if (!flush(i)) {
                return std::nullopt;
            }
            start = i + 1;
        }
    }
    if (!flush(path.size())) {
        return std::nullopt;
    }

    std::string out;
    for (std::size_t i{0}; i < stack.size(); ++i) {
        if (i != 0) {
            out.push_back('/');
        }
        out.append(stack[i]);
    }
    return out;
}

// ── mode handling ───────────────────────────────────────────────────────────

// File mode as bun applies it: mask the archived perms to 0o777 then OR 0o666
// so every extracted file is at least user/group readable+writable and setuid/
// setgid/sticky bits from the archive never reach the open() mode.
export inline std::uint32_t file_mode_for(std::uint32_t archivedMode) {
    return (archivedMode & 0777u) | 0666u;
}

// node-tar's mode-fix for directories: "if dirs are readable, then they should
// be listable" — mirror each read bit into the matching execute bit.
// https://github.com/npm/node-tar/blob/main/lib/mode-fix.js
export inline std::uint32_t dir_mode_fix(std::uint32_t mode) {
    if ((mode & 0400u) != 0) mode |= 0100u;
    if ((mode & 0040u) != 0) mode |= 0010u;
    if ((mode & 0004u) != 0) mode |= 0001u;
    return mode;
}

// ── gzip detection (the inflate seam) ───────────────────────────────────────

export inline bool looks_like_gzip(std::span<const std::byte> bytes) {
    return bytes.size() >= 2
           && std::to_integer<unsigned>(bytes[0]) == 0x1f
           && std::to_integer<unsigned>(bytes[1]) == 0x8b;
}

// ── extraction options + filesystem seam ────────────────────────────────────

export struct ExtractOptions {
    // libarchive `depth_to_skip`. npm/GitHub tarballs wrap entries one level
    // deep, so bun always passes 1. Set 0 to extract verbatim.
    unsigned depthToSkip{1};
    // npm mode: npm tarballs only contain files; every non-file entry is
    // skipped (matches Archiver.extractToDir's `npm: true`).
    bool npmOnlyFiles{true};
};

export struct ExtractResult {
    std::size_t filesWritten{0};
    std::size_t dirsCreated{0};
    std::size_t symlinksCreated{0};
    std::size_t skipped{0};
};

// Filesystem extraction (SEAM #2). Thin std::filesystem wrapper over the pure
// parser above: parse → strip prefix → normalize/traversal-check → write. bun's
// real path is fd-relative (openat/mkdirat/symlinkat into a temp dir) + an
// atomic rename into the content-addressed cache; that machinery is deferred.
//
// `bytes` must already be INFLATED tar. Use `extract_tgz_to_dir` for `.tgz`.
export inline std::expected<ExtractResult, TarError>
extract_tar_to_dir(std::span<const std::byte> bytes,
                   const std::filesystem::path& destDir,
                   const ExtractOptions& opts = {}) {
    auto parsed = parse_tar(bytes);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    ExtractResult result;
    std::error_code ec;
    std::filesystem::create_directories(destDir, ec);

    for (const TarEntry& entry : *parsed) {
        std::string rel{entry.name};
        for (unsigned d{0}; d < opts.depthToSkip; ++d) {
            rel = strip_leading_component(rel);
        }
        auto norm = normalize_entry_path(rel);
        if (!norm) {
            return std::unexpected(TarError::PathEscape);
        }
        if (norm->empty()) {
            ++result.skipped;
            continue;
        }
        if (opts.npmOnlyFiles && entry.type != TarEntryType::File
            && entry.type != TarEntryType::Directory) {
            ++result.skipped;
            continue;
        }

        std::filesystem::path outPath{destDir / std::filesystem::path{*norm}};
        switch (entry.type) {
            case TarEntryType::Directory: {
                std::filesystem::create_directories(outPath, ec);
                if (ec) return std::unexpected(TarError::IoError);
                std::filesystem::permissions(
                    outPath,
                    std::filesystem::perms{dir_mode_fix(entry.mode) & 0777u},
                    std::filesystem::perm_options::replace, ec);
                ++result.dirsCreated;
                break;
            }
            case TarEntryType::Symlink: {
                std::filesystem::create_directories(outPath.parent_path(), ec);
                std::filesystem::remove(outPath, ec);
                std::filesystem::create_symlink(
                    std::filesystem::path{entry.linkname}, outPath, ec);
                if (ec) return std::unexpected(TarError::IoError);
                ++result.symlinksCreated;
                break;
            }
            case TarEntryType::File: {
                std::filesystem::create_directories(outPath.parent_path(), ec);
                std::ofstream out{outPath, std::ios::binary | std::ios::trunc};
                if (!out) return std::unexpected(TarError::IoError);
                if (!entry.data.empty()) {
                    out.write(reinterpret_cast<const char*>(entry.data.data()),
                              static_cast<std::streamsize>(entry.data.size()));
                }
                out.close();
                std::filesystem::permissions(
                    outPath,
                    std::filesystem::perms{file_mode_for(entry.mode) & 0777u},
                    std::filesystem::perm_options::replace, ec);
                ++result.filesWritten;
                break;
            }
            default:
                ++result.skipped;
                break;
        }
    }
    return result;
}

// tgz entry point. Detects the gzip magic, gunzips via mbun.core.compress into
// a scratch buffer, and forwards the plain tar bytes to `extract_tar_to_dir`.
// Plain-tar input falls through to the parser directly.
// ref: extract_tarball.rs `extract()` — libdeflate/zlib gunzip before libarchive.
export inline std::expected<ExtractResult, TarError>
extract_tgz_to_dir(std::span<const std::byte> bytes,
                   const std::filesystem::path& destDir,
                   const ExtractOptions& opts = {}) {
    if (looks_like_gzip(bytes)) {
        auto inflated{mbun::core::compress::gzip_decompress(
            {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()},
            MAX_DECOMPRESSED_TARBALL_SIZE)};
        if (!inflated) {
            if (inflated.error() == mbun::core::compress::DECOMPRESS_LIMIT_ERROR) {
                return std::unexpected(TarError::GzipTooLarge);
            }
            return std::unexpected(TarError::GzipError);
        }
        return extract_tar_to_dir(
            {reinterpret_cast<const std::byte*>(inflated->data()), inflated->size()},
            destDir, opts);
    }
    return extract_tar_to_dir(bytes, destDir, opts);
}

}  // namespace mbun::install
