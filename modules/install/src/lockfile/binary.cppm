// lockfile/binary.cppm — mbun.install.lockfile.binary
//
// The bun.lockb binary format reader/writer. Mechanical port of bun
// src/install/lockfile/bun.lockb.rs (`save`/`load`, section tags, header) and
// lockfile/Buffers.rs (`write_array`/`read_array`, `Buffers::save`/`load`, the
// `Aligner` padding), plus the package-column serializer from
// lockfile/Package.rs (`serializer::save`/`load`).
//
// Layout: a shebang header + version, a u32 format, a 32-byte meta hash, a u64
// total-buffer-size backpatch slot, then the package struct-of-arrays, the side
// buffers, a trailing 0 u64, and a sequence of optional 8-byte-tagged sections
// (workspace versions/paths, trusted, overrides, patched, catalogs, config
// version). Every array payload is length-delimited by an absolute [start,end)
// u64 pair and padded to 8-byte (pointer) alignment. All integers are stored
// little-endian; the tag words are the byte strings read as LE u64 (byte-exact
// with bun on little-endian hosts).
//
// MC++ notes: writer and reader are separate types over a `std::vector<byte>` /
// `std::span<const byte>` (no aliased-`&mut` gymnastics the Rust needs); parse
// errors flow through `std::expected` instead of a `Result`.
export module mbun.install.lockfile.binary;

import std;
export import mbun.install.lockfile.model;
export import mbun.install.lockfile.migrate_v2;

namespace mbun::install::lockfile {

// ── error surface ───────────────────────────────────────────────────────────
export enum class BinaryError {
    InvalidLockfile,
    CorruptLockfile,
    UnexpectedVersion,
    OutdatedVersion,
    MissingData,
    MalformedTrailer,
    LengthMismatch,
    InvalidResolutionTag,
};

export template <class T>
using BinaryResult = std::expected<T, BinaryError>;

// Human-readable cause, so a failed `bun.lockb` parse names what was wrong
// instead of surfacing a bare enum. Wording follows bun's own messages
// (bun.lockb.rs / Package.rs `err!` strings).
export constexpr std::string_view binary_error_message(BinaryError e) noexcept {
    switch (e) {
        case BinaryError::InvalidLockfile: return "invalid lockfile";
        case BinaryError::CorruptLockfile: return "lockfile is corrupt";
        case BinaryError::UnexpectedVersion: return "unexpected lockfile version";
        case BinaryError::OutdatedVersion: return "outdated lockfile version";
        case BinaryError::MissingData: return "lockfile is missing data";
        case BinaryError::MalformedTrailer:
            return "lockfile is malformed (expected 0 at the end)";
        case BinaryError::LengthMismatch: return "lockfile has mismatched array lengths";
        case BinaryError::InvalidResolutionTag:
            return "lockfile validation failed: invalid resolution tag";
    }
    return "invalid lockfile";
}

// ── header + version + section tags (bun.lockb.rs) ──────────────────────────
inline constexpr std::string_view HEADER_BYTES{"#!/usr/bin/env bun\nbun-lockfile-format-v0\n"};
export inline constexpr std::string_view VERSION{"bun-lockfile-format-v0\n"};

// 144 zero bytes appended after the payload; also the source of the alignment
// padding (`Aligner`). ref: lib.rs ALIGNMENT_BYTES_TO_REPEAT_BUFFER.
inline constexpr std::size_t ALIGNMENT_BYTES_LEN{144};

// Native-endian 8-byte tag words, computed as little-endian so write/read via
// the LE integer helpers reproduce the exact ascii bytes on disk.
consteval std::uint64_t tag_of(const char (&s)[9]) {
    std::uint64_t v{0};
    for (std::size_t i{0}; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(s[i])) << (8 * i);
    }
    return v;
}
inline constexpr std::uint64_t HAS_PATCHED_DEPENDENCIES_TAG{tag_of("pAtChEdD")};
inline constexpr std::uint64_t HAS_WORKSPACE_PACKAGE_IDS_TAG{tag_of("wOrKsPaC")};
inline constexpr std::uint64_t HAS_TRUSTED_DEPENDENCIES_TAG{tag_of("tRuStEDd")};
inline constexpr std::uint64_t HAS_EMPTY_TRUSTED_DEPENDENCIES_TAG{tag_of("eMpTrUsT")};
inline constexpr std::uint64_t HAS_OVERRIDES_TAG{tag_of("oVeRriDs")};
inline constexpr std::uint64_t HAS_CATALOGS_TAG{tag_of("cAtAlOgS")};
inline constexpr std::uint64_t HAS_CONFIG_VERSION_TAG{tag_of("cNfGvRsN")};

// write_array prefix strings — byte-for-byte as bun emits them so re-saving an
// unchanged lockfile is a no-op. The reader skips them by absolute offset.
inline constexpr std::string_view PREFIX_U64{"\n<u64> 8 sizeof, 8 alignof\n"};
inline constexpr std::string_view PREFIX_U32{"\n<u32> 4 sizeof, 4 alignof\n"};
inline constexpr std::string_view PREFIX_U8{"\n<u8> 1 sizeof, 1 alignof\n"};
inline constexpr std::string_view PREFIX_DEP_EXTERNAL{"\n<[26]u8> 26 sizeof, 1 alignof\n"};
inline constexpr std::string_view PREFIX_SEMVER_VERSION{
    "\n<semver.Version.Version> 56 sizeof, 8 alignof\n"};
inline constexpr std::string_view PREFIX_SEMVER_STRING{"\n<semver.String> 8 sizeof, 1 alignof\n"};
inline constexpr std::string_view PREFIX_EXTERN_STRING{
    "\n<semver.ExternalString.ExternalString> 16 sizeof, 8 alignof\n"};
inline constexpr std::string_view PREFIX_TREE{"\n<install.lockfile.Tree> 20 sizeof, 4 alignof\n"};
inline constexpr std::string_view PREFIX_PATCHED_DEP{
    "\n<install.lockfile.PatchedDep> 24 sizeof, 8 alignof\n"};

constexpr std::uint32_t ARRAY_ALIGN{8};  // ALIGN_TYPE_0 = alignof(pointer)
constexpr std::uint64_t DEADBEEF{0xDEAD'BEEFull};

export bool is_binary_lockfile(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() < HEADER_BYTES.size()) {
        return false;
    }
    return std::string_view{reinterpret_cast<const char*>(bytes.data()), HEADER_BYTES.size()} ==
           HEADER_BYTES;
}

// ────────────────────────────────────────────────────────────────────────────
// Writer — append + positional (get_pos / pwrite), collapsing bun's
// StreamType(bytes) role.
// ────────────────────────────────────────────────────────────────────────────
export class Writer {
public:
    std::size_t pos() const noexcept { return bytes_.size(); }

    void write_bytes(std::span<const std::uint8_t> data) {
        bytes_.insert(bytes_.end(), data.begin(), data.end());
    }
    void write_str(std::string_view s) {
        bytes_.insert(bytes_.end(), s.begin(), s.end());
    }

    template <class T>
    void write_int_le(T v) {
        static_assert(std::is_unsigned_v<T>);
        for (std::size_t i{0}; i < sizeof(T); ++i) {
            bytes_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
        }
    }

    // Positional overwrite (never grows the buffer).
    void pwrite(std::span<const std::uint8_t> data, std::size_t index) {
        std::copy(data.begin(), data.end(), bytes_.begin() + static_cast<std::ptrdiff_t>(index));
    }
    void pwrite_int_le(std::uint64_t v, std::size_t index) {
        std::array<std::uint8_t, 8> tmp{};
        for (std::size_t i{0}; i < 8; ++i) {
            tmp[i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
        }
        pwrite(tmp, index);
    }

    // Pad to `align`-byte boundary with zero bytes (Aligner::write_with_align).
    void align_to(std::uint32_t align) {
        std::size_t p{pos()};
        std::size_t padded{(p + align - 1) / align * align};
        for (std::size_t i{p}; i < padded; ++i) {
            bytes_.push_back(0);
        }
    }

    std::vector<std::uint8_t> take() && { return std::move(bytes_); }
    const std::vector<std::uint8_t>& view() const noexcept { return bytes_; }

private:
    std::vector<std::uint8_t> bytes_;
};

// ────────────────────────────────────────────────────────────────────────────
// Reader — FixedBufferStream over borrowed bytes.
// ────────────────────────────────────────────────────────────────────────────
export class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> buf) : buf_{buf} {}

    std::size_t pos() const noexcept { return pos_; }
    void set_pos(std::size_t p) noexcept { pos_ = p; }
    std::size_t size() const noexcept { return buf_.size(); }
    std::span<const std::uint8_t> buffer() const noexcept { return buf_; }

    BinaryResult<std::size_t> read_all(std::span<std::uint8_t> out) {
        std::size_t n{std::min(out.size(), buf_.size() - std::min(pos_, buf_.size()))};
        std::copy_n(buf_.begin() + static_cast<std::ptrdiff_t>(pos_), n, out.begin());
        pos_ += n;
        return n;
    }

    template <class T>
    BinaryResult<T> read_int_le() {
        static_assert(std::is_unsigned_v<T>);
        if (pos_ + sizeof(T) > buf_.size()) {
            return std::unexpected(BinaryError::MissingData);
        }
        T v{0};
        for (std::size_t i{0}; i < sizeof(T); ++i) {
            v |= static_cast<T>(buf_[pos_ + i]) << (8 * i);
        }
        pos_ += sizeof(T);
        return v;
    }

private:
    std::span<const std::uint8_t> buf_;
    std::size_t pos_{0};
};

// ── write_array / read_array (Buffers.rs) ───────────────────────────────────
// Length-delimited, 8-byte-aligned payload with a [start,end) u64 header that is
// backpatched after the bytes are written.
template <class T>
void write_array(Writer& w, std::span<const T> arr, std::string_view prefix) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::size_t startPos{w.pos()};
    w.write_int_le<std::uint64_t>(DEADBEEF);
    w.write_int_le<std::uint64_t>(DEADBEEF);
    w.write_str(prefix);

    if (!arr.empty()) {
        w.align_to(ARRAY_ALIGN);
        std::uint64_t realStart{w.pos()};
        w.write_bytes({reinterpret_cast<const std::uint8_t*>(arr.data()), arr.size_bytes()});
        std::uint64_t realEnd{w.pos()};
        w.pwrite_int_le(realStart, startPos);
        w.pwrite_int_le(realEnd, startPos + 8);
    } else {
        std::uint64_t realEnd{w.pos()};
        w.pwrite_int_le(realEnd, startPos);
        w.pwrite_int_le(realEnd, startPos + 8);
    }
}

template <class T>
BinaryResult<std::vector<T>> read_array(Reader& r) {
    static_assert(std::is_trivially_copyable_v<T>);
    auto startR{r.read_int_le<std::uint64_t>()};
    if (!startR) {
        return std::unexpected(startR.error());
    }
    std::uint64_t startPos{*startR};
    if (startPos == DEADBEEF || startPos == 0) {
        return std::unexpected(BinaryError::CorruptLockfile);
    }
    // Must not go backwards (before the header we just consumed).
    std::uint64_t curBack{static_cast<std::uint64_t>(r.pos()) - 8};
    if (startPos < curBack) {
        return std::unexpected(BinaryError::CorruptLockfile);
    }
    auto endR{r.read_int_le<std::uint64_t>()};
    if (!endR) {
        return std::unexpected(endR.error());
    }
    std::uint64_t endPos{*endR};
    if (endPos == DEADBEEF || endPos == 0) {
        return std::unexpected(BinaryError::CorruptLockfile);
    }
    if (startPos > endPos || endPos > r.size()) {
        return std::unexpected(BinaryError::CorruptLockfile);
    }
    std::uint64_t byteLen{endPos - startPos};
    r.set_pos(static_cast<std::size_t>(endPos));
    if (byteLen == 0) {
        return std::vector<T>{};
    }
    if (startPos % alignof(T) != 0 || byteLen % sizeof(T) != 0) {
        return std::unexpected(BinaryError::CorruptLockfile);
    }
    std::vector<T> out(static_cast<std::size_t>(byteLen / sizeof(T)));
    std::memcpy(out.data(), r.buffer().data() + startPos, static_cast<std::size_t>(byteLen));
    return out;
}

// ── Tree <-> External (Tree.rs) ─────────────────────────────────────────────
inline TreeExternal tree_to_external(const Tree& t) {
    TreeExternal out{};
    auto put32 = [&](std::size_t at, std::uint32_t v) {
        for (std::size_t i{0}; i < 4; ++i) {
            out[at + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
        }
    };
    put32(0, t.id);
    put32(4, t.dependencyId);
    put32(8, t.parent);
    put32(12, t.dependencies.off);
    put32(16, t.dependencies.len);
    return out;
}
inline Tree tree_from_external(const TreeExternal& e) {
    auto get32 = [&](std::size_t at) {
        std::uint32_t v{0};
        for (std::size_t i{0}; i < 4; ++i) {
            v |= static_cast<std::uint32_t>(e[at + i]) << (8 * i);
        }
        return v;
    };
    Tree t{};
    t.id = get32(0);
    t.dependencyId = get32(4);
    t.parent = get32(8);
    t.dependencies = Slice{get32(12), get32(16)};
    return t;
}

// ── Buffers::save / load ────────────────────────────────────────────────────
inline void save_buffers(const Buffers& b, Writer& w) {
    std::vector<TreeExternal> treeExt;
    treeExt.reserve(b.trees.size());
    for (const auto& t : b.trees) {
        treeExt.push_back(tree_to_external(t));
    }
    write_array<TreeExternal>(w, treeExt, PREFIX_TREE);
    write_array<DependencyID>(w, b.hoistedDependencies, PREFIX_U32);
    write_array<PackageID>(w, b.resolutions, PREFIX_U32);
    write_array<DependencyExternal>(w, b.dependencies, PREFIX_DEP_EXTERNAL);
    write_array<SemverExternalString>(w, b.externStrings, PREFIX_EXTERN_STRING);
    write_array<std::uint8_t>(w, b.stringBytes, PREFIX_U8);
}

inline BinaryResult<Buffers> load_buffers(Reader& r) {
    Buffers b;
    auto trees{read_array<TreeExternal>(r)};
    if (!trees) {
        return std::unexpected(trees.error());
    }
    b.trees.reserve(trees->size());
    for (const auto& e : *trees) {
        b.trees.push_back(tree_from_external(e));
    }
    auto hoisted{read_array<DependencyID>(r)};
    if (!hoisted) {
        return std::unexpected(hoisted.error());
    }
    b.hoistedDependencies = std::move(*hoisted);
    auto res{read_array<PackageID>(r)};
    if (!res) {
        return std::unexpected(res.error());
    }
    b.resolutions = std::move(*res);
    auto deps{read_array<DependencyExternal>(r)};
    if (!deps) {
        return std::unexpected(deps.error());
    }
    b.dependencies = std::move(*deps);
    auto ext{read_array<SemverExternalString>(r)};
    if (!ext) {
        return std::unexpected(ext.error());
    }
    b.externStrings = std::move(*ext);
    auto sb{read_array<std::uint8_t>(r)};
    if (!sb) {
        return std::unexpected(sb.error());
    }
    b.stringBytes = std::move(*sb);
    return b;
}

// ── Package table serializer (Package.rs serializer::save / load) ───────────
template <class T>
void write_column(Writer& w, const std::vector<T>& col) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (!col.empty()) {
        w.write_bytes({reinterpret_cast<const std::uint8_t*>(col.data()), col.size() * sizeof(T)});
    }
}

inline void save_packages(const PackageList& list, Writer& w) {
    std::uint64_t n{list.len()};
    w.write_int_le<std::uint64_t>(n);
    w.write_int_le<std::uint64_t>(sizeof(void*));  // pointer alignment of the SoA bytes
    w.write_int_le<std::uint64_t>(PACKAGE_FIELD_COUNT);
    std::size_t beginAt{w.pos()};
    w.write_int_le<std::uint64_t>(0);
    std::size_t endAt{w.pos()};
    w.write_int_le<std::uint64_t>(0);
    w.align_to(static_cast<std::uint32_t>(sizeof(void*)));
    std::uint64_t reallyBegin{w.pos()};
    // Columns in PackageField::ALL order.
    write_column(w, list.name);
    write_column(w, list.nameHash);
    write_column(w, list.resolution);
    write_column(w, list.dependencies);
    write_column(w, list.resolutions);
    write_column(w, list.meta);
    write_column(w, list.bin);
    write_column(w, list.scripts);
    std::uint64_t reallyEnd{w.pos()};
    w.pwrite_int_le(reallyBegin, beginAt);
    w.pwrite_int_le(reallyEnd, endAt);
}

template <class T>
BinaryResult<std::monostate> read_column(Reader& r, std::vector<T>& col, std::size_t n,
                                          std::uint64_t endAt, bool fillZeroIfMissing) {
    std::size_t need{n * sizeof(T)};
    if (static_cast<std::uint64_t>(r.pos() + need) <= endAt) {
        col.resize(n);
        if (need > 0) {
            std::memcpy(col.data(), r.buffer().data() + r.pos(), need);
            r.set_pos(r.pos() + need);
        }
        return std::monostate{};
    }
    if (fillZeroIfMissing) {
        col.assign(n, T{});
        return std::monostate{};
    }
    return std::unexpected(BinaryError::CorruptLockfile);
}

// Read + tag-validate the `resolution` column at the element width the format
// dictates: `Resolution<u32>` (64B) for v2, `Resolution<u64>` (72B) for v3.
// ref: Package.rs `load_fields` — the tag byte is element[0] and the stride is
// `size_of::<ResolutionType<SemverIntType>>()` (Package.rs:3338), so reading a
// v2 table at the v3 stride desynchronizes on the *second* element.
//
// The accepted tag set {0,1,2,4,8,16,32,64,72,80,100} is bun's own, taken
// verbatim from Package.rs:3341 — it is exactly the named `Tag` consts of
// resolution.rs:927-961. (resolution.rs:909-913 makes `Tag` a u8 newtype rather
// than a `repr(u8)` enum so that *holding* an unnamed byte is not UB; the
// loader still rejects unnamed bytes here. The two are not in conflict.)
template <class ResT>
BinaryResult<std::vector<ResT>> read_resolution_column(Reader& r, std::size_t n,
                                                       std::uint64_t endAt) {
    const std::size_t need{n * sizeof(ResT)};
    if (static_cast<std::uint64_t>(r.pos() + need) > endAt) {
        return std::unexpected(BinaryError::CorruptLockfile);
    }
    const std::uint8_t* base{r.buffer().data() + r.pos()};
    for (std::size_t i{0}; i < n; ++i) {
        switch (base[i * sizeof(ResT)]) {
            case 0: case 1: case 2: case 4: case 8: case 16:
            case 32: case 64: case 72: case 80: case 100:
                break;
            default:
                return std::unexpected(BinaryError::InvalidResolutionTag);
        }
    }
    std::vector<ResT> col(n);
    if (need > 0) {
        std::memcpy(col.data(), base, need);
        r.set_pos(r.pos() + need);
    }
    return col;
}

// `migrateFromV2` selects the `Package<u32>` column widths and converts the
// resolution column up to `Package<u64>`. ref: Package.rs:3227-3292.
inline BinaryResult<PackageList> load_packages(Reader& r, std::size_t end, bool migrateFromV2) {
    PackageList list;
    auto lenR{r.read_int_le<std::uint64_t>()};
    if (!lenR) {
        return std::unexpected(lenR.error());
    }
    std::uint64_t listLen{*lenR};
    if (listLen > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) - 1) {
        return std::unexpected(BinaryError::InvalidLockfile);
    }
    auto alignR{r.read_int_le<std::uint64_t>()};
    if (!alignR) {
        return std::unexpected(alignR.error());
    }
    if (*alignR != sizeof(void*)) {
        return std::unexpected(BinaryError::InvalidLockfile);
    }
    auto fcR{r.read_int_le<std::uint64_t>()};
    if (!fcR) {
        return std::unexpected(fcR.error());
    }
    std::size_t fieldCount{static_cast<std::size_t>(*fcR)};
    bool scriptsAbsent{false};
    if (fieldCount == PACKAGE_FIELD_COUNT) {
        // ok
    } else if (fieldCount == PACKAGE_FIELD_COUNT - 1) {
        scriptsAbsent = true;  // pre-v0.6.8 lockfiles omit the scripts column
    } else {
        return std::unexpected(BinaryError::InvalidLockfile);
    }
    auto beginR{r.read_int_le<std::uint64_t>()};
    auto endR{r.read_int_le<std::uint64_t>()};
    if (!beginR || !endR) {
        return std::unexpected(BinaryError::MissingData);
    }
    std::uint64_t beginAt{*beginR};
    std::uint64_t endAt{*endR};
    if (beginAt > end || endAt > end || beginAt > endAt) {
        return std::unexpected(BinaryError::InvalidLockfile);
    }
    r.set_pos(static_cast<std::size_t>(beginAt));
    std::size_t n{static_cast<std::size_t>(listLen)};

#define MBUN_READ_COL(col, zeroFill)                                    \
    if (auto rc{read_column(r, list.col, n, endAt, zeroFill)}; !rc) {   \
        return std::unexpected(rc.error());                            \
    }
    MBUN_READ_COL(name, false)
    MBUN_READ_COL(nameHash, false)
    if (migrateFromV2) {
        auto oldCol{read_resolution_column<PackageResolutionV2>(r, n, endAt)};
        if (!oldCol) {
            return std::unexpected(oldCol.error());
        }
        list.resolution.reserve(n);
        for (const auto& old : *oldCol) {
            list.resolution.push_back(migrate_resolution(old));
        }
    } else {
        auto col{read_resolution_column<PackageResolution>(r, n, endAt)};
        if (!col) {
            return std::unexpected(col.error());
        }
        list.resolution = std::move(*col);
    }
    MBUN_READ_COL(dependencies, false)
    MBUN_READ_COL(resolutions, false)
    MBUN_READ_COL(meta, false)
    MBUN_READ_COL(bin, false)
    MBUN_READ_COL(scripts, scriptsAbsent)
#undef MBUN_READ_COL
    return list;
}

// ── read a tagged section header, rewinding if it does not match ────────────
// Returns true if `tag` matched and was consumed; otherwise rewinds 8 bytes.
inline BinaryResult<bool> peek_tag(Reader& r, std::uint64_t total, std::uint64_t tag,
                                   bool allowEqual) {
    std::uint64_t remaining{
        total > r.pos() ? total - static_cast<std::uint64_t>(r.pos()) : 0};
    bool haveRoom{allowEqual ? (remaining >= 8) : (remaining > 8)};
    if (!haveRoom || total > r.size()) {
        return false;
    }
    auto nextR{r.read_int_le<std::uint64_t>()};
    if (!nextR) {
        return std::unexpected(nextR.error());
    }
    if (*nextR == tag) {
        return true;
    }
    r.set_pos(r.pos() - 8);
    return false;
}

// ────────────────────────────────────────────────────────────────────────────
// Top-level save / load
// ────────────────────────────────────────────────────────────────────────────

// Serialize `lf` to a full bun.lockb byte buffer. `configVersion` is written in
// the trailing config-version section (defaults to CURRENT if nullopt).
export std::vector<std::uint8_t> save(const Lockfile& lf, std::uint64_t configVersion) {
    Writer w;
    w.write_str(HEADER_BYTES);
    w.write_int_le<std::uint32_t>(lf.format.value);
    w.write_bytes(lf.metaHash);

    std::size_t endPos{w.pos()};  // total-buffer-size backpatch slot
    w.write_int_le<std::uint64_t>(0);

    save_packages(lf.packages, w);
    save_buffers(lf.buffers, w);
    w.write_int_le<std::uint64_t>(0);

    // < Bun v1.0.4 stopped here; the extra tags say "there's more data".
    if (!lf.workspaceVersionKeys.empty()) {
        w.write_int_le<std::uint64_t>(HAS_WORKSPACE_PACKAGE_IDS_TAG);
        write_array<PackageNameHash>(w, lf.workspaceVersionKeys, PREFIX_U64);
        write_array<SemverVersion>(w, lf.workspaceVersionVals, PREFIX_SEMVER_VERSION);
        write_array<PackageNameHash>(w, lf.workspacePathKeys, PREFIX_U64);
        write_array<SemverString>(w, lf.workspacePathVals, PREFIX_SEMVER_STRING);
    }

    if (lf.trustedDependencies) {
        if (!lf.trustedDependencies->empty()) {
            w.write_int_le<std::uint64_t>(HAS_TRUSTED_DEPENDENCIES_TAG);
            write_array<TruncatedPackageNameHash>(w, *lf.trustedDependencies, PREFIX_U32);
        } else {
            w.write_int_le<std::uint64_t>(HAS_EMPTY_TRUSTED_DEPENDENCIES_TAG);
        }
    }

    if (!lf.overrideKeys.empty()) {
        w.write_int_le<std::uint64_t>(HAS_OVERRIDES_TAG);
        write_array<PackageNameHash>(w, lf.overrideKeys, PREFIX_U64);
        write_array<DependencyExternal>(w, lf.overrideVals, PREFIX_DEP_EXTERNAL);
    }

    if (!lf.patchedKeys.empty()) {
        w.write_int_le<std::uint64_t>(HAS_PATCHED_DEPENDENCIES_TAG);
        write_array<PackageNameAndVersionHash>(w, lf.patchedKeys, PREFIX_U64);
        write_array<PatchedDepExternal>(w, lf.patchedVals, PREFIX_PATCHED_DEP);
    }

    if (lf.has_catalogs()) {
        w.write_int_le<std::uint64_t>(HAS_CATALOGS_TAG);
        write_array<SemverString>(w, lf.catalogDefaultNames, PREFIX_SEMVER_STRING);
        write_array<DependencyExternal>(w, lf.catalogDefaultDeps, PREFIX_DEP_EXTERNAL);
        write_array<SemverString>(w, lf.catalogGroupNames, PREFIX_SEMVER_STRING);
        for (const auto& group : lf.catalogGroups) {
            write_array<SemverString>(w, group.depNames, PREFIX_SEMVER_STRING);
            write_array<DependencyExternal>(w, group.deps, PREFIX_DEP_EXTERNAL);
        }
    }

    w.write_int_le<std::uint64_t>(HAS_CONFIG_VERSION_TAG);
    w.write_int_le<std::uint64_t>(configVersion);

    std::uint64_t totalSize{w.pos()};
    w.pwrite_int_le(totalSize, endPos);  // backpatch total-buffer-size

    // Trailing alignment padding (never read).
    for (std::size_t i{0}; i < ALIGNMENT_BYTES_LEN; ++i) {
        w.write_int_le<std::uint8_t>(0);
    }
    return std::move(w).take();
}

// Parse a full bun.lockb byte buffer into a Lockfile.
export BinaryResult<Lockfile> load(std::span<const std::uint8_t> bytes) {
    Reader r{bytes};
    Lockfile lf;

    std::array<std::uint8_t, HEADER_BYTES.size()> headerBuf{};
    auto hn{r.read_all(headerBuf)};
    if (!hn) {
        return std::unexpected(hn.error());
    }
    if (*hn != HEADER_BYTES.size() ||
        std::string_view{reinterpret_cast<const char*>(headerBuf.data()), *hn} != HEADER_BYTES) {
        return std::unexpected(BinaryError::InvalidLockfile);
    }

    auto fmtR{r.read_int_le<std::uint32_t>()};
    if (!fmtR) {
        return std::unexpected(fmtR.error());
    }
    std::uint32_t format{*fmtR};
    if (format > FormatVersion::current().value) {
        return std::unexpected(BinaryError::UnexpectedVersion);
    }
    // Only v2 may be migrated forward. ref: bun.lockb.rs:376-389 — the flag this
    // computes is threaded into `package::serializer::load` (line 402), which is
    // what selects the `Package<u32>` column widths. Accepting v2 at the version
    // gate but then parsing at v3 widths is the bug this branch exists to avoid.
    bool migrateFromV2{false};
    if (format < FormatVersion::current().value) {
        if (format != FormatVersion::v2().value) {
            return std::unexpected(BinaryError::OutdatedVersion);
        }
        migrateFromV2 = true;
    }
    lf.format = FormatVersion::current();
    lf.migratedFromLockbV2 = migrateFromV2;

    auto mh{r.read_all(lf.metaHash)};
    if (!mh) {
        return std::unexpected(mh.error());
    }

    auto tbsR{r.read_int_le<std::uint64_t>()};
    if (!tbsR) {
        return std::unexpected(tbsR.error());
    }
    std::uint64_t totalBufferSize{*tbsR};
    if (totalBufferSize > r.size()) {
        return std::unexpected(BinaryError::MissingData);
    }

    auto pkgs{load_packages(r, static_cast<std::size_t>(totalBufferSize), migrateFromV2)};
    if (!pkgs) {
        return std::unexpected(pkgs.error());
    }
    lf.packages = std::move(*pkgs);
    // meta.id range validation is deferred to reconciliation (Meta is opaque here).

    auto bufs{load_buffers(r)};
    if (!bufs) {
        return std::unexpected(bufs.error());
    }
    lf.buffers = std::move(*bufs);

    auto trailer{r.read_int_le<std::uint64_t>()};
    if (!trailer) {
        return std::unexpected(trailer.error());
    }
    if (*trailer != 0) {
        return std::unexpected(BinaryError::MalformedTrailer);
    }

    // Optional workspace versions/paths section.
    {
        auto matched{peek_tag(r, totalBufferSize, HAS_WORKSPACE_PACKAGE_IDS_TAG, false)};
        if (!matched) {
            return std::unexpected(matched.error());
        }
        if (*matched) {
            auto vk{read_array<PackageNameHash>(r)};
            if (!vk) {
                return std::unexpected(vk.error());
            }
            // The second width-dependent array: v2 stores `VersionType<u32>`
            // (48B), v3 `VersionType<u64>` (56B). ref: bun.lockb.rs:443-460.
            std::vector<SemverVersion> versions;
            if (migrateFromV2) {
                auto oldVv{read_array<SemverVersionV2>(r)};
                if (!oldVv) {
                    return std::unexpected(oldVv.error());
                }
                versions.reserve(oldVv->size());
                for (const auto& old : *oldVv) {
                    versions.push_back(migrate_version(old));
                }
            } else {
                auto vv{read_array<SemverVersion>(r)};
                if (!vv) {
                    return std::unexpected(vv.error());
                }
                versions = std::move(*vv);
            }
            if (vk->size() != versions.size()) {
                return std::unexpected(BinaryError::LengthMismatch);
            }
            lf.workspaceVersionKeys = std::move(*vk);
            lf.workspaceVersionVals = std::move(versions);
            auto pk{read_array<PackageNameHash>(r)};
            auto pv{read_array<SemverString>(r)};
            if (!pk || !pv) {
                return std::unexpected(!pk ? pk.error() : pv.error());
            }
            if (pk->size() != pv->size()) {
                return std::unexpected(BinaryError::LengthMismatch);
            }
            lf.workspacePathKeys = std::move(*pk);
            lf.workspacePathVals = std::move(*pv);
        }
    }

    // Optional trusted-dependencies section (>= 8 because the empty tag is tag-only).
    {
        std::uint64_t remaining{
            totalBufferSize > r.pos() ? totalBufferSize - r.pos() : 0};
        if (remaining >= 8 && totalBufferSize <= r.size()) {
            auto nextR{r.read_int_le<std::uint64_t>()};
            if (!nextR) {
                return std::unexpected(nextR.error());
            }
            if (remaining > 8 && *nextR == HAS_TRUSTED_DEPENDENCIES_TAG) {
                auto td{read_array<TruncatedPackageNameHash>(r)};
                if (!td) {
                    return std::unexpected(td.error());
                }
                lf.trustedDependencies = std::move(*td);
            } else if (*nextR == HAS_EMPTY_TRUSTED_DEPENDENCIES_TAG) {
                lf.trustedDependencies = std::vector<TruncatedPackageNameHash>{};
            } else {
                r.set_pos(r.pos() - 8);
            }
        }
    }

    // Optional overrides section.
    {
        auto matched{peek_tag(r, totalBufferSize, HAS_OVERRIDES_TAG, false)};
        if (!matched) {
            return std::unexpected(matched.error());
        }
        if (*matched) {
            auto k{read_array<PackageNameHash>(r)};
            auto v{read_array<DependencyExternal>(r)};
            if (!k || !v) {
                return std::unexpected(!k ? k.error() : v.error());
            }
            lf.overrideKeys = std::move(*k);
            lf.overrideVals = std::move(*v);
        }
    }

    // Optional patched-dependencies section.
    {
        auto matched{peek_tag(r, totalBufferSize, HAS_PATCHED_DEPENDENCIES_TAG, false)};
        if (!matched) {
            return std::unexpected(matched.error());
        }
        if (*matched) {
            auto k{read_array<PackageNameAndVersionHash>(r)};
            auto v{read_array<PatchedDepExternal>(r)};
            if (!k || !v) {
                return std::unexpected(!k ? k.error() : v.error());
            }
            lf.patchedKeys = std::move(*k);
            lf.patchedVals = std::move(*v);
        }
    }

    // Optional catalogs section.
    {
        auto matched{peek_tag(r, totalBufferSize, HAS_CATALOGS_TAG, false)};
        if (!matched) {
            return std::unexpected(matched.error());
        }
        if (*matched) {
            auto dn{read_array<SemverString>(r)};
            auto dd{read_array<DependencyExternal>(r)};
            if (!dn || !dd) {
                return std::unexpected(!dn ? dn.error() : dd.error());
            }
            lf.catalogDefaultNames = std::move(*dn);
            lf.catalogDefaultDeps = std::move(*dd);
            auto gn{read_array<SemverString>(r)};
            if (!gn) {
                return std::unexpected(gn.error());
            }
            lf.catalogGroupNames = *gn;
            for (std::size_t i{0}; i < gn->size(); ++i) {
                auto cn{read_array<SemverString>(r)};
                auto cd{read_array<DependencyExternal>(r)};
                if (!cn || !cd) {
                    return std::unexpected(!cn ? cn.error() : cd.error());
                }
                lf.catalogGroups.push_back(CatalogGroup{std::move(*cn), std::move(*cd)});
            }
        }
    }

    // Optional config-version section.
    {
        auto matched{peek_tag(r, totalBufferSize, HAS_CONFIG_VERSION_TAG, false)};
        if (!matched) {
            return std::unexpected(matched.error());
        }
        if (*matched) {
            auto cv{r.read_int_le<std::uint64_t>()};
            if (!cv) {
                return std::unexpected(cv.error());
            }
            lf.savedConfigVersion = *cv;
        }
    }

    return lf;
}

}  // namespace mbun::install::lockfile
