// ini.cppm — Bun/npm-compatible INI parser kernel.
// ref: bun Rust src/ini/lib.rs Parser::parse/prepare_str/parse_env_substitution
// ref: bun Zig src/ini/ini.zig
export module mbun.config.ini;

import std;
import mbun.config.jsonc;
import mbun.config.value;

namespace mbun::config::ini {

export using EnvLookup = std::function<std::optional<std::string_view>(std::string_view)>;

namespace {

std::string_view trim(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' ||
                              value.front() == '\n' || value.front() == '\r')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                              value.back() == '\n' || value.back() == '\r')) {
        value.remove_suffix(1);
    }
    return value;
}

bool is_quoted(std::string_view value) noexcept {
    return !value.empty() && ((value.front() == '"' && value.back() == '"') ||
                                 (value.front() == '\'' && value.back() == '\''));
}

std::string js_to_string(const Value& value) {
    switch (value.type()) {
    case Type::String: return value.as_string();
    case Type::Boolean: return value.as_bool() ? "true" : "false";
    case Type::Null: return "null";
    case Type::Integer: return std::to_string(value.as_integer());
    case Type::Float: return std::format("{}", value.as_double());
    case Type::Object: return "[Object object]";
    case Type::Array: {
        std::string out;
        for (std::size_t i{}; i < value.size(); ++i) {
            if (i) {
                out.push_back(',');
            }
            out += js_to_string(value.at(i));
        }
        return out;
    }
    }
    std::unreachable();
}

class Parser {
public:
    Parser(std::string_view source, EnvLookup lookup) : source_{source}, lookup_{std::move(lookup)} {}

    Value parse_document() {
        Value root{Value::make_object()};
        Value* head{&root};
        bool skipUntilSection{};
        std::size_t lineStart{};
        while (lineStart <= source_.size()) {
            std::size_t lineEnd{source_.find('\n', lineStart)};
            if (lineEnd == std::string_view::npos) {
                lineEnd = source_.size();
            }
            std::string_view line{source_.substr(lineStart, lineEnd - lineStart)};
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            if (!should_skip_line_(line)) {
                bool treatAsKey{};
                if (line.front() == '[') {
                    skipUntilSection = false;
                    const auto close{line.find(']')};
                    if (close != std::string_view::npos) {
                        treatAsKey = !trim(line.substr(close + 1)).empty();
                        if (!treatAsKey) {
                            auto path{prepare_section_(line.substr(1, close - 1))};
                            head = descend_(root, path);
                            skipUntilSection = head == nullptr;
                        }
                    }
                    if (!treatAsKey) {
                        lineStart = lineEnd + 1;
                        continue;
                    }
                }
                if (!skipUntilSection && head != nullptr) {
                    parse_assignment_(*head, line);
                }
            }
            if (lineEnd == source_.size()) {
                break;
            }
            lineStart = lineEnd + 1;
        }
        return root;
    }

private:
    std::string_view source_;
    EnvLookup lookup_;

    static bool should_skip_line_(std::string_view line) noexcept {
        if (line.empty() || line.front() == ';' || line.front() == '#') {
            return true;
        }
        for (const char c : line) {
            if (c == '#' || c == ';') {
                return true;
            }
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                return false;
            }
        }
        return true;
    }

    static Value* find_or_add_(Value& object, std::string key, Value value) {
        if (Value* existing{object.get(key)}) {
            return existing;
        }
        auto& entries{object.object().entries};
        entries.emplace_back(std::move(key), std::move(value));
        return &entries.back().second;
    }

    static Value* descend_(Value& root, const std::vector<std::string>& path) {
        Value* current{&root};
        for (const auto& segment : path) {
            if (!current->is_object()) {
                return nullptr;
            }
            Value* child{current->get(segment)};
            if (child == nullptr) {
                child = find_or_add_(*current, segment, Value::make_object());
            } else if (!child->is_object()) {
                return nullptr;
            }
            current = child;
        }
        return current;
    }

    std::optional<std::size_t> expand_at_(std::string_view input, std::size_t start,
                                          std::size_t current, std::size_t depth,
                                          std::string& out) const {
        if (depth >= 32 || current + 2 >= input.size() || input[current] != '$' ||
            input[current + 1] != '{') {
            return std::nullopt;
        }
        bool escaped{};
        for (std::size_t j{current + 2}; j < input.size(); ++j) {
            switch (input[j]) {
            case '\\': escaped = !escaped; break;
            case '$':
                if (!escaped) {
                    return expand_at_(input, start, j, depth + 1, out);
                }
                escaped = false;
                break;
            case '{':
                if (!escaped) {
                    return std::nullopt;
                }
                escaped = false;
                break;
            case '}':
                if (!escaped) {
                    std::string_view name{input.substr(current + 2, j - current - 2)};
                    bool optional{!name.empty() && name.back() == '?'};
                    if (optional) {
                        name.remove_suffix(1);
                    }
                    const auto replacement{lookup_ ? lookup_(name) : std::nullopt};
                    if (replacement) {
                        if (start != current) {
                            out.append(input.substr(start, current - start));
                        }
                        out.append(*replacement);
                    } else if (!optional) {
                        return std::nullopt;
                    }
                    return j;
                }
                escaped = false;
                break;
            default: escaped = false; break;
            }
        }
        return std::nullopt;
    }

    std::string expand_quoted_(std::string_view input) const {
        if (input.find("${") == std::string_view::npos) {
            return std::string{input};
        }
        std::string out;
        out.reserve(input.size());
        for (std::size_t i{}; i < input.size(); ++i) {
            if (input[i] == '$' && i + 2 < input.size() && input[i + 1] == '{') {
                std::size_t depth{1};
                std::size_t end{i + 2};
                while (end < input.size() && depth > 0) {
                    if (input[end] == '{') {
                        ++depth;
                    } else if (input[end] == '}') {
                        --depth;
                    }
                    if (depth > 0) {
                        ++end;
                    }
                }
                if (depth == 0) {
                    std::string_view name{input.substr(i + 2, end - i - 2)};
                    const bool optional{!name.empty() && name.back() == '?'};
                    if (optional) {
                        name.remove_suffix(1);
                    }
                    if (const auto replacement{lookup_ ? lookup_(name) : std::nullopt}) {
                        out.append(*replacement);
                    } else if (!optional) {
                        out.append(input.substr(i, end - i + 1));
                    }
                    i = end;
                    continue;
                }
            }
            out.push_back(input[i]);
        }
        return out;
    }

    std::string prepare_unquoted_(std::string_view input, bool expandEnv) const {
        input = trim(input);
        std::string out;
        out.reserve(input.size());
        for (std::size_t i{}; i < input.size(); ++i) {
            const char c{input[i]};
            if (c == ';' || c == '#') {
                break;
            }
            if (c == '\\') {
                if (i + 1 >= input.size()) {
                    out.push_back('\\');
                    break;
                }
                const char next{input[++i]};
                if (next == '\\' || next == ';' || next == '#' || next == '$') {
                    out.push_back(next);
                } else {
                    out.push_back('\\');
                    out.push_back(next);
                }
                continue;
            }
            if (expandEnv && c == '$') {
                const std::size_t before{out.size()};
                if (const auto end{expand_at_(input, i, i, 0, out)}) {
                    i = *end;
                    continue;
                }
                out.resize(before);
            }
            out.push_back(c);
        }
        return out;
    }

    std::string prepare_key_(std::string_view input) const {
        input = trim(input);
        if (is_quoted(input)) {
            std::string_view candidate{input};
            if (candidate.front() == '\'') {
                candidate.remove_prefix(1);
                if (!candidate.empty()) {
                    candidate.remove_suffix(1);
                }
            }
            if (!candidate.empty()) {
                if (auto parsed{jsonc::parse_strict(candidate)}) {
                    return js_to_string(*parsed);
                }
            }
            return std::string{candidate};
        }
        return prepare_unquoted_(input, false);
    }

    std::vector<std::string> prepare_section_(std::string_view input) const {
        input = trim(input);
        if (is_quoted(input)) {
            std::string key{prepare_key_(input)};
            std::vector<std::string> result;
            std::size_t start{};
            while (true) {
                const auto dot{key.find('.', start)};
                result.emplace_back(key.substr(start, dot - start));
                if (dot == std::string::npos) {
                    return result;
                }
                start = dot + 1;
            }
        }
        std::vector<std::string> result;
        std::string segment;
        for (std::size_t i{}; i < input.size(); ++i) {
            if (input[i] == ';' || input[i] == '#') {
                break;
            }
            if (input[i] == '\\' && i + 1 < input.size()) {
                const char next{input[++i]};
                if (next == '.' || next == ';' || next == '#' || next == '\\') {
                    segment.push_back(next);
                } else {
                    segment.push_back('\\');
                    segment.push_back(next);
                }
            } else if (input[i] == '.' && result.size() < 511) {
                result.push_back(std::string{trim(segment)});
                segment.clear();
            } else {
                segment.push_back(input[i]);
            }
        }
        result.push_back(std::string{trim(segment)});
        return result;
    }

    Value prepare_value_(std::string_view input) const {
        input = trim(input);
        if (is_quoted(input)) {
            std::string_view candidate{input};
            if (candidate.front() == '\'') {
                candidate.remove_prefix(1);
                if (!candidate.empty()) {
                    candidate.remove_suffix(1);
                }
            }
            if (!candidate.empty()) {
                if (auto parsed{jsonc::parse_strict(candidate)}) {
                    if (parsed->is_string()) {
                        return Value::string(expand_quoted_(parsed->as_string()));
                    }
                    return std::move(*parsed);
                }
            }
            return Value::string(expand_quoted_(candidate));
        }
        std::string value{prepare_unquoted_(input, true)};
        if (value == "true") {
            return Value::boolean(true);
        }
        if (value == "false") {
            return Value::boolean(false);
        }
        if (value == "null") {
            return Value::null();
        }
        return Value::string(std::move(value));
    }

    void parse_assignment_(Value& head, std::string_view line) {
        const auto equal{line.find('=')};
        std::string key{prepare_key_(line.substr(0, equal))};
        const bool arrayKey{key.size() > 2 && key.ends_with("[]")};
        if (arrayKey) {
            key.resize(key.size() - 2);
        }
        if (key == "__proto__") {
            return;
        }
        Value value{equal == std::string_view::npos ? Value::boolean(true)
                                                    : prepare_value_(line.substr(equal + 1))};
        Value* existing{head.get(key)};
        if (arrayKey && existing == nullptr) {
            Value array{Value::make_array()};
            head.object().entries.emplace_back(std::move(key), std::move(array));
            existing = &head.object().entries.back().second;
        } else if (arrayKey && existing != nullptr && !existing->is_array()) {
            Value array{Value::make_array()};
            array.array().push_back(std::move(*existing));
            *existing = std::move(array);
        }
        if (existing != nullptr && existing->is_array()) {
            existing->array().push_back(std::move(value));
        } else if (existing != nullptr) {
            *existing = std::move(value);
        } else {
            head.object().entries.emplace_back(std::move(key), std::move(value));
        }
    }
};

} // namespace

export std::expected<Value, ParseError> parse(std::string_view source, EnvLookup lookup = {}) {
    return Parser{source, std::move(lookup)}.parse_document();
}

} // namespace mbun::config::ini
