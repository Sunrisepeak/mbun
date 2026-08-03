// Host-independent PatchFile JSON serialization for Bun's patch testing API.
// ref: bun-ref/src/patch/lib.rs json_fmt;
//      bun-zig-src/src/patch/patch.zig std.json.fmt(PatchFile).
export module mbun.patch_jsc.json;

import std;
import mbun.patch;

namespace mbun::patch_jsc {
namespace {

constexpr std::array<char, 16> HEX_DIGITS {
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
};

void append_json_string(std::string& output, std::string_view value) {
    output.push_back('"');
    std::size_t runStart { 0 };
    for (std::size_t index { 0 }; index < value.size(); ++index) {
        const auto byte { static_cast<unsigned char>(value[index]) };
        std::string_view escape;
        switch (byte) {
        case '"':
            escape = "\\\"";
            break;
        case '\\':
            escape = "\\\\";
            break;
        case '\b':
            escape = "\\b";
            break;
        case '\f':
            escape = "\\f";
            break;
        case '\n':
            escape = "\\n";
            break;
        case '\r':
            escape = "\\r";
            break;
        case '\t':
            escape = "\\t";
            break;
        default:
            if (byte < 0x20) {
                output.append(value.substr(runStart, index - runStart));
                output.append("\\u00");
                output.push_back(HEX_DIGITS[byte >> 4]);
                output.push_back(HEX_DIGITS[byte & 0x0f]);
                runStart = index + 1;
            }
            continue;
        }
        output.append(value.substr(runStart, index - runStart));
        output.append(escape);
        runStart = index + 1;
    }
    output.append(value.substr(runStart));
    output.push_back('"');
}

void append_optional_string(std::string& output,
                            const std::optional<std::string_view>& value) {
    if (value) {
        append_json_string(output, *value);
    } else {
        output.append("null");
    }
}

template <typename T, typename AppendElement>
void append_list(std::string& output, const std::vector<T>& values,
                 AppendElement&& appendElement) {
    output.append("{\"items\":[");
    for (std::size_t index { 0 }; index < values.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        appendElement(output, values[index]);
    }
    output.append("],\"capacity\":");
    output.append(std::to_string(values.capacity()));
    output.push_back('}');
}

constexpr std::string_view file_mode_tag(mbun::patch::FileMode mode) {
    switch (mode) {
    case mbun::patch::FileMode::NonExecutable:
        return "non_executable";
    case mbun::patch::FileMode::Executable:
        return "executable";
    }
    std::unreachable();
}

constexpr std::string_view part_type_tag(mbun::patch::PartType type) {
    switch (type) {
    case mbun::patch::PartType::Context:
        return "context";
    case mbun::patch::PartType::Insertion:
        return "insertion";
    case mbun::patch::PartType::Deletion:
        return "deletion";
    }
    std::unreachable();
}

void append_header(std::string& output, const mbun::patch::Header& header) {
    output.append("{\"original\":{\"start\":");
    output.append(std::to_string(header.original.start));
    output.append(",\"len\":");
    output.append(std::to_string(header.original.len));
    output.append("},\"patched\":{\"start\":");
    output.append(std::to_string(header.patched.start));
    output.append(",\"len\":");
    output.append(std::to_string(header.patched.len));
    output.append("}}");
}

void append_mutation_part(std::string& output,
                          const mbun::patch::PatchMutationPart& part) {
    output.append("{\"type\":\"");
    output.append(part_type_tag(part.ty));
    output.append("\",\"lines\":");
    append_list(output, part.lines,
                [](std::string& target, std::string_view line) {
                    append_json_string(target, line);
                });
    output.append(",\"no_newline_at_end_of_file\":");
    output.append(part.no_newline_at_end_of_file ? "true" : "false");
    output.push_back('}');
}

void append_hunk(std::string& output, const mbun::patch::Hunk& hunk) {
    output.append("{\"header\":");
    append_header(output, hunk.header);
    output.append(",\"parts\":");
    append_list(output, hunk.parts, append_mutation_part);
    output.push_back('}');
}

void append_optional_hunk(std::string& output,
                          const std::optional<mbun::patch::Hunk>& hunk) {
    if (hunk) {
        append_hunk(output, *hunk);
    } else {
        output.append("null");
    }
}

void append_file_patch(std::string& output, const mbun::patch::FilePatch& patch) {
    output.append("{\"path\":");
    append_json_string(output, patch.path);
    output.append(",\"hunks\":");
    append_list(output, patch.hunks, append_hunk);
    output.append(",\"before_hash\":");
    append_optional_string(output, patch.before_hash);
    output.append(",\"after_hash\":");
    append_optional_string(output, patch.after_hash);
    output.push_back('}');
}

void append_file_deletion(std::string& output,
                          const mbun::patch::FileDeletion& deletion) {
    output.append("{\"path\":");
    append_json_string(output, deletion.path);
    output.append(",\"mode\":\"");
    output.append(file_mode_tag(deletion.mode));
    output.append("\",\"hunk\":");
    append_optional_hunk(output, deletion.hunk);
    output.append(",\"hash\":");
    append_optional_string(output, deletion.hash);
    output.push_back('}');
}

void append_file_creation(std::string& output,
                          const mbun::patch::FileCreation& creation) {
    output.append("{\"path\":");
    append_json_string(output, creation.path);
    output.append(",\"mode\":\"");
    output.append(file_mode_tag(creation.mode));
    output.append("\",\"hunk\":");
    append_optional_hunk(output, creation.hunk);
    output.append(",\"hash\":");
    append_optional_string(output, creation.hash);
    output.push_back('}');
}

void append_file_rename(std::string& output, const mbun::patch::FileRename& rename) {
    output.append("{\"from_path\":");
    append_json_string(output, rename.from_path);
    output.append(",\"to_path\":");
    append_json_string(output, rename.to_path);
    output.push_back('}');
}

void append_file_mode_change(std::string& output,
                             const mbun::patch::FileModeChange& change) {
    output.append("{\"path\":");
    append_json_string(output, change.path);
    output.append(",\"old_mode\":\"");
    output.append(file_mode_tag(change.old_mode));
    output.append("\",\"new_mode\":\"");
    output.append(file_mode_tag(change.new_mode));
    output.append("\"}");
}

void append_patch_part(std::string& output, const mbun::patch::PatchFilePart& part) {
    std::visit(
        [&output](const auto& value) {
            using Part = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::same_as<Part, mbun::patch::FilePatch>) {
                output.append("{\"file_patch\":");
                append_file_patch(output, value);
            } else if constexpr (std::same_as<Part, mbun::patch::FileDeletion>) {
                output.append("{\"file_deletion\":");
                append_file_deletion(output, value);
            } else if constexpr (std::same_as<Part, mbun::patch::FileCreation>) {
                output.append("{\"file_creation\":");
                append_file_creation(output, value);
            } else if constexpr (std::same_as<Part, mbun::patch::FileRename>) {
                output.append("{\"file_rename\":");
                append_file_rename(output, value);
            } else {
                output.append("{\"file_mode_change\":");
                append_file_mode_change(output, value);
            }
            output.push_back('}');
        },
        part);
}

} // namespace

export std::string serialize_patch_file(const mbun::patch::PatchFile& patchFile) {
    std::string output;
    output.reserve(64 + patchFile.parts.size() * 128);
    output.append("{\"parts\":");
    append_list(output, patchFile.parts, append_patch_part);
    output.push_back('}');
    return output;
}

} // namespace mbun::patch_jsc
