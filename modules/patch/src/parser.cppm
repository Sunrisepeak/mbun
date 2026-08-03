// parser.cppm — mbun.patch.parser: unified-diff patch file parser.
//
// Ported from bun's src/patch/lib.rs (PatchLinesParser, parse_patch_file,
// patch_file_second_pass and the header/diff-line sub-parsers). The parser is
// fully pure: all output string_views borrow the input `file` buffer, so the
// caller MUST keep it alive for the lifetime of the returned PatchFile.
export module mbun.patch.parser;

import std;
import mbun.patch.types;
import mbun.patch.util;

namespace mbun::patch {

namespace {

using util::starts_with;
using util::trim;

// ──────────────────────────────────────────────────────────────────────────
// FileDeets — per-file accumulator during the first parse pass.
// ──────────────────────────────────────────────────────────────────────────

struct FileDeets {
    std::optional<std::string_view> diff_line_from_path;
    std::optional<std::string_view> diff_line_to_path;
    std::optional<std::string_view> old_mode;
    std::optional<std::string_view> new_mode;
    std::optional<std::string_view> deleted_file_mode;
    std::optional<std::string_view> new_file_mode;
    std::optional<std::string_view> rename_from;
    std::optional<std::string_view> rename_to;
    std::optional<std::string_view> before_hash;
    std::optional<std::string_view> after_hash;
    std::optional<std::string_view> from_path;
    std::optional<std::string_view> to_path;
    std::vector<Hunk> hunks;

    std::vector<Hunk> take_hunks() { return std::move(hunks); }

    void nullify_empty_strings() {
        auto nullify = [](std::optional<std::string_view>& f) {
            if (f && f->empty()) {
                f = std::nullopt;
            }
        };
        nullify(diff_line_from_path);
        nullify(diff_line_to_path);
        nullify(old_mode);
        nullify(new_mode);
        nullify(deleted_file_mode);
        nullify(new_file_mode);
        nullify(rename_from);
        nullify(rename_to);
        nullify(before_hash);
        nullify(after_hash);
        nullify(from_path);
        nullify(to_path);
    }
};

// ──────────────────────────────────────────────────────────────────────────
// ScalarSplitIter / LookbackIterator — split-on-'\n' with a rewind hook.
// ──────────────────────────────────────────────────────────────────────────

struct ScalarSplitIter {
    std::string_view buffer;
    std::optional<std::size_t> index { std::size_t { 0 } };
    char delimiter;

    ScalarSplitIter(std::string_view buf, char delim) : buffer { buf }, delimiter { delim } {}

    std::optional<std::string_view> next() {
        if (!index) {
            return std::nullopt;
        }
        std::size_t start = *index;
        std::size_t end;
        auto pos = util::index_of_char(buffer.substr(start), delimiter);
        if (pos) {
            index = start + *pos + 1;
            end = start + *pos;
        } else {
            index = std::nullopt;
            end = buffer.size();
        }
        return buffer.substr(start, end - start);
    }
};

struct LookbackIterator {
    ScalarSplitIter inner;
    std::size_t prev_index { 0 };

    explicit LookbackIterator(ScalarSplitIter it) : inner { it } {}

    std::optional<std::string_view> next() {
        prev_index = inner.index.value_or(prev_index);
        return inner.next();
    }

    void back() { inner.index = prev_index; }
};

// ──────────────────────────────────────────────────────────────────────────
// hunk header / diff line sub-parsers
// ──────────────────────────────────────────────────────────────────────────

constexpr bool is_digit_(unsigned char c) { return c >= '0' && c <= '9'; }
constexpr bool is_word_(unsigned char c) {
    return is_digit_(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

struct HunkHeaderLineImpl {
    std::uint32_t line_nr;
    std::uint32_t line_count;
    std::string_view rest;
};

std::expected<HunkHeaderLineImpl, ParseErr> parse_hunk_header_line_impl(std::string_view text) {
    // @@ -100,32 +100,32 @@
    //     ^  (text starts just past "@@ -")
    std::size_t line_nr_end { 0 };
    bool saw_comma { false };
    bool saw_whitespace { false };
    while (line_nr_end < text.size()) {
        if (text[line_nr_end] == ',') {
            saw_comma = true;
            break;
        } else if (text[line_nr_end] == ' ') {
            saw_whitespace = true;
            break;
        }
        if (!is_digit_(static_cast<unsigned char>(text[line_nr_end]))) {
            return std::unexpected(ParseErr::bad_header_line);
        }
        ++line_nr_end;
    }
    if (!saw_comma && !saw_whitespace) {
        return std::unexpected(ParseErr::bad_header_line);
    }
    std::string_view line_nr = text.substr(0, line_nr_end);
    std::string_view line_nr_count = "1";
    if (line_nr_end + 1 >= text.size()) {
        return std::unexpected(ParseErr::bad_header_line);
    }

    text = text.substr(line_nr_end);
    if (text.empty()) {
        return std::unexpected(ParseErr::bad_header_line);
    }

    if (saw_comma) {
        text = text.substr(1);
        saw_whitespace = false;
        std::size_t first_col_end { 0 };
        while (first_col_end < text.size()) {
            if (text[first_col_end] == ' ') {
                saw_whitespace = true;
                break;
            }
            if (!is_digit_(static_cast<unsigned char>(text[first_col_end]))) {
                return std::unexpected(ParseErr::bad_header_line);
            }
            ++first_col_end;
        }
        if (!saw_whitespace) {
            return std::unexpected(ParseErr::bad_header_line);
        }
        line_nr_count = text.substr(0, first_col_end);
        text = text.substr(first_col_end);
    }

    auto nr = util::parse_uint(line_nr, 10);
    if (!nr) {
        return std::unexpected(ParseErr::bad_header_line);
    }
    auto count = util::parse_uint(line_nr_count, 10);
    if (!count) {
        return std::unexpected(ParseErr::bad_header_line);
    }
    return HunkHeaderLineImpl {
        .line_nr = std::max<std::uint32_t>(1, *nr),
        .line_count = *count,
        .rest = text,
    };
}

std::expected<Hunk, ParseErr> parse_hunk_header_line(std::string_view line_) {
    std::string_view line = trim(line_, util::WHITESPACE);
    // @@ -100,32 +100,32 @@
    // ^^^^  this part
    if (!(line.size() >= 4 && line[0] == '@' && line[1] == '@' && line[2] == ' '
          && line[3] == '-')) {
        return std::unexpected(ParseErr::bad_header_line);
    }
    if (line.size() <= 4) {
        return std::unexpected(ParseErr::bad_header_line);
    }
    line = line.substr(4);

    auto first = parse_hunk_header_line_impl(line);
    if (!first) {
        return std::unexpected(first.error());
    }
    line = first->rest;
    if (line.size() < 2 || line[1] != '+') {
        return std::unexpected(ParseErr::bad_header_line);
    }
    line = line.substr(2);

    auto second = parse_hunk_header_line_impl(line);
    if (!second) {
        return std::unexpected(second.error());
    }
    line = second->rest;

    if (line.size() >= 3 && line[0] == ' ' && line[1] == '@' && line[2] == '@') {
        Hunk hunk {};
        hunk.header.original = HeaderRange { first->line_nr, first->line_count };
        hunk.header.patched = HeaderRange { second->line_nr, second->line_count };
        return hunk;
    }

    return std::unexpected(ParseErr::bad_header_line);
}

std::optional<std::pair<std::string_view, std::string_view>>
parse_diff_hashes(std::string_view line) {
    // index 2de83dd..842652c 100644  (caller already stripped "index ")
    // From @pnpm/patch-package: const match = line.match(/(\w+)\.\.(\w+)/)
    auto delim = util::index_of(line, "..");
    if (!delim) {
        return std::nullopt;
    }
    std::size_t delimiter_start = *delim;

    std::string_view a_part = line.substr(0, delimiter_start);
    for (unsigned char c : a_part) {
        if (!is_word_(c)) {
            return std::nullopt;
        }
    }

    std::size_t b_part_start = delimiter_start + 2;
    if (b_part_start >= line.size()) {
        return std::nullopt;
    }
    std::string_view rest = line.substr(b_part_start);
    auto stop = util::index_of_any(rest, " \n\r\t");
    std::size_t b_part_end = stop ? *stop + b_part_start : line.size();

    std::string_view b_part = line.substr(b_part_start, b_part_end - b_part_start);
    for (unsigned char c : b_part) {
        if (!is_word_(c)) {
            return std::nullopt;
        }
    }

    return std::pair { a_part, b_part };
}

std::optional<std::pair<std::string_view, std::string_view>>
parse_diff_line_paths(std::string_view line) {
    // const match = line.match(/^diff --git a\/(.*?) b\/(.*?)\s*$/)
    constexpr std::string_view PREFIX = "diff --git a/";
    if (!starts_with(line, PREFIX)) {
        return std::nullopt;
    }
    std::string_view rest = line.substr(PREFIX.size());
    if (rest.empty()) {
        return std::nullopt;
    }

    std::size_t a_path_end_index;
    std::size_t b_path_start_index;

    std::size_t i { 0 };
    while (true) {
        auto found = util::index_of_char(rest.substr(i), 'b');
        if (!found) {
            return std::nullopt;
        }
        i += *found;
        if (i > 0 && rest[i - 1] == ' ' && i + 1 < rest.size() && rest[i + 1] == '/') {
            a_path_end_index = i - 1;
            b_path_start_index = i + 2;
            break;
        }
        i += 1;
    }

    std::string_view a_path = rest.substr(0, a_path_end_index);
    std::string_view b_path = util::trim_right(rest.substr(b_path_start_index), " \n\r\t");
    return std::pair { a_path, b_path };
}

std::optional<FileMode> parse_file_mode(std::string_view mode) {
    auto parsed = util::parse_uint(mode, 8);
    if (!parsed) {
        return std::nullopt;
    }
    return file_mode_from_u32(*parsed & 0777);
}

// ──────────────────────────────────────────────────────────────────────────
// PatchLinesParser
// ──────────────────────────────────────────────────────────────────────────

enum class ParserState { ParsingHeader, ParsingHunks };

enum class HunkLineType {
    Context = 0,
    Insertion,
    Deletion,
    Header,
    Pragma,
};

struct ParseOpts {
    bool support_legacy_diffs { false };
};

struct PatchLinesParser {
    std::vector<FileDeets> result;
    FileDeets current_file_patch;
    ParserState state { ParserState::ParsingHeader };
    std::optional<Hunk> current_hunk;
    std::optional<PatchMutationPart> current_hunk_mutation_part;

    void reset() {
        std::vector<FileDeets> keep = std::move(result);
        keep.clear();
        *this = PatchLinesParser {};
        result = std::move(keep);
    }

    void commit_hunk() {
        if (current_hunk) {
            if (current_hunk_mutation_part) {
                current_hunk->parts.push_back(std::move(*current_hunk_mutation_part));
                current_hunk_mutation_part = std::nullopt;
            }
            current_file_patch.hunks.push_back(std::move(*current_hunk));
            current_hunk = std::nullopt;
        }
    }

    void commit_file_patch() {
        commit_hunk();
        current_file_patch.nullify_empty_strings();
        result.push_back(std::move(current_file_patch));
        current_file_patch = FileDeets {};
    }

    std::expected<void, ParseErr> parse(std::string_view file_, ParseOpts opts) {
        if (file_.empty()) {
            return {};
        }
        // Peek at the last segment after the final '\n'; strip a single trailing
        // newline so we don't emit a spurious trailing empty line.
        std::size_t end = file_.size();
        {
            auto last_nl_pos = file_.rfind('\n');
            std::string_view last_line = (last_nl_pos == std::string_view::npos)
                                             ? file_
                                             : file_.substr(last_nl_pos + 1);
            if (last_line.empty() && last_nl_pos != std::string_view::npos) {
                end = last_nl_pos;
            }
        }
        if (end == 0 || end > file_.size()) {
            return {};
        }
        std::string_view file = file_.substr(0, end);
        LookbackIterator lines { ScalarSplitIter { file, '\n' } };

        while (auto maybe_line = lines.next()) {
            std::string_view line = *maybe_line;
            switch (state) {
            case ParserState::ParsingHeader: {
                if (starts_with(line, "@@")) {
                    state = ParserState::ParsingHunks;
                    current_file_patch.hunks.clear();
                    lines.back();
                } else if (starts_with(line, "diff --git ")) {
                    if (current_file_patch.diff_line_from_path) {
                        commit_file_patch();
                    }
                    auto m = parse_diff_line_paths(line);
                    if (!m) {
                        return std::unexpected(ParseErr::bad_diff_line);
                    }
                    current_file_patch.diff_line_from_path = m->first;
                    current_file_patch.diff_line_to_path = m->second;
                } else if (starts_with(line, "old mode ")) {
                    current_file_patch.old_mode
                        = trim(line.substr(std::string_view("old mode ").size()), util::WHITESPACE);
                } else if (starts_with(line, "new mode ")) {
                    current_file_patch.new_mode
                        = trim(line.substr(std::string_view("new mode ").size()), util::WHITESPACE);
                } else if (starts_with(line, "deleted file mode ")) {
                    current_file_patch.deleted_file_mode = trim(
                        line.substr(std::string_view("deleted file mode ").size()),
                        util::WHITESPACE);
                } else if (starts_with(line, "new file mode ")) {
                    current_file_patch.new_file_mode = trim(
                        line.substr(std::string_view("new file mode ").size()), util::WHITESPACE);
                } else if (starts_with(line, "rename from ")) {
                    current_file_patch.rename_from = trim(
                        line.substr(std::string_view("rename from ").size()), util::WHITESPACE);
                } else if (starts_with(line, "rename to ")) {
                    current_file_patch.rename_to
                        = trim(line.substr(std::string_view("rename to ").size()), util::WHITESPACE);
                } else if (starts_with(line, "index ")) {
                    auto hashes = parse_diff_hashes(line.substr(std::string_view("index ").size()));
                    if (!hashes) {
                        continue;
                    }
                    current_file_patch.before_hash = hashes->first;
                    current_file_patch.after_hash = hashes->second;
                } else if (starts_with(line, "--- ")) {
                    // May be shorter than "--- a/" (e.g. a bare "--- "); treat
                    // the missing path as empty like the JS `slice("--- a/".length)`.
                    constexpr std::size_t n = std::string_view("--- a/").size();
                    std::string_view rest = line.size() >= n ? line.substr(n) : std::string_view {};
                    current_file_patch.from_path = trim(rest, util::WHITESPACE);
                } else if (starts_with(line, "+++ ")) {
                    constexpr std::size_t n = std::string_view("+++ b/").size();
                    std::string_view rest = line.size() >= n ? line.substr(n) : std::string_view {};
                    current_file_patch.to_path = trim(rest, util::WHITESPACE);
                }
                break;
            }
            case ParserState::ParsingHunks: {
                if (opts.support_legacy_diffs && starts_with(line, "--- a/")) {
                    state = ParserState::ParsingHeader;
                    commit_file_patch();
                    lines.back();
                    continue;
                }
                HunkLineType hunk_line_type;
                if (line.empty()) {
                    hunk_line_type = HunkLineType::Context;
                } else {
                    std::optional<HunkLineType> maybe;
                    switch (line[0]) {
                    case '@':
                        maybe = HunkLineType::Header;
                        break;
                    case '-':
                        maybe = HunkLineType::Deletion;
                        break;
                    case '+':
                        maybe = HunkLineType::Insertion;
                        break;
                    case ' ':
                        maybe = HunkLineType::Context;
                        break;
                    case '\\':
                        maybe = HunkLineType::Pragma;
                        break;
                    case '\r':
                        maybe = HunkLineType::Context;
                        break;
                    default:
                        maybe = std::nullopt;
                        break;
                    }
                    if (!maybe) {
                        // unrecognized, bail out
                        state = ParserState::ParsingHeader;
                        commit_file_patch();
                        lines.back();
                        continue;
                    }
                    hunk_line_type = *maybe;
                }

                switch (hunk_line_type) {
                case HunkLineType::Header: {
                    commit_hunk();
                    auto h = parse_hunk_header_line(line);
                    if (!h) {
                        return std::unexpected(h.error());
                    }
                    current_hunk = std::move(*h);
                    break;
                }
                case HunkLineType::Pragma: {
                    if (!starts_with(line, "\\ No newline at end of file")) {
                        return std::unexpected(ParseErr::unrecognized_pragma);
                    }
                    if (!current_hunk_mutation_part) {
                        return std::unexpected(
                            ParseErr::no_newline_at_eof_pragma_encountered_without_context);
                    }
                    current_hunk_mutation_part->no_newline_at_end_of_file = true;
                    break;
                }
                case HunkLineType::Insertion:
                case HunkLineType::Deletion:
                case HunkLineType::Context: {
                    if (!current_hunk) {
                        return std::unexpected(ParseErr::hunk_lines_encountered_before_hunk_header);
                    }
                    PartType want = hunk_line_type == HunkLineType::Context ? PartType::Context
                        : hunk_line_type == HunkLineType::Insertion       ? PartType::Insertion
                                                                          : PartType::Deletion;
                    if (current_hunk_mutation_part && current_hunk_mutation_part->ty != want) {
                        current_hunk->parts.push_back(std::move(*current_hunk_mutation_part));
                        current_hunk_mutation_part = std::nullopt;
                    }
                    if (!current_hunk_mutation_part) {
                        PatchMutationPart p {};
                        p.ty = want;
                        current_hunk_mutation_part = std::move(p);
                    }
                    // strip the leading marker byte (line[1..], clamped)
                    current_hunk_mutation_part->lines.push_back(
                        line.substr(std::min<std::size_t>(1, line.size())));
                    break;
                }
                }
                break;
            }
            }
        }

        commit_file_patch();

        for (const auto& file_deet : result) {
            for (const auto& hunk : file_deet.hunks) {
                if (!hunk.verify_integrity()) {
                    return std::unexpected(ParseErr::hunk_header_integrity_check_failed);
                }
            }
        }

        return {};
    }
};

// ──────────────────────────────────────────────────────────────────────────
// second pass — turn FileDeets into PatchFilePart entries
// ──────────────────────────────────────────────────────────────────────────

enum class PatchFilePartKind {
    FilePatch,
    FileDeletion,
    FileCreation,
    FileRename,
    FileModeChange,
};

std::expected<PatchFile, ParseErr> patch_file_second_pass(std::vector<FileDeets>& files) {
    PatchFile result {};

    for (auto& file : files) {
        PatchFilePartKind ty;
        if (file.rename_from && !file.rename_from->empty()) {
            ty = PatchFilePartKind::FileRename;
        } else if (file.deleted_file_mode && !file.deleted_file_mode->empty()) {
            ty = PatchFilePartKind::FileDeletion;
        } else if (file.new_file_mode && !file.new_file_mode->empty()) {
            ty = PatchFilePartKind::FileCreation;
        } else if (!file.hunks.empty()) {
            ty = PatchFilePartKind::FilePatch;
        } else {
            ty = PatchFilePartKind::FileModeChange;
        }

        std::optional<std::string_view> destination_file_path;

        switch (ty) {
        case PatchFilePartKind::FileRename: {
            if (!file.rename_from || !file.rename_to) {
                return std::unexpected(ParseErr::rename_from_and_to_not_give);
            }
            result.parts.emplace_back(FileRename {
                .from_path = *file.rename_from,
                .to_path = *file.rename_to,
            });
            destination_file_path = file.rename_to;
            break;
        }
        case PatchFilePartKind::FileDeletion: {
            auto path = file.diff_line_from_path ? file.diff_line_from_path : file.from_path;
            if (!path) {
                return std::unexpected(ParseErr::no_path_given_for_file_deletion);
            }
            std::optional<Hunk> hunk;
            if (!file.hunks.empty()) {
                hunk = std::move(file.hunks[0]);
                file.hunks[0] = Hunk { Header::empty(), {} };
            }
            auto mode = parse_file_mode(*file.deleted_file_mode);
            if (!mode) {
                return std::unexpected(ParseErr::bad_file_mode);
            }
            result.parts.emplace_back(FileDeletion {
                .path = *path,
                .mode = *mode,
                .hunk = std::move(hunk),
                .hash = file.before_hash,
            });
            break;
        }
        case PatchFilePartKind::FileCreation: {
            auto path = file.diff_line_to_path ? file.diff_line_to_path : file.to_path;
            if (!path) {
                return std::unexpected(ParseErr::no_path_given_for_file_creation);
            }
            std::optional<Hunk> hunk;
            if (!file.hunks.empty()) {
                hunk = std::move(file.hunks[0]);
                file.hunks[0] = Hunk { Header::empty(), {} };
            }
            auto mode = parse_file_mode(*file.new_file_mode);
            if (!mode) {
                return std::unexpected(ParseErr::bad_file_mode);
            }
            result.parts.emplace_back(FileCreation {
                .path = *path,
                .mode = *mode,
                .hunk = std::move(hunk),
                .hash = file.after_hash,
            });
            break;
        }
        case PatchFilePartKind::FilePatch:
        case PatchFilePartKind::FileModeChange: {
            destination_file_path = file.to_path ? file.to_path : file.diff_line_to_path;
            break;
        }
        }

        if (destination_file_path && file.old_mode && file.new_mode
            && *file.old_mode != *file.new_mode) {
            auto old_m = parse_file_mode(*file.old_mode);
            auto new_m = parse_file_mode(*file.new_mode);
            if (!old_m || !new_m) {
                return std::unexpected(ParseErr::bad_file_mode);
            }
            result.parts.emplace_back(FileModeChange {
                .path = *destination_file_path,
                .old_mode = *old_m,
                .new_mode = *new_m,
            });
        }

        if (destination_file_path && !file.hunks.empty()) {
            result.parts.emplace_back(FilePatch {
                .path = *destination_file_path,
                .hunks = file.take_hunks(),
                .before_hash = file.before_hash,
                .after_hash = file.after_hash,
            });
        }
    }

    return result;
}

}  // namespace

// ──────────────────────────────────────────────────────────────────────────
// public entry point
// ──────────────────────────────────────────────────────────────────────────

// Parse a whole patch file. The returned PatchFile borrows from `file`, so keep
// `file` alive as long as the result is used.
export std::expected<PatchFile, ParseErr> parse_patch_file(std::string_view file) {
    PatchLinesParser parser {};

    auto r = parser.parse(file, ParseOpts {});
    if (!r) {
        // Legacy-diff retry hack (mirrors the Rust): a failed integrity check
        // may just mean the patch uses legacy `--- a/` separators.
        if (r.error() == ParseErr::hunk_header_integrity_check_failed) {
            parser.reset();
            auto r2 = parser.parse(file, ParseOpts { .support_legacy_diffs = true });
            if (!r2) {
                return std::unexpected(r2.error());
            }
        } else {
            return std::unexpected(r.error());
        }
    }

    std::vector<FileDeets> files = std::move(parser.result);
    return patch_file_second_pass(files);
}

}  // namespace mbun::patch
