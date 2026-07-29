// toml.cppm — mbun.toml: TOML parser (Bun.TOML.parse equivalent).
//
// Behavior is pinned by bun's original TOML test suite (see tests/test_toml.cpp).
// This is a re-implementation, not a port of bun's zig lexer.
export module mbun.toml;

import std;

namespace mbun::toml {

export enum class Type { Table, Array, String, Integer, Float, Boolean, Datetime };

export class Value {
public:
    using Array = std::vector<Value>;

    // An ordered table (JS objects preserve insertion order). Lookup is linear;
    // configs are tiny and this keeps the value tree allocation-light.
    struct Table {
        std::vector<std::pair<std::string, Value>> entries;
        bool explicitlyDefined{false};  // defined via a [header] or {inline}
        bool viaInline{false};          // sealed inline table (no later extension)

        Value* find(std::string_view key) {
            for (auto& [k, v] : entries) {
                if (k == key) {
                    return &v;
                }
            }
            return nullptr;
        }
        const Value* find(std::string_view key) const {
            for (const auto& [k, v] : entries) {
                if (k == key) {
                    return &v;
                }
            }
            return nullptr;
        }
    };

    Value() : type_{Type::Table}, data_{Table{}} {}

    static Value make_table() {
        Value v;
        v.type_ = Type::Table;
        v.data_ = Table{};
        return v;
    }
    static Value make_array() {
        Value v;
        v.type_ = Type::Array;
        v.data_ = Array{};
        return v;
    }
    static Value string(std::string s) {
        Value v;
        v.type_ = Type::String;
        v.data_ = std::move(s);
        return v;
    }
    static Value datetime(std::string s) {
        Value v;
        v.type_ = Type::Datetime;
        v.data_ = std::move(s);
        return v;
    }
    static Value integer(std::int64_t n) {
        Value v;
        v.type_ = Type::Integer;
        v.data_ = n;
        return v;
    }
    static Value floating(double d) {
        Value v;
        v.type_ = Type::Float;
        v.data_ = d;
        return v;
    }
    static Value boolean(bool b) {
        Value v;
        v.type_ = Type::Boolean;
        v.data_ = b;
        return v;
    }

    Type type() const {
        return type_;
    }
    bool is_table() const {
        return type_ == Type::Table;
    }
    bool is_array() const {
        return type_ == Type::Array;
    }
    bool is_string() const {
        return type_ == Type::String;
    }
    bool is_integer() const {
        return type_ == Type::Integer;
    }
    bool is_float() const {
        return type_ == Type::Float;
    }
    bool is_boolean() const {
        return type_ == Type::Boolean;
    }
    bool is_datetime() const {
        return type_ == Type::Datetime;
    }

    Table& table() {
        return std::get<Table>(data_);
    }
    const Table& table() const {
        return std::get<Table>(data_);
    }
    Array& array() {
        return std::get<Array>(data_);
    }
    const Array& array() const {
        return std::get<Array>(data_);
    }

    const std::string& as_string() const {
        return std::get<std::string>(data_);
    }
    std::int64_t as_integer() const {
        return std::get<std::int64_t>(data_);
    }
    double as_float() const {
        return std::get<double>(data_);
    }
    bool as_bool() const {
        return std::get<bool>(data_);
    }

    // Table child lookup by exact key; nullptr if this is not a table or the key
    // is absent (mirrors a missing JS object property).
    const Value* get(std::string_view key) const {
        if (type_ != Type::Table) {
            return nullptr;
        }
        return std::get<Table>(data_).find(key);
    }

    const Value& at(std::size_t i) const {
        return std::get<Array>(data_)[i];
    }
    std::size_t size() const {
        if (type_ == Type::Array) {
            return std::get<Array>(data_).size();
        }
        if (type_ == Type::Table) {
            return std::get<Table>(data_).entries.size();
        }
        return 0;
    }

private:
    Type type_;
    std::variant<Table, Array, std::string, std::int64_t, double, bool> data_;
};

export struct ParseError {
    std::string message;
    std::size_t offset{0};  // byte offset into the source
};

namespace {

// Internal error-propagation carrier. Using an exception keeps the recursive
// descent readable and, crucially, the MAX_DEPTH guard throws *before* the
// stack can overflow, so pathological nesting unwinds instead of crashing.
struct ParseException {
    ParseError err;
};

[[noreturn]] void fail(std::size_t off, std::string msg) {
    throw ParseException{ParseError{std::move(msg), off}};
}

void append_utf8(std::string& out, char32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int hex_val(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return c - 'A' + 10;
}

bool is_bare_key_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-';
}

bool is_value_delim(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',' || c == ']' || c == '}' ||
           c == '#';
}

constexpr std::int64_t MAX_SAFE_INTEGER{(std::int64_t{1} << 53) - 1};

class Parser {
public:
    explicit Parser(std::string_view s) : s_{s} {}

    Value parse_document() {
        Value root{Value::make_table()};
        Value::Table* cur{&root.table()};
        skip_blank();
        while (i_ < s_.size()) {
            char c{s_[i_]};
            if (c == '[') {
                cur = parse_header_(root);
            } else {
                parse_keyval_(*cur);
            }
            end_of_line_();
            skip_blank();
        }
        return root;
    }

private:
    static constexpr int MAX_DEPTH{1000};

    std::string_view s_;
    std::size_t i_{0};

    char peek_() const {
        return i_ < s_.size() ? s_[i_] : '\0';
    }
    char peek_at_(std::size_t off) const {
        return i_ + off < s_.size() ? s_[i_ + off] : '\0';
    }

    void skip_inline_ws_() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t')) {
            ++i_;
        }
    }

    // Whitespace, newlines and full-line comments between statements.
    void skip_blank() {
        while (i_ < s_.size()) {
            char c{s_[i_]};
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++i_;
            } else if (c == '#') {
                while (i_ < s_.size() && s_[i_] != '\n') {
                    ++i_;
                }
            } else {
                break;
            }
        }
    }

    // Consume an optional inline comment and the statement-terminating newline.
    void end_of_line_() {
        skip_inline_ws_();
        if (i_ < s_.size() && s_[i_] == '#') {
            while (i_ < s_.size() && s_[i_] != '\n') {
                ++i_;
            }
        }
        if (i_ >= s_.size()) {
            return;
        }
        char c{s_[i_]};
        if (c == '\n') {
            ++i_;
        } else if (c == '\r') {
            ++i_;
            if (i_ < s_.size() && s_[i_] == '\n') {
                ++i_;
            }
        } else {
            fail(i_, "Syntax Error: expected end of line");
        }
    }

    // --- keys --------------------------------------------------------------

    std::string parse_key_segment_() {
        skip_inline_ws_();
        if (i_ >= s_.size()) {
            fail(i_, "Syntax Error: expected key");
        }
        char c{s_[i_]};
        if (c == '"') {
            std::string out;
            parse_basic_string_(out);
            return out;
        }
        if (c == '\'') {
            std::string out;
            parse_literal_string_(out);
            return out;
        }
        std::size_t start{i_};
        while (i_ < s_.size() && is_bare_key_char(s_[i_])) {
            ++i_;
        }
        if (i_ == start) {
            fail(i_, "Syntax Error: invalid key");
        }
        return std::string{s_.substr(start, i_ - start)};
    }

    std::vector<std::string> parse_key_path_() {
        std::vector<std::string> segs;
        segs.push_back(parse_key_segment_());
        while (true) {
            skip_inline_ws_();
            if (i_ < s_.size() && s_[i_] == '.') {
                ++i_;
                segs.push_back(parse_key_segment_());
            } else {
                break;
            }
        }
        return segs;
    }

    // Navigate/create nested tables through `segs`, descending into the last
    // element when a segment resolves to an array of tables.
    Value::Table* dig_(Value::Table* t, std::span<const std::string> segs) {
        for (const auto& seg : segs) {
            Value* child{t->find(seg)};
            if (child == nullptr) {
                t->entries.emplace_back(seg, Value::make_table());
                child = &t->entries.back().second;
            }
            if (child->is_table()) {
                if (child->table().viaInline) {
                    fail(i_, "Syntax Error: cannot extend inline table");
                }
                t = &child->table();
            } else if (child->is_array()) {
                auto& arr{child->array()};
                if (arr.empty() || !arr.back().is_table()) {
                    fail(i_, "Syntax Error: key is not a table");
                }
                t = &arr.back().table();
            } else {
                fail(i_, "Syntax Error: key already has a value");
            }
        }
        return t;
    }

    // --- headers -----------------------------------------------------------

    Value::Table* parse_header_(Value& root) {
        bool arrayHeader{peek_at_(1) == '['};
        i_ += arrayHeader ? 2 : 1;
        std::vector<std::string> segs{parse_key_path_()};
        skip_inline_ws_();
        if (i_ >= s_.size() || s_[i_] != ']') {
            fail(i_, "Syntax Error: expected ']'");
        }
        ++i_;
        if (arrayHeader) {
            if (i_ >= s_.size() || s_[i_] != ']') {
                fail(i_, "Syntax Error: expected ']]'");
            }
            ++i_;
        }
        if (segs.empty()) {
            fail(i_, "Syntax Error: empty table name");
        }

        if (!arrayHeader) {
            Value::Table* t{dig_(&root.table(), std::span{segs})};
            t->explicitlyDefined = true;
            return t;
        }
        // Array-of-tables: dig to the parent, then append a fresh table element.
        Value::Table* parent{dig_(&root.table(), std::span{segs}.first(segs.size() - 1))};
        const std::string& key{segs.back()};
        Value* arrv{parent->find(key)};
        if (arrv == nullptr) {
            parent->entries.emplace_back(key, Value::make_array());
            arrv = &parent->entries.back().second;
        }
        if (!arrv->is_array()) {
            fail(i_, "Syntax Error: key is not an array of tables");
        }
        arrv->array().push_back(Value::make_table());
        return &arrv->array().back().table();
    }

    // --- key = value -------------------------------------------------------

    void parse_keyval_(Value::Table& into) {
        std::vector<std::string> segs{parse_key_path_()};
        skip_inline_ws_();
        if (i_ >= s_.size() || s_[i_] != '=') {
            fail(i_, "Syntax Error: expected '='");
        }
        ++i_;
        skip_inline_ws_();
        Value v{parse_value_(0)};
        Value::Table* t{dig_(&into, std::span{segs}.first(segs.size() - 1))};
        const std::string& key{segs.back()};
        if (t->find(key) != nullptr) {
            fail(i_, "Syntax Error: duplicate key");
        }
        t->entries.emplace_back(key, std::move(v));
    }

    // --- values ------------------------------------------------------------

    Value parse_value_(int depth) {
        skip_inline_ws_();
        if (i_ >= s_.size()) {
            fail(i_, "Syntax Error: expected value");
        }
        char c{s_[i_]};
        if (c == '"') {
            std::string out;
            parse_basic_string_(out);
            return Value::string(std::move(out));
        }
        if (c == '\'') {
            std::string out;
            parse_literal_string_(out);
            return Value::string(std::move(out));
        }
        if (c == '[') {
            return parse_array_(depth);
        }
        if (c == '{') {
            return parse_inline_table_(depth);
        }
        return parse_scalar_();
    }

    Value parse_array_(int depth) {
        if (depth >= MAX_DEPTH) {
            fail(i_, "RangeError: TOML nesting too deep");
        }
        ++i_;  // consume '['
        Value arr{Value::make_array()};
        while (true) {
            skip_blank();
            if (i_ >= s_.size()) {
                fail(i_, "Syntax Error: unterminated array");
            }
            if (s_[i_] == ']') {
                ++i_;
                break;
            }
            arr.array().push_back(parse_value_(depth + 1));
            skip_blank();
            if (i_ >= s_.size()) {
                fail(i_, "Syntax Error: unterminated array");
            }
            if (s_[i_] == ',') {
                ++i_;
            } else if (s_[i_] == ']') {
                ++i_;
                break;
            } else {
                fail(i_, "Syntax Error: expected ',' or ']' in array");
            }
        }
        return arr;
    }

    Value parse_inline_table_(int depth) {
        if (depth >= MAX_DEPTH) {
            fail(i_, "RangeError: TOML nesting too deep");
        }
        ++i_;  // consume '{'
        Value tbl{Value::make_table()};
        tbl.table().viaInline = true;
        tbl.table().explicitlyDefined = true;
        skip_inline_ws_();
        if (i_ < s_.size() && s_[i_] == '}') {
            ++i_;
            return tbl;
        }
        while (true) {
            std::vector<std::string> segs{parse_key_path_()};
            skip_inline_ws_();
            if (i_ >= s_.size() || s_[i_] != '=') {
                fail(i_, "Syntax Error: expected '=' in inline table");
            }
            ++i_;
            Value v{parse_value_(depth + 1)};
            // Inline-table sub-tables created via dotted keys must stay mutable
            // for sibling keys, so clear the seal on the transient parents.
            Value::Table* t{&tbl.table()};
            {
                std::span<const std::string> parents{std::span{segs}.first(segs.size() - 1)};
                for (const auto& seg : parents) {
                    Value* child{t->find(seg)};
                    if (child == nullptr) {
                        t->entries.emplace_back(seg, Value::make_table());
                        child = &t->entries.back().second;
                    }
                    if (!child->is_table()) {
                        fail(i_, "Syntax Error: key already has a value");
                    }
                    t = &child->table();
                }
            }
            const std::string& key{segs.back()};
            if (t->find(key) != nullptr) {
                fail(i_, "Syntax Error: duplicate key in inline table");
            }
            t->entries.emplace_back(key, std::move(v));
            skip_inline_ws_();
            if (i_ >= s_.size()) {
                fail(i_, "Syntax Error: unterminated inline table");
            }
            if (s_[i_] == ',') {
                ++i_;
                skip_inline_ws_();
            } else if (s_[i_] == '}') {
                ++i_;
                break;
            } else {
                fail(i_, "Syntax Error: expected ',' or '}' in inline table");
            }
        }
        return tbl;
    }

    // Bare (unquoted) scalar: bool, integer, float, inf/nan, or datetime.
    Value parse_scalar_() {
        std::size_t start{i_};
        while (i_ < s_.size() && !is_value_delim(s_[i_])) {
            ++i_;
        }
        std::string_view tok{s_.substr(start, i_ - start)};
        if (tok.empty()) {
            fail(start, "Syntax Error: expected value");
        }
        if (tok == "true") {
            return Value::boolean(true);
        }
        if (tok == "false") {
            return Value::boolean(false);
        }
        if (tok == "inf" || tok == "+inf") {
            return Value::floating(std::numeric_limits<double>::infinity());
        }
        if (tok == "-inf") {
            return Value::floating(-std::numeric_limits<double>::infinity());
        }
        if (tok == "nan" || tok == "+nan" || tok == "-nan") {
            return Value::floating(std::numeric_limits<double>::quiet_NaN());
        }
        if (is_datetime_(tok)) {
            return Value::datetime(std::string{tok});
        }
        return parse_number_(tok, start);
    }

    static bool is_datetime_(std::string_view tok) {
        if (tok.find(':') != std::string_view::npos) {
            return true;  // local time or a date-time with a time component
        }
        // date form YYYY-MM-DD
        if (tok.size() >= 10 && tok[4] == '-' && tok[7] == '-') {
            for (std::size_t k{0}; k < 10; ++k) {
                if (k == 4 || k == 7) {
                    continue;
                }
                if (tok[k] < '0' || tok[k] > '9') {
                    return false;
                }
            }
            return true;
        }
        return false;
    }

    Value parse_number_(std::string_view tok, std::size_t off) {
        // Radix-prefixed integers: 0x / 0o / 0b.
        if (tok.size() > 2 && tok[0] == '0' &&
            (tok[1] == 'x' || tok[1] == 'o' || tok[1] == 'b')) {
            int base{tok[1] == 'x' ? 16 : (tok[1] == 'o' ? 8 : 2)};
            std::string digits;
            for (char c : tok.substr(2)) {
                if (c != '_') {
                    digits.push_back(c);
                }
            }
            std::int64_t value{0};
            auto res{std::from_chars(digits.data(), digits.data() + digits.size(), value, base)};
            if (res.ec == std::errc::result_out_of_range) {
                fail(off, "Integer is outside the 64-bit signed range");
            }
            if (res.ec != std::errc{} || res.ptr != digits.data() + digits.size()) {
                fail(off, "Syntax Error: invalid integer");
            }
            if (value > MAX_SAFE_INTEGER) {
                fail(off, "Integer cannot be losslessly represented as a JavaScript number; it must be within +/-(2^53 - 1)");
            }
            return Value::integer(value);
        }

        std::string cleaned;
        cleaned.reserve(tok.size());
        for (char c : tok) {
            if (c != '_') {
                cleaned.push_back(c);
            }
        }
        bool isFloat{cleaned.find('.') != std::string::npos ||
                     cleaned.find('e') != std::string::npos ||
                     cleaned.find('E') != std::string::npos};
        if (isFloat) {
            double value{0};
            auto res{std::from_chars(cleaned.data(), cleaned.data() + cleaned.size(), value)};
            if (res.ec != std::errc{} || res.ptr != cleaned.data() + cleaned.size()) {
                fail(off, "Syntax Error: invalid float");
            }
            return Value::floating(value);
        }
        std::int64_t value{0};
        const char* begin{cleaned.data()};
        if (!cleaned.empty() && cleaned[0] == '+') {
            ++begin;  // std::from_chars rejects a leading '+'
        }
        auto res{std::from_chars(begin, cleaned.data() + cleaned.size(), value)};
        if (res.ec == std::errc::result_out_of_range) {
            fail(off, "Integer is outside the 64-bit signed range");
        }
        if (res.ec != std::errc{} || res.ptr != cleaned.data() + cleaned.size()) {
            fail(off, "Syntax Error: invalid integer");
        }
        if (value > MAX_SAFE_INTEGER || value < -MAX_SAFE_INTEGER) {
            fail(off, "Integer cannot be losslessly represented as a JavaScript number; it must be within +/-(2^53 - 1)");
        }
        return Value::integer(value);
    }

    // --- strings -----------------------------------------------------------

    void parse_basic_string_(std::string& out) {
        // Multiline?  """ ... """
        if (peek_at_(1) == '"' && peek_at_(2) == '"') {
            i_ += 3;
            parse_multiline_basic_(out);
            return;
        }
        ++i_;  // opening quote
        while (true) {
            if (i_ >= s_.size()) {
                fail(i_, "Syntax Error: unterminated string");
            }
            char c{s_[i_]};
            if (c == '"') {
                ++i_;
                return;
            }
            if (c == '\n' || c == '\r') {
                fail(i_, "Syntax Error: newline in single-line string");
            }
            if (c == '\\') {
                decode_escape_(out, false);
            } else {
                out.push_back(c);
                ++i_;
            }
        }
    }

    void parse_multiline_basic_(std::string& out) {
        // A newline immediately after the opening delimiter is trimmed.
        if (i_ < s_.size() && s_[i_] == '\r' && peek_at_(1) == '\n') {
            i_ += 2;
        } else if (i_ < s_.size() && s_[i_] == '\n') {
            ++i_;
        }
        while (true) {
            if (i_ >= s_.size()) {
                fail(i_, "Syntax Error: unterminated multiline string");
            }
            char c{s_[i_]};
            if (c == '"' && peek_at_(1) == '"' && peek_at_(2) == '"') {
                i_ += 3;
                return;
            }
            if (c == '\\') {
                char nc{peek_at_(1)};
                if (nc == '\n' || nc == '\r' || nc == ' ' || nc == '\t') {
                    // Line-continuation: drop the backslash and all following
                    // whitespace/newlines up to the next non-whitespace char.
                    ++i_;  // backslash
                    while (i_ < s_.size() &&
                           (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) {
                        ++i_;
                    }
                    continue;
                }
                decode_escape_(out, true);
                continue;
            }
            if (c == '\r' && peek_at_(1) == '\n') {
                out.push_back('\n');
                i_ += 2;
                continue;
            }
            if (c == '\r') {
                out.push_back('\n');
                ++i_;
                continue;
            }
            out.push_back(c);
            ++i_;
        }
    }

    void parse_literal_string_(std::string& out) {
        // Multiline literal?  ''' ... '''
        if (peek_at_(1) == '\'' && peek_at_(2) == '\'') {
            i_ += 3;
            if (i_ < s_.size() && s_[i_] == '\r' && peek_at_(1) == '\n') {
                i_ += 2;
            } else if (i_ < s_.size() && s_[i_] == '\n') {
                ++i_;
            }
            while (true) {
                if (i_ >= s_.size()) {
                    fail(i_, "Syntax Error: unterminated multiline literal string");
                }
                if (s_[i_] == '\'' && peek_at_(1) == '\'' && peek_at_(2) == '\'') {
                    i_ += 3;
                    return;
                }
                if (s_[i_] == '\r' && peek_at_(1) == '\n') {
                    out.push_back('\n');
                    i_ += 2;
                    continue;
                }
                out.push_back(s_[i_]);
                ++i_;
            }
        }
        ++i_;  // opening quote
        while (true) {
            if (i_ >= s_.size()) {
                fail(i_, "Syntax Error: unterminated literal string");
            }
            char c{s_[i_]};
            if (c == '\'') {
                ++i_;
                return;
            }
            if (c == '\n' || c == '\r') {
                fail(i_, "Syntax Error: newline in single-line literal string");
            }
            out.push_back(c);
            ++i_;
        }
    }

    // i_ points at the backslash. Decodes one escape into `out`.
    void decode_escape_(std::string& out, bool multiline) {
        (void)multiline;
        ++i_;  // backslash
        if (i_ >= s_.size()) {
            fail(i_, "Syntax Error: unterminated escape");
        }
        char e{s_[i_]};
        ++i_;
        switch (e) {
        case 'b':
            out.push_back('\b');
            return;
        case 't':
            out.push_back('\t');
            return;
        case 'n':
            out.push_back('\n');
            return;
        case 'f':
            out.push_back('\f');
            return;
        case 'r':
            out.push_back('\r');
            return;
        case '"':
            out.push_back('"');
            return;
        case '\\':
            out.push_back('\\');
            return;
        case '/':
            out.push_back('/');
            return;
        case 'x':
            append_utf8(out, read_fixed_hex_(2));
            return;
        case 'u':
            if (i_ < s_.size() && s_[i_] == '{') {
                ++i_;
                append_utf8(out, read_variable_hex_());
                return;
            }
            append_utf8(out, read_fixed_hex_(4));
            return;
        case 'U':
            append_utf8(out, read_fixed_hex_(8));
            return;
        default:
            fail(i_, "Syntax Error: invalid escape sequence");
        }
    }

    char32_t read_fixed_hex_(int n) {
        std::uint32_t value{0};
        for (int k{0}; k < n; ++k) {
            if (i_ >= s_.size() || !is_hex(s_[i_])) {
                fail(i_, "Syntax Error: invalid hex escape");
            }
            value = value * 16 + static_cast<std::uint32_t>(hex_val(s_[i_]));
            ++i_;
        }
        if (value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
            fail(i_, "Unicode escape sequence is out of range");
        }
        return static_cast<char32_t>(value);
    }

    // Variable-length \u{...}; i_ points just past '{'. Finds the closing brace
    // before judging range, so a missing brace is a Syntax Error and an in-range
    // check never trips on a truncated value (#30825).
    char32_t read_variable_hex_() {
        std::uint64_t value{0};
        bool overflow{false};
        bool any{false};
        while (i_ < s_.size() && is_hex(s_[i_])) {
            any = true;
            value = value * 16 + static_cast<std::uint64_t>(hex_val(s_[i_]));
            if (value > 0x10FFFF) {
                overflow = true;
                value = 0x110000;  // saturate to keep scanning without overflowing
            }
            ++i_;
        }
        if (i_ >= s_.size() || s_[i_] != '}') {
            fail(i_, "Syntax Error: expected '}' in unicode escape");
        }
        ++i_;  // closing brace
        if (!any) {
            fail(i_, "Syntax Error: empty unicode escape");
        }
        if (overflow || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
            fail(i_, "Unicode escape sequence is out of range");
        }
        return static_cast<char32_t>(value);
    }
};

}  // namespace

export std::expected<Value, ParseError> parse(std::string_view src) {
    try {
        Parser p{src};
        return p.parse_document();
    } catch (const ParseException& ex) {
        return std::unexpected(ex.err);
    }
}

}  // namespace mbun::toml
