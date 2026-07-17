// parser.cppm — mbun.ini.parser: core INI parser + ${} env expansion.
//
// Mechanical port of `ini::Parser` from bun's src/ini/lib.rs (the pure parsing
// algorithm: sections `[a.b]`, `key=value`, `;`/`#` comments, single/double
// quotes -> JSON, `key[]=` arrays, `\` escapes, and `${VAR}` / `${VAR?}` env
// substitution). Source `Loc`/arena/`Source` threading is dropped — this
// produces a plain `ini::Value` tree.
//
// DEFERRED(S-install): load_npmrc / ConfigIterator / ScopeIterator / pnpm
// matcher live in bun's same file but couple to bun install / npm-registry /
// URL infra; they are out of scope for this pure-logic member.
export module mbun.ini.parser;

import std;
import mbun.ini.value;
import mbun.ini.json;

namespace mbun::ini {

// Env lookup: returns the value for a variable name, or nullopt if undefined.
// Mirrors `DotEnvLoader::get`.
export using EnvLookup =
    std::function<std::optional<std::string>(std::string_view)>;

export struct Options {
    // npm/ini `key[] = v` bracketed-array syntax. bun currently hardcodes this
    // on (the alternate duplicate-key mode is unsupported upstream too).
    bool brackedArray{true};
};

// ── pure byte helpers (bun: free fns, unit-testable) ────────────────────────

// Hard cap on dot-separated segments in a section header, mirrors bun's
// MAX_SECTION_ROPE_SEGMENTS (guards against unbounded recursion upstream).
inline constexpr std::size_t MAX_SECTION_ROPE_SEGMENTS = 512;
inline constexpr std::size_t MAX_ENV_SUBSTITUTION_DEPTH = 32;

export bool should_skip_line(std::string_view line) {
    if (line.empty() || line[0] == ';' || line[0] == '#') {
        return true;
    }
    for (char c : line) {
        switch (c) {
            case ' ':
            case '\t':
            case '\n':
            case '\r':
                break;
            case '#':
            case ';':
                return true;
            default:
                return false;
        }
    }
    return true;
}

export bool is_quoted(std::string_view val) {
    if (val.empty()) {
        return false;
    }
    return (val.front() == '"' && val.back() == '"') ||
           (val.front() == '\'' && val.back() == '\'');
}

inline std::string_view trim_ws(std::string_view s) {
    constexpr std::string_view WS = " \n\r\t";
    std::size_t b = s.find_first_not_of(WS);
    if (b == std::string_view::npos) {
        return {};
    }
    std::size_t e = s.find_last_not_of(WS);
    return s.substr(b, e - b + 1);
}

inline bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.substr(s.size() - suffix.size()) == suffix;
}

// bun_core::utf8_byte_sequence_length: bytes in the sequence led by `c`.
inline int utf8_seq_len(unsigned char c) {
    if (c < 0x80) {
        return 1;
    }
    if ((c >> 5) == 0x6) {
        return 2;
    }
    if ((c >> 4) == 0xE) {
        return 3;
    }
    if ((c >> 3) == 0x1E) {
        return 4;
    }
    return 0;
}

// Split `key` on '.' into rope segments (unescaped dots only), capped at
// MAX_SECTION_ROPE_SEGMENTS; the remainder past the cap becomes one segment.
inline std::vector<std::string> str_to_rope(std::string_view key) {
    std::vector<std::string> segs;
    std::size_t start = 0;
    while (true) {
        std::size_t dot = key.find('.', start);
        if (dot == std::string_view::npos ||
            segs.size() + 1 >= MAX_SECTION_ROPE_SEGMENTS) {
            segs.emplace_back(key.substr(start));
            break;
        }
        segs.emplace_back(key.substr(start, dot - start));
        start = dot + 1;
    }
    return segs;
}

// ── Parser ──────────────────────────────────────────────────────────────────

enum class Usage { Section, Key, Value };

struct Prepared {
    Value value{Value::null()};      // Usage::Value
    std::vector<std::string> section;  // Usage::Section
    std::string key;                   // Usage::Key
};

export class Parser {
public:
    Parser(std::string_view src, EnvLookup env, Options opts)
        : src_{src}, env_{std::move(env)}, opts_{opts} {}

    // Parse the whole document into a root object Value.
    Value parse() {
        Value root = Value::make_object();
        Value* head = &root;  // current section object
        bool skipUntilNextSection = false;

        std::size_t pos = 0;
        while (pos <= src_.size()) {
            std::size_t nl = src_.find('\n', pos);
            std::string_view rawLine;
            if (nl == std::string_view::npos) {
                rawLine = src_.substr(pos);
                pos = src_.size() + 1;  // terminate after this iteration
            } else {
                rawLine = src_.substr(pos, nl - pos);
                pos = nl + 1;
            }

            std::string_view line = rawLine;
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            if (should_skip_line(line)) {
                continue;
            }

            // Section: [foo]
            if (line[0] == '[') {
                bool treatAsKey = false;
                bool handled = false;
                skipUntilNextSection = false;
                std::size_t close = line.find(']');
                if (close == std::string_view::npos) {
                    continue;  // skip whole line
                }
                // The rest after ']' must be whitespace, else treat as key.
                if (close + 1 < line.size()) {
                    for (char c : line.substr(close + 1)) {
                        if (c != ' ' && c != '\t') {
                            treatAsKey = true;
                            break;
                        }
                    }
                }
                if (!treatAsKey) {
                    Prepared sec = prepare_str(Usage::Section,
                                               line.substr(1, close - 1));
                    Value* parent = get_or_put_object(root, sec.section);
                    if (parent == nullptr) {
                        // Clobber: key exists but is not an object. Match npm/ini
                        // by skipping until the next section.
                        skipUntilNextSection = true;
                    } else {
                        head = parent;
                    }
                    handled = true;
                }
                if (!handled && !treatAsKey) {
                    continue;
                }
                if (!treatAsKey) {
                    continue;
                }
                // fallthrough: treat the whole line as a key=value below.
            }
            if (skipUntilNextSection) {
                continue;
            }

            // key = value
            std::size_t eq = line.find('=');
            std::string_view keyPart =
                line.substr(0, eq == std::string_view::npos ? line.size() : eq);
            Prepared keyPrep = prepare_str(Usage::Key, keyPart);
            std::string keyRaw = std::move(keyPrep.key);

            bool isArray = keyRaw.size() > 2 && ends_with(keyRaw, "[]");

            std::string key = (isArray && ends_with(keyRaw, "[]"))
                                  ? keyRaw.substr(0, keyRaw.size() - 2)
                                  : keyRaw;

            if (key == "__proto__") {
                continue;
            }

            Value valueRaw = Value::boolean(true);  // bare key -> true
            if (eq != std::string_view::npos) {
                if (eq + 1 < line.size()) {
                    valueRaw =
                        std::move(prepare_str(Usage::Value, line.substr(eq + 1))
                                      .value);
                } else {
                    valueRaw = Value::string("");
                }
            }

            // Unquoted true/false/null literals coerce to their JS values.
            Value value = [&]() -> Value {
                if (valueRaw.is_string()) {
                    const std::string& s = valueRaw.as_string();
                    if (s == "true") {
                        return Value::boolean(true);
                    }
                    if (s == "false") {
                        return Value::boolean(false);
                    }
                    if (s == "null") {
                        return Value::null();
                    }
                }
                return std::move(valueRaw);
            }();

            if (isArray) {
                if (Value* existing = head->get(key)) {
                    if (!existing->is_array()) {
                        Value arr = Value::make_array();
                        arr.array().push_back(std::move(*existing));
                        head->put(key, std::move(arr));
                    }
                } else {
                    head->put(key, Value::make_array());
                }
            }

            // Safeguard: appending to a previously defined array even without
            // the brackets, rather than clobbering it.
            bool wasAlreadyArray = false;
            if (Value* existing = head->get(key)) {
                if (existing->is_array()) {
                    wasAlreadyArray = true;
                    existing->array().push_back(std::move(value));
                }
            }
            if (!wasAlreadyArray) {
                head->put(key, std::move(value));
            }
        }
        return root;
    }

private:
    std::string_view src_;
    EnvLookup env_;
    Options opts_;

    std::optional<std::string> env_get(std::string_view name) {
        if (!env_) {
            return std::nullopt;
        }
        return env_(name);
    }

    // Descend/create the nested object for a section rope. nullptr on Clobber
    // (a segment exists but is not an object).
    static Value* get_or_put_object(Value& root,
                                    const std::vector<std::string>& segs) {
        Value* cur = &root;
        for (const std::string& seg : segs) {
            Value* child = cur->object().find(seg);
            if (child == nullptr) {
                cur->put(seg, Value::make_object());
                child = cur->object().find(seg);
            } else if (!child->is_object()) {
                return nullptr;
            }
            cur = child;
        }
        return cur;
    }

    // Port of `Parser::prepare_str`. Dispatches on `usage`; the section rope is
    // returned as a segment list.
    Prepared prepare_str(Usage usage, std::string_view val_) {
        std::string_view val = trim_ws(val_);

        if (is_quoted(val)) {
            std::optional<Prepared> r = prepare_quoted(usage, val);
            if (r) {
                return std::move(*r);
            }
            // fall through to the plain-string result below (val may have been
            // single-quote-stripped by prepare_quoted).
        } else {
            return prepare_unquoted(usage, val);
        }

        // fallthrough (empty quoted value, or JSON parse failed for a
        // non-Value usage): treat as a plain string.
        Prepared out;
        if (usage == Usage::Value) {
            out.value = Value::string(std::string{val});
        } else if (usage == Usage::Key) {
            out.key = std::string{val};
        } else {
            out.section = str_to_rope(val);
        }
        return out;
    }

    // Quoted branch. Returns nullopt to signal "fall through to string path";
    // `val` is updated in place if a leading single quote was stripped.
    std::optional<Prepared> prepare_quoted(Usage usage, std::string_view& val) {
        // Strip single quotes before JSON.parse.
        if (!val.empty() && val.front() == '\'') {
            val = val.size() > 1 ? val.substr(1, val.size() - 2) : val.substr(1);
        }
        if (val.empty()) {
            return std::nullopt;  // -> string fallthrough
        }

        std::optional<Value> parsed = json::parse(val);
        if (!parsed) {
            // JSON failed (e.g. single-quoted '${VAR}'): still expand env vars.
            if (usage == Usage::Value) {
                Prepared out;
                out.value = Value::string(expand_env_vars(val));
                return out;
            }
            return std::nullopt;  // Section/Key -> string fallthrough
        }

        Value jsonVal = std::move(*parsed);

        if (jsonVal.is_string()) {
            std::string str = jsonVal.as_string();
            std::string expanded =
                usage == Usage::Value ? expand_env_vars(str) : str;
            Prepared out;
            if (usage == Usage::Value) {
                out.value = Value::string(std::move(expanded));
            } else if (usage == Usage::Section) {
                out.section = str_to_rope(expanded);
            } else {
                out.key = std::move(expanded);
            }
            return out;
        }

        if (usage == Usage::Value) {
            // Preserve array/object/number/bool/null tags.
            Prepared out;
            out.value = std::move(jsonVal);
            return out;
        }

        // Section/Key with a non-string JSON value: npm/ini stringifies it.
        Prepared out;
        if (jsonVal.is_object()) {
            if (usage == Usage::Section) {
                out.section = {"[Object object]"};
            } else {
                out.key = "[Object object]";
            }
        } else {
            std::string s = to_string(jsonVal);
            if (usage == Usage::Section) {
                out.section = {std::move(s)};
            } else {
                out.key = std::move(s);
            }
        }
        return out;
    }

    // Unquoted branch: walk the value handling escapes, comments, `${}` env
    // substitution, and (for sections) dot-splitting into rope segments.
    Prepared prepare_unquoted(Usage usage, std::string_view val) {
        std::string unesc;
        std::vector<std::string> rope;   // section segments
        std::size_t ropeParts = 0;
        bool didAnyEscape = false;
        bool esc = false;

        auto commit_rope_part = [&]() {
            rope.emplace_back(unesc);
            unesc.clear();
        };

        std::size_t i = 0;
        while (i < val.size()) {
            unsigned char c = static_cast<unsigned char>(val[i]);
            if (esc) {
                switch (c) {
                    case '\\':
                        unesc += '\\';
                        break;
                    case ';':
                    case '#':
                    case '$':
                        unesc += static_cast<char>(c);
                        break;
                    case '.':
                        if (usage == Usage::Section) {
                            unesc += '.';
                        } else {
                            unesc += "\\.";
                        }
                        break;
                    default: {
                        int len = utf8_seq_len(c);
                        if (len <= 1) {
                            unesc += '\\';
                            unesc += static_cast<char>(c);
                        } else {
                            std::size_t avail = val.size() - i;
                            std::size_t take =
                                std::min<std::size_t>(len, avail);
                            unesc += '\\';
                            unesc.append(val.substr(i, take));
                            i += take - 1;
                        }
                        break;
                    }
                }
                esc = false;
            } else {
                switch (c) {
                    case '$': {
                        bool substituted = false;
                        if (usage == Usage::Value) {
                            std::optional<std::size_t> newI =
                                parse_env_substitution(val, i, i, 0, unesc);
                            if (newI) {
                                didAnyEscape = true;
                                i = *newI + 1;
                                substituted = true;
                            }
                        }
                        if (substituted) {
                            continue;
                        }
                        unesc += '$';
                        break;
                    }
                    case ';':
                    case '#':
                        goto done;  // start of a comment
                    case '\\':
                        esc = true;
                        didAnyEscape = true;
                        break;
                    case '.':
                        if (usage == Usage::Section &&
                            ropeParts < MAX_SECTION_ROPE_SEGMENTS) {
                            commit_rope_part();
                            ++ropeParts;
                        } else {
                            unesc += '.';
                        }
                        break;
                    default: {
                        int len = utf8_seq_len(c);
                        if (len <= 1) {
                            unesc += static_cast<char>(c);
                        } else {
                            std::size_t avail = val.size() - i;
                            std::size_t take =
                                std::min<std::size_t>(len, avail);
                            unesc.append(val.substr(i, take));
                            i += take - 1;
                        }
                        break;
                    }
                }
            }
            ++i;
        }
    done:
        if (esc) {
            unesc += '\\';
        }

        Prepared out;
        switch (usage) {
            case Usage::Section:
                commit_rope_part();
                out.section = std::move(rope);
                break;
            case Usage::Value:
                if (!didAnyEscape) {
                    out.value = Value::string(std::string{val});
                } else {
                    out.value = Value::string(std::move(unesc));
                }
                break;
            case Usage::Key:
                if (!didAnyEscape) {
                    out.key = std::string{val};
                } else {
                    out.key = std::move(unesc);
                }
                break;
        }
        return out;
    }

    // Expands ${VAR} / ${VAR?} in a string (used for quoted values after JSON
    // parsing has already handled escapes).
    std::string expand_env_vars(std::string_view val) {
        if (val.find("${") == std::string_view::npos) {
            return std::string{val};
        }
        std::string result;
        result.reserve(val.size());
        std::size_t i = 0;
        while (i < val.size()) {
            if (val[i] == '$' && i + 2 < val.size() && val[i + 1] == '{') {
                std::size_t j = i + 2;
                std::size_t depth = 1;
                while (j < val.size() && depth > 0) {
                    if (val[j] == '{') {
                        ++depth;
                    } else if (val[j] == '}') {
                        --depth;
                    }
                    if (depth > 0) {
                        ++j;
                    }
                }
                if (depth == 0) {
                    std::string_view raw = val.substr(i + 2, j - (i + 2));
                    bool optional = !raw.empty() && raw.back() == '?';
                    std::string_view name =
                        optional ? raw.substr(0, raw.size() - 1) : raw;
                    if (auto v = env_get(name)) {
                        result += *v;
                    } else if (!optional) {
                        result.append(val.substr(i, j + 1 - i));
                    }
                    // optional & missing -> expands to empty.
                    i = j + 1;
                    continue;
                }
            }
            result += val[i];
            ++i;
        }
        return result;
    }

    // Returns the index of the closing '}' to skip past, or nullopt if the
    // '$' at `i` does not begin an env substitution. Selects the innermost
    // ${...}; supports ${VAR} (undefined -> nullopt) and ${VAR?} (undefined ->
    // empty). Appends the substitution result into `unesc`.
    std::optional<std::size_t> parse_env_substitution(std::string_view val,
                                                      std::size_t start,
                                                      std::size_t i,
                                                      std::size_t depth,
                                                      std::string& unesc) {
        if (depth >= MAX_ENV_SUBSTITUTION_DEPTH) {
            return std::nullopt;
        }
        bool esc = false;
        if (i + 2 < val.size() && val[i + 1] == '{') {
            bool foundClosing = false;
            std::size_t j = i + 2;
            while (j < val.size()) {
                char cj = val[j];
                if (cj == '\\') {
                    esc = !esc;
                } else if (cj == '$') {
                    if (!esc) {
                        return parse_env_substitution(val, start, j, depth + 1,
                                                      unesc);
                    }
                } else if (cj == '{') {
                    if (!esc) {
                        return std::nullopt;
                    }
                } else if (cj == '}') {
                    if (!esc) {
                        foundClosing = true;
                        break;
                    }
                }
                // bun only toggles `esc` on backslash; it is intentionally not
                // reset on other bytes.
                ++j;
            }

            if (!foundClosing) {
                return std::nullopt;
            }

            if (start != i) {
                unesc.append(val.substr(start, i - start));
            }

            std::string_view raw = val.substr(i + 2, j - (i + 2));
            bool optional = !raw.empty() && raw.back() == '?';
            std::string_view name =
                optional ? raw.substr(0, raw.size() - 1) : raw;

            if (auto v = env_get(name)) {
                unesc += *v;
            } else if (!optional) {
                return std::nullopt;  // leave as-is
            }
            // optional & missing -> empty.
            return j;
        }
        return std::nullopt;
    }
};

// Convenience: parse `src` into a root object Value.
export Value parse(std::string_view src, EnvLookup env = {},
                   Options opts = {}) {
    Parser p{src, std::move(env), opts};
    return p.parse();
}

}  // namespace mbun::ini
