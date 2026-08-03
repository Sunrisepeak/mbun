// apply.cppm — mbun.patch.apply: apply a parsed PatchFile.
//
// Ported from bun's src/patch/lib.rs (PatchFile::apply + apply_patch). The
// line-splicing algorithm is pure and translated 1:1. All real filesystem
// effects (open/write/unlink/rename/chmod/mkdir) are injected through the
// `FileSystem` callback struct — same pattern as modules/resolver — so the
// applier is unit-testable off a real disk.
//
// DEFERRED: the concrete disk-backed FileSystem (bun_sys openat/fstatat/…,
// patch_dir fd handling, Windows abs-path join) is layered on later at the
// runtime binding. Here it is an injected interface only.
export module mbun.patch.apply;

import std;
import mbun.patch.types;
import mbun.core.paths;

namespace mbun::patch {

// Result of a stat on the target file (only the fields the applier needs).
export struct FileStat {
    std::uint32_t mode { 0 };
    std::uint64_t size { 0 };
};

// Injected filesystem. Every path is relative to the patch directory. A bool
// return of false (or nullopt) signals an I/O failure and aborts the apply.
export struct FileSystem {
    std::function<std::optional<FileStat>(std::string_view path)> stat;
    std::function<std::optional<std::string>(std::string_view path)> read_file;
    // Create/overwrite `path` (O_CREAT|WRONLY|TRUNC) with `contents` and `mode`.
    std::function<bool(std::string_view path, std::string_view contents, std::uint32_t mode)>
        write_file;
    std::function<bool(std::string_view path)> unlink;
    std::function<bool(std::string_view from, std::string_view to)> rename;
    // mkdir -p `path` with `mode`.
    std::function<bool(std::string_view path, std::uint32_t mode)> mkdir_recursive;
    std::function<bool(std::string_view path, std::uint32_t mode)> chmod;
};

export enum class ApplyErrorKind {
    UnsafePath,  // path escaped the patch dir (EINVAL in bun)
    Io,          // an injected fs callback failed
    Invalid,     // hunk did not line up with the target file
};

export struct ApplyError {
    ApplyErrorKind kind;
    std::string path;
};

namespace {

bool is_absolute_loose_(std::string_view p) {
    if (mbun::core::paths::posix::is_absolute(p)) {
        return true;
    }
    if (!p.empty() && p[0] == '\\') {
        return true;
    }
    if (p.size() >= 2 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':') {
        return true;
    }
    return false;
}

// Reject empty, absolute, or any path with a ".." segment (bun is_safe_patch_path).
bool is_safe_patch_path_(std::string_view path) {
    if (path.empty()) {
        return false;
    }
    if (is_absolute_loose_(path)) {
        return false;
    }
    std::size_t start { 0 };
    for (std::size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/' || path[i] == '\\') {
            if (path.substr(start, i - start) == "..") {
                return false;
            }
            start = i + 1;
        }
    }
    return true;
}

// Everything before the last '/' or '\\'; empty if none (bun dirname_simple).
std::string_view dirname_simple_(std::string_view p) {
    auto pos = p.find_last_of("/\\");
    if (pos == std::string_view::npos) {
        return {};
    }
    return p.substr(0, pos);
}

// Split on '\n' with Rust `slice::split` semantics: N delimiters => N+1 pieces
// (a trailing '\n' yields a final empty piece).
std::vector<std::string_view> split_lines_(std::string_view buf) {
    std::vector<std::string_view> lines;
    std::size_t start { 0 };
    while (true) {
        auto pos = buf.find('\n', start);
        if (pos == std::string_view::npos) {
            lines.push_back(buf.substr(start));
            break;
        }
        lines.push_back(buf.substr(start, pos - start));
        start = pos + 1;
    }
    return lines;
}

// Join line slices with '\n' (bun join_bytes).
std::string join_lines_(const std::vector<std::string_view>& lines) {
    if (lines.empty()) {
        return {};
    }
    std::size_t total { 0 };
    for (const auto& l : lines) {
        total += l.size();
    }
    total += lines.size() - 1;
    std::string out;
    out.reserve(total);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i != 0) {
            out.push_back('\n');
        }
        out.append(lines[i]);
    }
    return out;
}

// Build the byte contents of a freshly-created file from its single hunk part
// (bun FileCreation content assembly).
std::string build_creation_contents_(const PatchMutationPart& first_part) {
    std::size_t last_line = first_part.lines.empty() ? 0 : first_part.lines.size() - 1;
    bool no_nl = first_part.no_newline_at_end_of_file;
    std::string contents;
    std::size_t total { 0 };
    for (std::size_t i = 0; i < first_part.lines.size(); ++i) {
        total += first_part.lines[i].size();
        total += (i < last_line) ? 1 : 0;
    }
    total += no_nl ? 0 : 1;
    contents.reserve(total);
    for (std::size_t idx = 0; idx < first_part.lines.size(); ++idx) {
        contents.append(first_part.lines[idx]);
        if (idx < last_line || !no_nl) {
            contents.push_back('\n');
        }
    }
    return contents;
}

std::optional<ApplyError> apply_patch_(const FilePatch& patch, const FileSystem& fs) {
    auto stat = fs.stat(patch.path);
    if (!stat) {
        return ApplyError { ApplyErrorKind::Io, std::string { patch.path } };
    }
    auto filebuf_opt = fs.read_file(patch.path);
    if (!filebuf_opt) {
        return ApplyError { ApplyErrorKind::Invalid, std::string { patch.path } };
    }
    std::string filebuf = std::move(*filebuf_opt);

    std::vector<std::string_view> lines = split_lines_(filebuf);

    for (const auto& hunk : patch.hunks) {
        std::size_t line_cursor = static_cast<std::size_t>(hunk.header.patched.start - 1);

        if (line_cursor > lines.size()) {
            return ApplyError { ApplyErrorKind::Invalid, std::string { patch.path } };
        }

        for (const auto& part : hunk.parts) {
            switch (part.ty) {
            case PartType::Context: {
                if (line_cursor + part.lines.size() > lines.size()) {
                    return ApplyError { ApplyErrorKind::Invalid, std::string { patch.path } };
                }
                line_cursor += part.lines.size();
                break;
            }
            case PartType::Insertion: {
                if (line_cursor > lines.size()) {
                    return ApplyError { ApplyErrorKind::Invalid, std::string { patch.path } };
                }
                lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(line_cursor),
                             part.lines.begin(), part.lines.end());
                line_cursor += part.lines.size();
                if (part.no_newline_at_end_of_file && !lines.empty()) {
                    lines.pop_back();
                }
                break;
            }
            case PartType::Deletion: {
                if (line_cursor + part.lines.size() > lines.size()) {
                    return ApplyError { ApplyErrorKind::Invalid, std::string { patch.path } };
                }
                lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(line_cursor),
                            lines.begin()
                                + static_cast<std::ptrdiff_t>(line_cursor + part.lines.size()));
                if (part.no_newline_at_end_of_file) {
                    lines.push_back(std::string_view {});
                }
                break;
            }
            }
        }
    }

    std::string contents = join_lines_(lines);
    if (!fs.write_file(patch.path, contents, stat->mode)) {
        return ApplyError { ApplyErrorKind::Io, std::string { patch.path } };
    }
    return std::nullopt;
}

}  // namespace

// Apply every part of a parsed patch, in order. Returns nullopt on success or
// the first error encountered (bun PatchFile::apply).
export std::optional<ApplyError> apply(const PatchFile& patch_file, const FileSystem& fs) {
    for (const auto& part : patch_file.parts) {
        if (const auto* fd = std::get_if<FileDeletion>(&part)) {
            if (!is_safe_patch_path_(fd->path)) {
                return ApplyError { ApplyErrorKind::UnsafePath, std::string { fd->path } };
            }
            if (!fs.unlink(fd->path)) {
                return ApplyError { ApplyErrorKind::Io, std::string { fd->path } };
            }
        } else if (const auto* fr = std::get_if<FileRename>(&part)) {
            if (!is_safe_patch_path_(fr->from_path) || !is_safe_patch_path_(fr->to_path)) {
                return ApplyError { ApplyErrorKind::UnsafePath, std::string { fr->to_path } };
            }
            std::string_view todir = dirname_simple_(fr->to_path);
            if (!todir.empty()) {
                if (!fs.mkdir_recursive(todir, 0755)) {
                    return ApplyError { ApplyErrorKind::Io, std::string { todir } };
                }
            }
            if (!fs.rename(fr->from_path, fr->to_path)) {
                return ApplyError { ApplyErrorKind::Io, std::string { fr->to_path } };
            }
        } else if (const auto* fc = std::get_if<FileCreation>(&part)) {
            if (!is_safe_patch_path_(fc->path)) {
                return ApplyError { ApplyErrorKind::UnsafePath, std::string { fc->path } };
            }
            std::uint32_t mode = file_mode_to_bun_mode(fc->mode);
            std::string_view filedir = dirname_simple_(fc->path);
            if (!filedir.empty()) {
                if (!fs.mkdir_recursive(filedir, mode)) {
                    return ApplyError { ApplyErrorKind::Io, std::string { filedir } };
                }
            }
            // Assemble contents (empty file when no hunk / a `@@ -0,0 +0,0 @@`
            // header with no body).
            std::string contents;
            if (fc->hunk && !fc->hunk->parts.empty()) {
                contents = build_creation_contents_(fc->hunk->parts.front());
            }
            if (!fs.write_file(fc->path, contents, mode)) {
                return ApplyError { ApplyErrorKind::Io, std::string { fc->path } };
            }
        } else if (const auto* fp = std::get_if<FilePatch>(&part)) {
            if (!is_safe_patch_path_(fp->path)) {
                return ApplyError { ApplyErrorKind::UnsafePath, std::string { fp->path } };
            }
            if (auto err = apply_patch_(*fp, fs)) {
                return err;
            }
        } else if (const auto* fm = std::get_if<FileModeChange>(&part)) {
            if (!is_safe_patch_path_(fm->path)) {
                return ApplyError { ApplyErrorKind::UnsafePath, std::string { fm->path } };
            }
            if (!fs.chmod(fm->path, file_mode_to_bun_mode(fm->new_mode))) {
                return ApplyError { ApplyErrorKind::Io, std::string { fm->path } };
            }
        }
    }
    return std::nullopt;
}

}  // namespace mbun::patch
