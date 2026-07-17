// types.cppm — mbun.patch.types: the structured representation of a parsed
// patch file. Ported 1:1 from bun's src/patch/lib.rs data model.
//
// Borrowing note (from the Rust): every string_view in this tree points into
// the ORIGINAL patch file text passed to parse_patch_file. The caller must keep
// that buffer alive for as long as the PatchFile is used.
export module mbun.patch.types;

import std;

namespace mbun::patch {

// ──────────────────────────────────────────────────────────────────────────
// PatchMutationPart / Hunk
// ──────────────────────────────────────────────────────────────────────────

// Keep context/insertion/deletion values in sync with HunkLineType.
export enum class PartType : std::uint8_t {
    Context = 0,
    Insertion,
    Deletion,
};

export struct PatchMutationPart {
    PartType ty { PartType::Context };
    std::vector<std::string_view> lines;
    // This technically can only be on the last part of a hunk.
    bool no_newline_at_end_of_file { false };
};

export struct HeaderRange {
    std::uint32_t start { 1 };
    std::uint32_t len { 0 };
};

export struct Header {
    HeaderRange original {};
    HeaderRange patched {};

    // The Rust Header::EMPTY — a default-constructed Header already matches
    // (start=1, len=0), kept as a named constant for parity with the port.
    static Header empty() { return Header {}; }
};

export struct Hunk {
    Header header {};
    std::vector<PatchMutationPart> parts;

    // Original/patched line counts implied by the parts must match the header.
    bool verify_integrity() const {
        std::size_t original_length { 0 };
        std::size_t patched_length { 0 };
        for (const auto& part : parts) {
            switch (part.ty) {
            case PartType::Context:
                patched_length += part.lines.size();
                original_length += part.lines.size();
                break;
            case PartType::Insertion:
                patched_length += part.lines.size();
                break;
            case PartType::Deletion:
                original_length += part.lines.size();
                break;
            }
        }
        if (original_length != static_cast<std::size_t>(header.original.len)
            || patched_length != static_cast<std::size_t>(header.patched.len)) {
            return false;
        }
        return true;
    }
};

// ──────────────────────────────────────────────────────────────────────────
// FileMode
// ──────────────────────────────────────────────────────────────────────────

export enum class FileMode : std::uint32_t {
    NonExecutable = 0644,
    Executable = 0755,
};

export inline std::optional<FileMode> file_mode_from_u32(std::uint32_t mode) {
    switch (mode) {
    case 0644:
        return FileMode::NonExecutable;
    case 0755:
        return FileMode::Executable;
    default:
        return std::nullopt;
    }
}

export inline std::uint32_t file_mode_to_bun_mode(FileMode m) {
    return static_cast<std::uint32_t>(m);
}

// ──────────────────────────────────────────────────────────────────────────
// FileRename / FileModeChange / FilePatch / FileDeletion / FileCreation
// ──────────────────────────────────────────────────────────────────────────

export struct FileRename {
    std::string_view from_path;
    std::string_view to_path;
};

export struct FileModeChange {
    std::string_view path;
    FileMode old_mode { FileMode::NonExecutable };
    FileMode new_mode { FileMode::NonExecutable };
};

export struct FilePatch {
    std::string_view path;
    std::vector<Hunk> hunks;
    std::optional<std::string_view> before_hash;
    std::optional<std::string_view> after_hash;
};

export struct FileDeletion {
    std::string_view path;
    FileMode mode { FileMode::NonExecutable };
    // Box<Hunk> in Rust; a value optional here (patch tree is not recursive).
    std::optional<Hunk> hunk;
    std::optional<std::string_view> hash;
};

export struct FileCreation {
    std::string_view path;
    FileMode mode { FileMode::NonExecutable };
    std::optional<Hunk> hunk;
    std::optional<std::string_view> hash;
};

// Rust's PatchFilePart tagged union. Variant order matters only for indexing;
// apply/json dispatch on the active alternative.
export using PatchFilePart = std::variant<
    FilePatch,
    FileDeletion,
    FileCreation,
    FileRename,
    FileModeChange>;

export struct PatchFile {
    std::vector<PatchFilePart> parts;
};

// ──────────────────────────────────────────────────────────────────────────
// ParseErr
// ──────────────────────────────────────────────────────────────────────────

export enum class ParseErr {
    unrecognized_pragma,
    no_newline_at_eof_pragma_encountered_without_context,
    hunk_lines_encountered_before_hunk_header,
    hunk_header_integrity_check_failed,
    bad_diff_line,
    bad_header_line,
    rename_from_and_to_not_give,
    no_path_given_for_file_deletion,
    no_path_given_for_file_creation,
    bad_file_mode,
};

export inline std::string_view parse_err_name(ParseErr e) {
    switch (e) {
    case ParseErr::unrecognized_pragma:
        return "unrecognized_pragma";
    case ParseErr::no_newline_at_eof_pragma_encountered_without_context:
        return "no_newline_at_eof_pragma_encountered_without_context";
    case ParseErr::hunk_lines_encountered_before_hunk_header:
        return "hunk_lines_encountered_before_hunk_header";
    case ParseErr::hunk_header_integrity_check_failed:
        return "hunk_header_integrity_check_failed";
    case ParseErr::bad_diff_line:
        return "bad_diff_line";
    case ParseErr::bad_header_line:
        return "bad_header_line";
    case ParseErr::rename_from_and_to_not_give:
        return "rename_from_and_to_not_give";
    case ParseErr::no_path_given_for_file_deletion:
        return "no_path_given_for_file_deletion";
    case ParseErr::no_path_given_for_file_creation:
        return "no_path_given_for_file_creation";
    case ParseErr::bad_file_mode:
        return "bad_file_mode";
    }
    return "unknown";
}

}  // namespace mbun::patch
