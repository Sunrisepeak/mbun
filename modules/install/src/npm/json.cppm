// npm/json.cppm — mbun.install.npm.json
//
// A small self-contained JSON reader for parsing npm registry packuments.
// bun's npm.rs parses the manifest with its own AST JSON parser
// (`JSON::ParsedJson::parse_npm_manifest`); here we hand-roll an equivalent
// tree parser over std::string_view so the install module stays self-contained
// (no dependency on the transpiler's internal JSON reader).
//
// Design (MC++ over the reference):
//   - object member order is preserved (npm packuments are order-sensitive for
//     the version list; bun iterates `versions` in document order)
//   - string values that contain no escapes stay as zero-copy views into the
//     source; escaped strings are decoded once into an owned pool string
//   - numbers are parsed as double (npm `fileCount`/`unpackedSize` fit)
export module mbun.install.npm.json;

import std;

namespace mbun::install::npm::json {

export enum class Kind : std::uint8_t { Null, Boolean, Number, String, Array, Object };

export struct Value;

// A single object member: key + value. Members keep insertion order.
export struct Member {
    std::string_view key;
    std::unique_ptr<Value> value;
};

export struct Value {
    Kind kind{Kind::Null};
    bool boolean{false};
    double number{0.0};
    std::string_view str;  // for String: view into source or into ownedStrings
    std::vector<std::unique_ptr<Value>> items;  // Array
    std::vector<Member> members;                // Object

    // Source layout of an Object/Array: true when the whole `{...}` / `[...]`
    // span sat on one line, which is what `stringify` replays.
    //   ref: bun-ref/src/parsers/json_stage2.rs:638-689 (`is_single_line =
    //        !newline_before(here)`, cleared as soon as any member token has a
    //        newline before it) and json.rs:1000/:1034, which forward the flag
    //        into `E::Object` / `E::Array`.
    // A freshly built node defaults to false — the same default bun's
    // `E::Object { .. ..Default::default() }` gets, which is exactly why an
    // edited root re-prints multi-line even when it was parsed single-line
    //   (ref: bun-ref/src/install/PackageManager/PackageJSONEditor.rs:1024-1030).
    bool singleLine{false};

    // ── typed accessors (mirror bun's `as_str` / `as_object` helpers) ────────
    bool is_object() const {
        return kind == Kind::Object;
    }
    bool is_array() const {
        return kind == Kind::Array;
    }

    std::optional<std::string_view> as_str() const {
        if (kind == Kind::String) {
            return str;
        }
        return std::nullopt;
    }

    std::optional<bool> as_bool() const {
        if (kind == Kind::Boolean) {
            return boolean;
        }
        return std::nullopt;
    }

    std::optional<double> as_number() const {
        if (kind == Kind::Number) {
            return number;
        }
        return std::nullopt;
    }

    // Object member lookup by key (returns nullptr if absent / not an object).
    const Value* get(std::string_view key) const {
        if (kind != Kind::Object) {
            return nullptr;
        }
        for (const auto& m : members) {
            if (m.key == key) {
                return m.value.get();
            }
        }
        return nullptr;
    }
};

// Owns the parse tree + any decoded (escaped) strings the views point into.
export struct Document {
    std::unique_ptr<Value> root;
    // Backing storage for decoded escaped strings; Value::str views may point
    // into these, so they must outlive the tree. std::string is used (not
    // string_view) since these bytes are synthesized, not in the source.
    std::deque<std::string> ownedStrings;
};

namespace detail {

class Parser {
public:
    Parser(std::string_view src, Document& doc) : src_{src}, doc_{doc} {}

    std::optional<std::unique_ptr<Value>> parse() {
        skip_ws_();
        auto v{parse_value_()};
        if (!v) {
            return std::nullopt;
        }
        skip_ws_();
        if (pos_ != src_.size()) {
            return std::nullopt;  // trailing garbage
        }
        return v;
    }

private:
    std::string_view src_;
    Document& doc_;
    std::size_t pos_{0};
    // guard against pathological deep nesting blowing the stack
    int depth_{0};
    static constexpr int MAX_DEPTH{512};

    void skip_ws_() {
        while (pos_ < src_.size()) {
            char c{src_[pos_]};
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool eof_() const {
        return pos_ >= src_.size();
    }

    std::optional<std::unique_ptr<Value>> parse_value_() {
        if (eof_()) {
            return std::nullopt;
        }
        char c{src_[pos_]};
        switch (c) {
            case '{':
                return parse_object_();
            case '[':
                return parse_array_();
            case '"':
                return parse_string_value_();
            case 't':
            case 'f':
                return parse_bool_();
            case 'n':
                return parse_null_();
            default:
                if (c == '-' || (c >= '0' && c <= '9')) {
                    return parse_number_();
                }
                return std::nullopt;
        }
    }

    std::optional<std::unique_ptr<Value>> parse_object_() {
        if (++depth_ > MAX_DEPTH) {
            return std::nullopt;
        }
        const std::size_t open{pos_};
        ++pos_;  // '{'
        auto v{std::make_unique<Value>()};
        v->kind = Kind::Object;
        skip_ws_();
        if (!eof_() && src_[pos_] == '}') {
            ++pos_;
            --depth_;
            v->singleLine = span_is_single_line_(open);
            return v;
        }
        while (true) {
            skip_ws_();
            if (eof_() || src_[pos_] != '"') {
                return std::nullopt;
            }
            auto key{parse_string_()};
            if (!key) {
                return std::nullopt;
            }
            skip_ws_();
            if (eof_() || src_[pos_] != ':') {
                return std::nullopt;
            }
            ++pos_;  // ':'
            skip_ws_();
            auto val{parse_value_()};
            if (!val) {
                return std::nullopt;
            }
            v->members.push_back(Member{*key, std::move(*val)});
            skip_ws_();
            if (eof_()) {
                return std::nullopt;
            }
            if (src_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (src_[pos_] == '}') {
                ++pos_;
                break;
            }
            return std::nullopt;
        }
        --depth_;
        v->singleLine = span_is_single_line_(open);
        return v;
    }

    std::optional<std::unique_ptr<Value>> parse_array_() {
        if (++depth_ > MAX_DEPTH) {
            return std::nullopt;
        }
        const std::size_t open{pos_};
        ++pos_;  // '['
        auto v{std::make_unique<Value>()};
        v->kind = Kind::Array;
        skip_ws_();
        if (!eof_() && src_[pos_] == ']') {
            ++pos_;
            --depth_;
            v->singleLine = span_is_single_line_(open);
            return v;
        }
        while (true) {
            skip_ws_();
            auto val{parse_value_()};
            if (!val) {
                return std::nullopt;
            }
            v->items.push_back(std::move(*val));
            skip_ws_();
            if (eof_()) {
                return std::nullopt;
            }
            if (src_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (src_[pos_] == ']') {
                ++pos_;
                break;
            }
            return std::nullopt;
        }
        --depth_;
        v->singleLine = span_is_single_line_(open);
        return v;
    }

    // True when the `{...}` / `[...]` that started at `open` and ends at the
    // current `pos_` contains no newline. bun tracks this token-by-token via
    // `newline_before` (json_stage2.rs:638-689); a raw newline can never occur
    // *inside* a JSON string token (it must be escaped as `\n`), so scanning the
    // closed span for '\n' is equivalent — and a nested multi-line value puts a
    // newline in the parent's span too, which is bun's behaviour as well.
    bool span_is_single_line_(std::size_t open) const {
        return src_.substr(open, pos_ - open).find('\n') == std::string_view::npos;
    }

    std::optional<std::unique_ptr<Value>> parse_string_value_() {
        auto s{parse_string_()};
        if (!s) {
            return std::nullopt;
        }
        auto v{std::make_unique<Value>()};
        v->kind = Kind::String;
        v->str = *s;
        return v;
    }

    // Parses a JSON string. Returns a view into the source (fast path, no
    // escapes) or into doc_.ownedStrings (decoded escapes).
    std::optional<std::string_view> parse_string_() {
        ++pos_;  // opening '"'
        std::size_t start{pos_};
        bool hasEscape{false};
        while (pos_ < src_.size()) {
            char c{src_[pos_]};
            if (c == '"') {
                std::string_view raw{src_.substr(start, pos_ - start)};
                ++pos_;  // closing '"'
                if (!hasEscape) {
                    return raw;
                }
                return decode_escapes_(raw);
            }
            if (c == '\\') {
                hasEscape = true;
                pos_ += 2;  // skip escape + next char
            } else {
                ++pos_;
            }
        }
        return std::nullopt;  // unterminated
    }

    std::string_view decode_escapes_(std::string_view raw) {
        std::string out;
        out.reserve(raw.size());
        for (std::size_t i{0}; i < raw.size(); ++i) {
            char c{raw[i]};
            if (c != '\\' || i + 1 >= raw.size()) {
                out.push_back(c);
                continue;
            }
            char e{raw[++i]};
            switch (e) {
                case 'n':
                    out.push_back('\n');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case '/':
                    out.push_back('/');
                    break;
                case '\\':
                    out.push_back('\\');
                    break;
                case '"':
                    out.push_back('"');
                    break;
                case 'u': {
                    if (i + 4 < raw.size()) {
                        std::uint32_t cp{0};
                        bool ok{true};
                        for (int k{0}; k < 4; ++k) {
                            char h{raw[i + 1 + static_cast<std::size_t>(k)]};
                            cp <<= 4;
                            if (h >= '0' && h <= '9') {
                                cp |= static_cast<std::uint32_t>(h - '0');
                            } else if (h >= 'a' && h <= 'f') {
                                cp |= static_cast<std::uint32_t>(h - 'a' + 10);
                            } else if (h >= 'A' && h <= 'F') {
                                cp |= static_cast<std::uint32_t>(h - 'A' + 10);
                            } else {
                                ok = false;
                                break;
                            }
                        }
                        if (ok) {
                            i += 4;
                            append_utf8_(out, cp);
                            break;
                        }
                    }
                    out.push_back('u');
                    break;
                }
                default:
                    out.push_back(e);
                    break;
            }
        }
        doc_.ownedStrings.push_back(std::move(out));
        return doc_.ownedStrings.back();
    }

    static void append_utf8_(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    std::optional<std::unique_ptr<Value>> parse_number_() {
        std::size_t start{pos_};
        if (!eof_() && src_[pos_] == '-') {
            ++pos_;
        }
        while (!eof_()) {
            char c{src_[pos_]};
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' ||
                c == '-') {
                ++pos_;
            } else {
                break;
            }
        }
        std::string_view tok{src_.substr(start, pos_ - start)};
        double d{0.0};
        auto res{std::from_chars(tok.data(), tok.data() + tok.size(), d)};
        if (res.ec != std::errc{}) {
            return std::nullopt;
        }
        auto v{std::make_unique<Value>()};
        v->kind = Kind::Number;
        v->number = d;
        return v;
    }

    std::optional<std::unique_ptr<Value>> parse_bool_() {
        if (src_.substr(pos_).starts_with("true")) {
            pos_ += 4;
            auto v{std::make_unique<Value>()};
            v->kind = Kind::Boolean;
            v->boolean = true;
            return v;
        }
        if (src_.substr(pos_).starts_with("false")) {
            pos_ += 5;
            auto v{std::make_unique<Value>()};
            v->kind = Kind::Boolean;
            v->boolean = false;
            return v;
        }
        return std::nullopt;
    }

    std::optional<std::unique_ptr<Value>> parse_null_() {
        if (src_.substr(pos_).starts_with("null")) {
            pos_ += 4;
            return std::make_unique<Value>();  // Kind::Null default
        }
        return std::nullopt;
    }
};

}  // namespace detail

// Parse a JSON document. Returns std::nullopt on malformed input (mirrors
// bun's parse-failure path where the manifest parse yields Ok(None)).
// The returned Document owns the tree; Value::str views borrow either `source`
// (which the caller must keep alive) or the Document's owned string pool.
export std::optional<Document> parse(std::string_view source) {
    Document doc{};
    detail::Parser parser{source, doc};
    auto root{parser.parse()};
    if (!root) {
        return std::nullopt;
    }
    doc.root = std::move(*root);
    return doc;
}

// ── printer ─────────────────────────────────────────────────────────────────
// The write-side of the package.json round-trip (`bun add` parses → edits →
// prints → the next run reparses). bun routes this through its JS printer in
// JSON mode: js_printer/lib.rs:8150 `print_json` instantiates
// `Printer<.., IS_JSON=true, ..>` with `PrintJsonOptions { indent }`, and the
// bytes come out of `print_object_json` / `print_array_json` / `quote_for_json`.

// ref: bun-ref/src/parsers/json.rs:208 (`indentation: guess_indentation(..)`).
export struct Indentation {
    char character{' '};
    unsigned count{2};
};

// ref: bun-ref/src/parsers/json.rs:257-300. Walks the source skipping over
// string and comment tokens, and reports the whitespace run that follows the
// first newline. Falls back to bun's `Indentation::default()` (2 spaces).
export Indentation guess_indentation(std::string_view s) {
    std::size_t i{0};
    while (i < s.size()) {
        if (s[i] == '"' || s[i] == '\'') {
            const char q{s[i]};
            ++i;
            while (i < s.size() && s[i] != q) {
                i += (s[i] == '\\') ? 2 : 1;
            }
            ++i;
            continue;
        }
        if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            i += 2;
            while (i < s.size() && s[i] != '\n') {
                ++i;
            }
            continue;
        }
        if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            const std::size_t close{s.find("*/", i + 2)};
            if (close == std::string_view::npos) {
                return Indentation{};
            }
            i = close + 2;
            continue;
        }
        if (s[i] == '\n') {
            ++i;
            while (i < s.size() && (s[i] == '\n' || s[i] == '\r')) {
                ++i;
            }
            if (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
                const char ch{s[i]};
                unsigned count{0};
                while (i < s.size() && s[i] == ch) {
                    ++i;
                    ++count;
                }
                return Indentation{ch, count};
            }
            continue;
        }
        ++i;
    }
    return Indentation{};
}

namespace detail {

// ref: bun-ref/src/js_printer/lib.rs:792-807 `can_print_without_escape`, with
// `ascii_only=false` (the mode `print_json` uses: lib.rs:8155 sets ASCII_ONLY=false).
// Only the ASCII half is reachable here — the caller feeds this one byte at a
// time and hands every byte >= 0x80 straight through as UTF-8.
constexpr bool can_print_without_escape_ascii(unsigned char c) {
    // FIRST_ASCII=0x20, LAST_ASCII=0x7E.
    return c >= 0x20 && c <= 0x7E && c != '\\' && c != '"' && c != '\'' && c != '`' && c != '$';
}

// ref: bun-ref/src/js_printer/lib.rs:1167 `quote_for_json` →
// `write_pre_quoted_string_inner::<Utf8>(text, .., quote_char='"',
// ascii_only=false, json=true)` (lib.rs:974-1166). The escape arms below are
// that function's `match c` transcribed for quote_char='"' + json=true:
// the '`'/'$'/'\'' arms all collapse to "print the raw byte", and the default
// arm's `c <= 0xFF && !json` \xHH branch is dead for json, so sub-0x20 bytes
// fall through to the \uXXXX branch.
void quote_for_json(std::string& out, std::string_view text) {
    out.push_back('"');
    for (std::size_t i{0}; i < text.size(); ++i) {
        const unsigned char c{static_cast<unsigned char>(text[i])};
        if (can_print_without_escape_ascii(c) || c >= 0x80) {
            // >= 0x80: already well-formed UTF-8 in the source; `ascii_only` is
            // false so bun re-emits the codepoint verbatim (lib.rs:1041-1057).
            out.push_back(text[i]);
            continue;
        }
        switch (c) {
            case 0x07: out += "\\x07"; break;  // lib.rs:1068 — emitted even for json
            case 0x08: out += "\\b"; break;
            case 0x0C: out += "\\f"; break;
            case 0x0A: out += "\\n"; break;
            case 0x0D: out += "\\r"; break;
            case 0x0B: out += "\\v"; break;   // lib.rs:1089 — likewise not strict JSON
            case 0x5C: out += "\\\\"; break;
            case 0x22: out += "\\\""; break;
            case 0x09: out += "\\t"; break;
            case 0x27: out.push_back('\''); break;  // quote_char != '\'' → raw
            case 0x60: out.push_back('`'); break;   // quote_char != '`'  → raw
            case 0x24: out.push_back('$'); break;   // quote_char != '`'  → raw
            default:
                // lib.rs:1155-1157: `c <= 0xFFFF` → \uXXXX (lower-case hex,
                // matching bun's `bmp_escape`).
                out += std::format("\\u{:04x}", static_cast<unsigned>(c));
                break;
        }
    }
    out.push_back('"');
}

// ref: bun-ref/src/js_printer/lib.rs:4711-4726 `print_number` reaches
// `print_json_value`'s Number arm. package.json numbers are integral in
// practice ("version" fields are strings), so print integers without a
// trailing ".0" and let std::format's shortest round-trip handle the rest.
void print_number(std::string& out, double n) {
    if (std::isfinite(n) && n == std::trunc(n) && std::abs(n) < 1e15) {
        out += std::format("{}", static_cast<std::int64_t>(n));
        return;
    }
    out += std::format("{}", n);
}

void print_value(std::string& out, const Value& v, const Indentation& indent, unsigned depth);

void print_indent(std::string& out, const Indentation& indent, unsigned depth) {
    out.append(static_cast<std::size_t>(indent.count) * depth, indent.character);
}

// ref: bun-ref/src/js_printer/lib.rs:4651-4691 `print_object_json`.
void print_object(std::string& out, const Value& v, const Indentation& indent, unsigned depth) {
    out.push_back('{');
    if (!v.members.empty()) {
        for (std::size_t i{0}; i < v.members.size(); ++i) {
            if (i > 0) {
                out.push_back(',');
            }
            if (v.singleLine) {
                // lib.rs:4666-4669: for json, the space is only printed between
                // members — never after the opening brace.
                if (i > 0) {
                    out.push_back(' ');
                }
            } else {
                out.push_back('\n');
                print_indent(out, indent, depth + 1);
            }
            quote_for_json(out, v.members[i].key);
            out.push_back(':');
            out.push_back(' ');
            print_value(out, *v.members[i].value, indent, depth + 1);
        }
        if (!v.singleLine) {
            out.push_back('\n');
            print_indent(out, indent, depth);
        }
    }
    out.push_back('}');
}

// ref: bun-ref/src/js_printer/lib.rs:4694+ `print_array_json` (same shape as
// the object printer, minus the keys).
void print_array(std::string& out, const Value& v, const Indentation& indent, unsigned depth) {
    out.push_back('[');
    if (!v.items.empty()) {
        for (std::size_t i{0}; i < v.items.size(); ++i) {
            if (i > 0) {
                out.push_back(',');
            }
            if (v.singleLine) {
                if (i > 0) {
                    out.push_back(' ');
                }
            } else {
                out.push_back('\n');
                print_indent(out, indent, depth + 1);
            }
            print_value(out, *v.items[i], indent, depth + 1);
        }
        if (!v.singleLine) {
            out.push_back('\n');
            print_indent(out, indent, depth);
        }
    }
    out.push_back(']');
}

// ref: bun-ref/src/js_printer/lib.rs:4728-4741 `print_json_value`.
void print_value(std::string& out, const Value& v, const Indentation& indent, unsigned depth) {
    switch (v.kind) {
        case Kind::Null: out += "null"; break;
        case Kind::Boolean: out += v.boolean ? "true" : "false"; break;
        case Kind::Number: print_number(out, v.number); break;
        case Kind::String: quote_for_json(out, v.str); break;
        case Kind::Object: print_object(out, v, indent, depth); break;
        case Kind::Array: print_array(out, v, indent, depth); break;
    }
}

}  // namespace detail

// Print a parse tree back to JSON text. No trailing newline — bun writes
// exactly the printer's bytes (updatePackageJSONAndInstall.rs:374-411 hands
// `print_json`'s buffer straight to the package.json write).
export std::string stringify(const Value& root, const Indentation& indent = {}) {
    std::string out;
    detail::print_value(out, root, indent, 0);
    return out;
}

}  // namespace mbun::install::npm::json
