// tarball_stream.cppm — mbun.install.tarball_stream
//
// Port of the PURE-LOGIC subset of bun's `src/install/TarballStream.rs`, the
// resumable/streaming tarball extractor `bun install` uses to overlap download
// and extraction. The Rust file is dominated by machinery that has no place in
// a pure-logic port:
//   - libarchive's incremental gunzip+untar state machine (`archive_read_*`,
//     the ARCHIVE_RETRY resumption protocol),
//   - the HTTP-thread ↔ worker-thread handoff (mutex, `draining` atomic,
//     thread-pool `drain_task`), and
//   - the raw-pointer/Stacked-Borrows self-owning heap allocation.
// Those are the gzip-inflate and syscall/threading SEAMS. What survives — and
// is ported faithfully here — is the byte-level state machine and the path
// safety rules that decide, per entry, what lands on disk:
//   - the WantHeader → WantData → Done phase machine over tar entries, fed
//     incrementally as chunks of ALREADY-INFLATED tar bytes arrive,
//   - `tokenize_rest_after_first` (strip the leading `package/` component),
//   - the normalized-path traversal rejection (leading `..`, absolute),
//   - `is_symlink_target_safe` (bun's `make_symlink` target vetting: no
//     absolute targets, no `..` that climbs above the extraction root),
//   - `path_traverses_created_symlink` (skip entries whose path descends
//     through a symlink created earlier in the same extraction — the chained-
//     symlink escape defense),
//   - the npm-mode "files only" filter.
//
// SEAM: this reader consumes plain (inflated) tar bytes. The gzip layer that
// bun feeds through libarchive's filter is wired in later via
// `mbun.core.compress`.  // TODO: wire mbun.core.compress inflate upstream of feed().
export module mbun.install.tarball_stream;

import std;
import mbun.install.extract_tarball;

namespace mbun::install {

// ── streaming state machine ─────────────────────────────────────────────────

export enum class StreamPhase {
    WantHeader,  // call next() to read the next entry header
    WantData,    // (internal) an entry's body is being buffered
    Done,        // reader hit end-of-archive
};

// Incremental tar reader. Push inflated tar bytes with `feed()` (call `close()`
// once the last chunk has arrived), then pull complete entries with `next()`.
//
// `next()` returns:
//   - a `TarEntry` when a full header+body is buffered (its `data` span is
//     valid until the next `feed()`/`next()` call),
//   - `std::nullopt` when more bytes are needed (yield — bun returns the worker
//     to the pool here via ARCHIVE_RETRY),
//   - an `unexpected(TarError)` on a malformed header.
//
// Long-name ('L') / long-link ('K') GNU records and pax ('x'/'g') headers are
// consumed transparently, exactly as the buffered parser does.
export class StreamingTarReader {
private:
    std::vector<std::byte> buffer_;
    std::size_t pos_{0};
    bool closed_{false};
    StreamPhase phase_{StreamPhase::WantHeader};

    std::string pendingLongName_;
    std::string pendingLongLink_;
    bool haveLongName_{false};
    bool haveLongLink_{false};

    static constexpr std::size_t BLOCK{512};

public:
    StreamingTarReader() = default;

    void feed(std::span<const std::byte> chunk) {
        buffer_.insert(buffer_.end(), chunk.begin(), chunk.end());
    }

    void close() { closed_ = true; }

    StreamPhase phase() const { return phase_; }

    bool done() const { return phase_ == StreamPhase::Done; }

    // Bytes still unconsumed in the internal buffer.
    std::size_t buffered() const { return buffer_.size() - pos_; }

    std::optional<std::expected<TarEntry, TarError>> next() {
        while (true) {
            if (phase_ == StreamPhase::Done) {
                return std::nullopt;
            }
            std::size_t avail{buffer_.size() - pos_};
            if (avail < BLOCK) {
                return std::nullopt;  // need a full header block
            }
            std::span<const std::byte> block{buffer_.data() + pos_, BLOCK};

            // End-of-archive: a zero block.
            if (is_zero_block_(block)) {
                phase_ = StreamPhase::Done;
                return std::nullopt;
            }

            char typeflag{static_cast<char>(
                std::to_integer<unsigned>(block[156]))};

            auto sizeField = parse_tar_numeric(block.subspan(124, 12));
            if (!sizeField) {
                return std::expected<TarEntry, TarError>{
                    std::unexpect, TarError::BadOctalField};
            }
            std::uint64_t size{*sizeField};
            std::size_t dataBlocks{
                (static_cast<std::size_t>(size) + BLOCK - 1) / BLOCK};
            std::size_t need{BLOCK + dataBlocks * BLOCK};

            if (avail < need) {
                // Header is here but the body hasn't fully arrived yet. Yield
                // unless the stream is closed and can never provide more.
                if (closed_) {
                    return std::expected<TarEntry, TarError>{
                        std::unexpect, TarError::Truncated};
                }
                return std::nullopt;
            }

            std::span<const std::byte> data{
                buffer_.data() + pos_ + BLOCK,
                static_cast<std::size_t>(size)};

            // GNU long-name / long-link carry into the next real entry.
            if (typeflag == 'L') {
                assign_trimmed_(pendingLongName_, data);
                haveLongName_ = true;
                pos_ += need;
                continue;
            }
            if (typeflag == 'K') {
                assign_trimmed_(pendingLongLink_, data);
                haveLongLink_ = true;
                pos_ += need;
                continue;
            }
            if (typeflag == 'x' || typeflag == 'g') {
                pos_ += need;  // skip pax extended header
                continue;
            }

            TarEntry entry;
            if (haveLongName_) {
                entry.name = std::move(pendingLongName_);
                pendingLongName_.clear();
                haveLongName_ = false;
            } else {
                entry.name = full_name_(block);
            }
            if (haveLongLink_) {
                entry.linkname = std::move(pendingLongLink_);
                pendingLongLink_.clear();
                haveLongLink_ = false;
            } else {
                entry.linkname = cstr_(block.subspan(157, 100));
            }
            auto modeField = parse_tar_numeric(block.subspan(100, 8));
            entry.mode = modeField
                             ? static_cast<std::uint32_t>(*modeField)
                             : 0u;
            entry.size = size;
            entry.type = type_from_flag_(typeflag);
            entry.data = data;

            pos_ += need;
            return std::expected<TarEntry, TarError>{std::move(entry)};
        }
    }

private:
    static bool is_zero_block_(std::span<const std::byte> block) {
        for (std::byte b : block) {
            if (std::to_integer<unsigned>(b) != 0) {
                return false;
            }
        }
        return true;
    }
    static std::string cstr_(std::span<const std::byte> field) {
        std::size_t len{0};
        while (len < field.size()
               && std::to_integer<unsigned>(field[len]) != 0) {
            ++len;
        }
        return std::string{reinterpret_cast<const char*>(field.data()), len};
    }
    static std::string full_name_(std::span<const std::byte> block) {
        std::string prefix{cstr_(block.subspan(345, 155))};
        std::string name{cstr_(block.subspan(0, 100))};
        if (!prefix.empty()) {
            prefix.push_back('/');
            prefix.append(name);
            return prefix;
        }
        return name;
    }
    static void assign_trimmed_(std::string& dst,
                                std::span<const std::byte> data) {
        dst.assign(reinterpret_cast<const char*>(data.data()), data.size());
        while (!dst.empty() && dst.back() == '\0') {
            dst.pop_back();
        }
    }
    static TarEntryType type_from_flag_(char flag) {
        switch (flag) {
            case '0':
            case '\0':
            case '7':
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
};

// ── path helpers (ported from TarballStream.rs free functions) ──────────────

// `tokenize_rest_after_first`: skip leading `/`, drop the first path component,
// return everything after it. For "package/index.js" → "index.js". Identical
// intent to extract_tarball's `strip_leading_component`, provided here under
// the streaming-file's name so the mapping is explicit.
export inline std::string_view tokenize_rest_after_first(std::string_view s) {
    std::size_t i{0};
    while (i < s.size() && (s[i] == '/' || s[i] == '\\')) ++i;
    while (i < s.size() && s[i] != '/' && s[i] != '\\') ++i;
    while (i < s.size() && (s[i] == '/' || s[i] == '\\')) ++i;
    return s.substr(i);
}

namespace stream_detail {

inline std::string_view dirname(std::string_view path) {
    std::size_t slash{path.find_last_of("/\\")};
    if (slash == std::string_view::npos) {
        return std::string_view{};
    }
    return path.substr(0, slash);
}

// POSIX-style relative normalization that PRESERVES leading `..` (bun uses
// `normalize_string_generic_t::<u8, true, false>` — allowAboveRoot=true).
inline std::string normalize_relative_keep_updots(std::string_view input) {
    std::vector<std::string_view> out;
    int leadingUpdots{0};
    std::size_t start{0};
    auto handle = [&](std::string_view comp) {
        if (comp.empty() || comp == ".") {
            return;
        }
        if (comp == "..") {
            if (!out.empty()) {
                out.pop_back();
            } else {
                ++leadingUpdots;
            }
            return;
        }
        out.push_back(comp);
    };
    for (std::size_t i{0}; i < input.size(); ++i) {
        if (input[i] == '/' || input[i] == '\\') {
            handle(input.substr(start, i - start));
            start = i + 1;
        }
    }
    handle(input.substr(start));

    std::string result;
    for (int i{0}; i < leadingUpdots; ++i) {
        if (i != 0) result.push_back('/');
        result.append("..");
    }
    for (std::size_t i{0}; i < out.size(); ++i) {
        if (!result.empty()) result.push_back('/');
        result.append(out[i]);
    }
    return result;
}

}  // namespace stream_detail

// `is_symlink_target_safe` — bun's `make_symlink` vetting. A symlink at
// (already-stripped) `linkPath` pointing at `target` is safe to create only
// when the target cannot escape the extraction root:
//   - reject empty or absolute (`/`-leading) targets,
//   - reject a `..` component that follows a named component (would let the
//     link, once resolved, walk back out), and
//   - normalize `dirname(linkPath)/target` as a relative path keeping leading
//     `..`; reject if it resolves to `..` or `../…` (climbs above the root).
export inline bool is_symlink_target_safe(std::string_view linkPath,
                                          std::string_view target) {
    if (target.empty() || target.front() == '/') {
        return false;
    }
    bool seenNamed{false};
    std::size_t start{0};
    auto check = [&](std::string_view comp) -> bool {
        if (comp.empty() || comp == ".") {
            return true;
        }
        if (comp == "..") {
            return !seenNamed;  // a `..` after a named component is unsafe
        }
        seenNamed = true;
        return true;
    };
    for (std::size_t i{0}; i <= target.size(); ++i) {
        if (i == target.size() || target[i] == '/') {
            if (!check(target.substr(start, i - start))) {
                return false;
            }
            start = i + 1;
        }
    }

    std::string_view dir{stream_detail::dirname(linkPath)};
    std::string joined;
    if (!dir.empty()) {
        joined.append(dir);
        joined.push_back('/');
    }
    joined.append(target);
    std::string resolved{
        stream_detail::normalize_relative_keep_updots(joined)};
    if (resolved == ".." || resolved.starts_with("../")) {
        return false;
    }
    return true;
}

// `path_traverses_created_symlink` — reject an entry whose path descends
// through a symlink that a previous entry in this extraction already created.
// Purely lexical: `path` traverses `link` when `link` is a proper directory
// prefix of `path` (i.e. `path == link + "/" + …`). Once such a link is on
// disk the kernel would follow it, so a later entry could escape the root.
export inline bool
path_traverses_created_symlink(std::string_view path,
                               std::span<const std::string> createdSymlinks) {
    for (const std::string& link : createdSymlinks) {
        if (link.empty()) {
            continue;
        }
        if (path.size() > link.size()
            && path.starts_with(link)
            && (path[link.size()] == '/' || path[link.size()] == '\\')) {
            return true;
        }
    }
    return false;
}

// npm-mode filter: npm tarballs contain only files; every other entry kind is
// skipped (matches TarballStream's `npm_mode && kind != File` early-out).
export inline bool npm_mode_should_skip(TarEntryType type) {
    return type != TarEntryType::File;
}

}  // namespace mbun::install
