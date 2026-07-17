export module mbun.clap.parser;

import std;
import mbun.clap.argument_schema;
import mbun.clap.error;

export namespace mbun::clap {

struct ParsedArgument {
    const ArgumentSpec* spec {};
    std::string_view value {};
    bool present {false};
};

struct ParseResult {
    std::vector<ParsedArgument> arguments {};
    std::vector<std::string_view> positionals {};
};

[[nodiscard]] auto parse(const ArgumentSchema& schema, std::span<const std::string_view> argv)
    -> std::expected<ParseResult, Error>;

}

namespace mbun::clap {

namespace {
[[nodiscard]] auto find_long(std::span<const ArgumentSpec> specs, std::string_view name) -> const ArgumentSpec* {
    for (const auto& spec : specs) if (matches_long(spec, name)) return &spec;
    return nullptr;
}

[[nodiscard]] auto find_short(std::span<const ArgumentSpec> specs, char name) -> const ArgumentSpec* {
    for (const auto& spec : specs) if (matches_short(spec, name)) return &spec;
    return nullptr;
}

[[nodiscard]] auto already_present(const ParseResult& result, const ArgumentSpec* spec) -> bool {
    return std::ranges::any_of(result.arguments, [spec](const auto& item) { return item.spec == spec; });
}

[[nodiscard]] auto add_argument(ParseResult& result, const ArgumentSpec* spec, std::string_view value)
    -> std::expected<void, Error> {
    if (!spec->multiple && already_present(result, spec))
        return std::unexpected(Error {ErrorCode::InvalidValue, spec->name, "argument repeated"});
    result.arguments.push_back(ParsedArgument {.spec = spec, .value = value, .present = true});
    return {};
}
}

auto parse(const ArgumentSchema& schema, std::span<const std::string_view> argv)
    -> std::expected<ParseResult, Error> {
    ParseResult result {};
    for (std::size_t index {0}; index < argv.size(); ++index) {
        const auto token {argv[index]};
        if (token == "--") {
            for (++index; index < argv.size(); ++index) result.positionals.push_back(argv[index]);
            break;
        }
        if (token.starts_with("--")) {
            const auto body {token.substr(2)};
            const auto equals {body.find('=')};
            const auto name {body.substr(0, equals)};
            const auto* spec {find_long(schema.arguments, name)};
            if (spec == nullptr) return std::unexpected(Error {ErrorCode::UnknownArgument, token, name});
            if (spec->kind == ArgumentKind::Positional)
                return std::unexpected(Error {ErrorCode::UnexpectedValue, token, "positional"});
            std::string_view value {};
            if (spec->kind == ArgumentKind::Flag) {
                if (equals != std::string_view::npos)
                    return std::unexpected(Error {ErrorCode::UnexpectedValue, token, "flag"});
            } else if (equals != std::string_view::npos) {
                value = body.substr(equals + 1);
                if (value.empty()) return std::unexpected(Error {ErrorCode::MissingValue, token, name});
            } else if (++index < argv.size()) {
                value = argv[index];
            } else {
                return std::unexpected(Error {ErrorCode::MissingValue, token, name});
            }
            if (auto added {add_argument(result, spec, value)}; !added) return std::unexpected(added.error());
            continue;
        }
        if (token.size() >= 2 && token[0] == '-') {
            for (std::size_t offset {1}; offset < token.size(); ++offset) {
                const auto* spec {find_short(schema.arguments, token[offset])};
                if (spec == nullptr) return std::unexpected(Error {ErrorCode::UnknownArgument, token, token});
                if (spec->kind == ArgumentKind::Flag) {
                    if (auto added {add_argument(result, spec, {})}; !added) return std::unexpected(added.error());
                    continue;
                }
                std::string_view value {};
                if (offset + 1 < token.size()) value = token.substr(offset + 1);
                else if (++index < argv.size()) value = argv[index];
                else return std::unexpected(Error {ErrorCode::MissingValue, token, spec->name});
                if (auto added {add_argument(result, spec, value)}; !added) return std::unexpected(added.error());
                break;
            }
            continue;
        }
        result.positionals.push_back(token);
    }
    for (const auto& spec : schema.arguments)
        if (spec.required && !already_present(result, &spec))
            return std::unexpected(Error {ErrorCode::MissingRequired, spec.name, spec.name});
    return result;
}

}
