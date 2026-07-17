// tag_template.cppm — Bun shell tagged-template source construction.
//
// Ref: bun src/runtime/shell/shell_body.rs shell_cmd_from_js / ShellSrcBuilder
// and the Zig predecessor src/shell/shell.zig shellCmdFromJS. Bun carries JS
// values as parser markers; mbun's process-backed executor materializes the
// same safety boundary as shell text while preserving the surrounding quote
// context.
export module mbun.shell.tag_template;

import std;
import mbun.shell.parser;
import mbun.shell.json;

export namespace mbun::shell {

inline constexpr std::size_t MAX_TEMPLATE_ARRAY_DEPTH{100};

enum class TemplateErrorCode : std::uint8_t {
    ArityMismatch,
    NullByte,
    Parse,
    UnsupportedPlatform,
};

enum class TemplatePlatform : std::uint8_t { Posix, Windows };

struct TemplateError {
    TemplateErrorCode code{TemplateErrorCode::ArityMismatch};
    std::string message;
};

struct TemplateArgument {
    std::vector<std::string> words;

    // Set when the interpolated value is array-buffer-backed (Uint8Array, ArrayBuffer,
    // ...). bun does not stringify those: it pushes the value into out_jsobjs and writes
    // a \x08__bun_N reference into the script, so the parser sees a JSObjRef token rather
    // than text (shell_body.rs:766-777). `words` is unused when this is set.
    //
    // Only a *whole* interpolation can be an object ref today. bun also handles refs
    // nested inside an interpolated array (handle_template_value recurses before the
    // as_array_buffer check, shell_body.rs:851-881); mbun's per-argument word list cannot
    // express that interleaving, so nested buffers still stringify.
    std::optional<std::uint32_t> jsObjIndex;
};

namespace detail {

void append_unquoted(std::string& output, std::string_view word) {
    output.push_back('\'');
    for (const char byte : word) {
        if (byte == '\'') {
            output += "'\\''";
        } else {
            output.push_back(byte);
        }
    }
    output.push_back('\'');
}

void append_single_quoted(std::string& output, std::string_view word) {
    for (const char byte : word) {
        if (byte == '\'') {
            output += "'\\''";
        } else {
            output.push_back(byte);
        }
    }
}

void append_double_quoted(std::string& output, std::string_view word) {
    for (const char byte : word) {
        if (byte == '\\' || byte == '"' || byte == '$' || byte == '`') {
            output.push_back('\\');
        }
        output.push_back(byte);
    }
}

void append_word(std::string& output, std::string_view word, TemplateQuoteContext context) {
    switch (context) {
        case TemplateQuoteContext::Unquoted: append_unquoted(output, word); break;
        case TemplateQuoteContext::SingleQuoted: append_single_quoted(output, word); break;
        case TemplateQuoteContext::DoubleQuoted: append_double_quoted(output, word); break;
    }
}

}  // namespace detail

std::expected<std::string, TemplateError>
compile_template(std::span<const std::string> rawSegments,
                 std::span<const TemplateArgument> arguments,
                 TemplatePlatform platform = TemplatePlatform::Posix) {
    if (platform == TemplatePlatform::Windows) {
        return std::unexpected(TemplateError{
            TemplateErrorCode::UnsupportedPlatform,
            "Bun.$ shell execution is unsupported on Windows in this build",
        });
    }
    if (rawSegments.size() != arguments.size() + 1) {
        return std::unexpected(TemplateError{
            TemplateErrorCode::ArityMismatch,
            "Shell script is missing JSValue arg",
        });
    }

    std::size_t outputSize{0};
    for (const auto& raw : rawSegments) outputSize += raw.size();
    for (const auto& argument : arguments) {
        for (const auto& word : argument.words) outputSize += word.size() + 3;
    }

    // Probe the script for the quote context at each interpolation site. Text sites get a
    // \x08__bunstr_N marker; object-ref sites get the real \x08__bun_N reference so the
    // parser rejects them exactly where bun does (e.g. a buffer in command-name position
    // is "expected a command or assignment but got: \"JSObjRef\""). Marker indices must
    // stay dense -- the lexer errors on any unseen one -- so only text sites are numbered,
    // and `contextOfArgument` maps an argument back to its marker.
    std::uint32_t jsobjsLen{0};
    for (const auto& argument : arguments) {
        if (argument.jsObjIndex) jsobjsLen = std::max(jsobjsLen, *argument.jsObjIndex + 1);
    }

    std::string markerScript;
    std::vector<std::size_t> contextOfArgument(arguments.size(), 0);
    std::size_t markerCount{0};
    markerScript.reserve(outputSize + arguments.size() * 20);
    for (std::size_t index{0}; index < rawSegments.size(); ++index) {
        markerScript += rawSegments[index];
        if (index >= arguments.size()) continue;
        if (const auto objIndex{arguments[index].jsObjIndex}) {
            std::format_to(std::back_inserter(markerScript), "\x08__bun_{}", *objIndex);
        } else {
            contextOfArgument[index] = markerCount;
            std::format_to(std::back_inserter(markerScript), "\x08__bunstr_{}", markerCount);
            ++markerCount;
        }
    }
    const auto analysis{analyze_template_markers(markerScript, markerCount, jsobjsLen)};
    if (!analysis.errors.empty()) {
        return std::unexpected(TemplateError{TemplateErrorCode::Parse, analysis.errors});
    }

    std::string output;
    output.reserve(outputSize);
    for (std::size_t i{0}; i < rawSegments.size(); ++i) {
        const auto& raw{rawSegments[i]};
        if (raw.find('\0') != std::string::npos) {
            return std::unexpected(TemplateError{
                TemplateErrorCode::NullByte,
                "The shell script must not contain null bytes",
            });
        }
        output += raw;
        if (i == arguments.size()) continue;

        // An object ref carries no text: emit the reference itself, as bun writes
        // LEX_JS_OBJREF_PREFIX ++ idx straight into out_script (shell_body.rs:808-814).
        if (const auto objIndex{arguments[i].jsObjIndex}) {
            std::format_to(std::back_inserter(output), "\x08__bun_{}", *objIndex);
            continue;
        }

        const auto& words{arguments[i].words};
        for (std::size_t wordIndex{0}; wordIndex < words.size(); ++wordIndex) {
            if (words[wordIndex].find('\0') != std::string::npos) {
                return std::unexpected(TemplateError{
                    TemplateErrorCode::NullByte,
                    "The shell argument must be a string without null bytes",
                });
            }
            if (wordIndex != 0) output.push_back(' ');
            detail::append_word(output, words[wordIndex], analysis.contexts[contextOfArgument[i]]);
        }
    }

    if (const std::string errors{parse_errors(output, jsobjsLen)}; !errors.empty()) {
        return std::unexpected(TemplateError{TemplateErrorCode::Parse, std::move(errors)});
    }
    return output;
}

}  // namespace mbun::shell
