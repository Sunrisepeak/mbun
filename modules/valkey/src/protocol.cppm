export module mbun.valkey.protocol;

import std;

export namespace mbun::valkey {

enum class RedisError {
    authentication_failed,
    connection_closed,
    invalid_argument,
    invalid_array,
    invalid_attribute,
    invalid_big_number,
    invalid_blob_error,
    invalid_boolean,
    invalid_bulk_string,
    invalid_command,
    invalid_double,
    invalid_error_string,
    invalid_integer,
    invalid_map,
    invalid_null,
    invalid_push,
    invalid_response,
    invalid_response_type,
    invalid_set,
    invalid_simple_string,
    invalid_verbatim_string,
    unsupported_protocol,
    connection_timeout,
    idle_timeout,
    nesting_depth_exceeded,
    line_too_long,
};

enum class RESPType : unsigned char {
    simple_string = '+', error = '-', integer = ':', bulk_string = '$', array = '*',
    null = '_', double_value = ',', boolean = '#', blob_error = '!',
    verbatim_string = '=', map = '%', set = '~', attribute = '|', push = '>',
    big_number = '(',
};

[[nodiscard]] constexpr auto resp_type_from_byte(unsigned char byte) noexcept
    -> std::optional<RESPType> {
    switch (static_cast<char>(byte)) {
    case '+': return RESPType::simple_string;
    case '-': return RESPType::error;
    case ':': return RESPType::integer;
    case '$': return RESPType::bulk_string;
    case '*': return RESPType::array;
    case '_': return RESPType::null;
    case ',': return RESPType::double_value;
    case '#': return RESPType::boolean;
    case '!': return RESPType::blob_error;
    case '=': return RESPType::verbatim_string;
    case '%': return RESPType::map;
    case '~': return RESPType::set;
    case '|': return RESPType::attribute;
    case '>': return RESPType::push;
    case '(': return RESPType::big_number;
    default: return std::nullopt;
    }
}

class RESPValue {
public:
    using Bytes = std::vector<unsigned char>;
    using Child = std::shared_ptr<RESPValue>;
    using Children = std::vector<Child>;
    using MapEntry = std::pair<Child, Child>;

    RESPType type { RESPType::null };
    Bytes bytes {};
    std::int64_t integer { 0 };
    double floating { 0.0 };
    bool boolean { false };
    std::string format {};
    Children children {};
    std::vector<MapEntry> entries {};
    Child value {};

    [[nodiscard]] static auto text(RESPType valueType, std::string_view textValue) -> RESPValue {
        RESPValue result;
        result.type = valueType;
        result.bytes.assign(textValue.begin(), textValue.end());
        return result;
    }
    [[nodiscard]] static auto null() -> RESPValue { return {}; }
    [[nodiscard]] auto display() const -> std::string;

    // Typed accessors. Mirror bun's RESPValue → JS coercions:
    // src/runtime/valkey_jsc/protocol_jsc.rs (scalar mapping) and
    // valkey.rs `return_as_bool` (integer > 0 → Boolean).
    [[nodiscard]] auto is_null() const noexcept -> bool { return type == RESPType::null; }
    [[nodiscard]] auto is_error() const noexcept -> bool {
        return type == RESPType::error || type == RESPType::blob_error;
    }
    [[nodiscard]] auto as_string() const -> std::optional<std::string>;
    [[nodiscard]] auto as_integer() const -> std::optional<std::int64_t>;
    [[nodiscard]] auto as_double() const -> std::optional<double>;
    [[nodiscard]] auto as_boolean() const -> std::optional<bool>;
    [[nodiscard]] auto error_message() const -> std::optional<std::string>;
    auto apply_return_as_bool() -> void;
};

class ValkeyReader {
public:
    static constexpr std::size_t max_nesting_depth = 128;
    static constexpr std::size_t max_line_length = 512 * 1024;
    static constexpr std::int64_t max_bulk_length = 512 * 1024 * 1024;

    explicit ValkeyReader(std::span<const unsigned char> buffer) : buffer_(buffer) {}

    [[nodiscard]] auto position() const noexcept -> std::size_t { return position_; }
    [[nodiscard]] auto read_value() -> std::expected<RESPValue, RedisError>;

private:
    [[nodiscard]] auto read_byte() -> std::expected<unsigned char, RedisError>;
    [[nodiscard]] auto read_line() -> std::expected<std::span<const unsigned char>, RedisError>;
    [[nodiscard]] auto read_integer() -> std::expected<std::int64_t, RedisError>;
    [[nodiscard]] auto read_value_at(std::size_t depth) -> std::expected<RESPValue, RedisError>;

    std::span<const unsigned char> buffer_;
    std::size_t position_ { 0 };
};

inline auto ValkeyReader::read_byte() -> std::expected<unsigned char, RedisError> {
    if (position_ >= buffer_.size()) return std::unexpected(RedisError::invalid_response);
    return buffer_[position_++];
}

inline auto ValkeyReader::read_line() -> std::expected<std::span<const unsigned char>, RedisError> {
    const auto remaining = buffer_.subspan(position_);
    const auto limit = std::min(remaining.size(), max_line_length + 1);
    for (std::size_t index = 0; index + 1 < limit; ++index) {
        if (remaining[index] == '\r' && remaining[index + 1] == '\n') {
            const auto result = remaining.first(index);
            position_ += index + 2;
            return result;
        }
    }
    return std::unexpected(remaining.size() > max_line_length + 1
        ? RedisError::line_too_long : RedisError::invalid_response);
}

inline auto ValkeyReader::read_integer() -> std::expected<std::int64_t, RedisError> {
    const auto line = read_line();
    if (!line) return std::unexpected(RedisError::invalid_integer);
    std::string value(line->begin(), line->end());
    std::int64_t parsed {};
    auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc {} || end != value.data() + value.size()) {
        return std::unexpected(RedisError::invalid_integer);
    }
    return parsed;
}

inline auto ValkeyReader::read_value() -> std::expected<RESPValue, RedisError> {
    return read_value_at(0);
}

inline auto ValkeyReader::read_value_at(std::size_t depth) -> std::expected<RESPValue, RedisError> {
    const auto tag = read_byte();
    if (!tag) return std::unexpected(tag.error());
    const auto type = resp_type_from_byte(*tag);
    if (!type) return std::unexpected(RedisError::invalid_response_type);

    if (*type == RESPType::simple_string || *type == RESPType::error || *type == RESPType::big_number) {
        const auto line = read_line();
        if (!line) return std::unexpected(line.error());
        return RESPValue::text(*type, std::string_view(reinterpret_cast<const char*>(line->data()), line->size()));
    }
    if (*type == RESPType::integer) {
        const auto integer = read_integer();
        if (!integer) return std::unexpected(integer.error());
        RESPValue result; result.type = *type; result.integer = *integer; return result;
    }
    if (*type == RESPType::null) {
        const auto line = read_line();
        if (!line || !line->empty()) return std::unexpected(RedisError::invalid_null);
        return RESPValue::null();
    }
    if (*type == RESPType::boolean) {
        const auto line = read_line();
        if (!line || line->size() != 1 || ((*line)[0] != 't' && (*line)[0] != 'f')) {
            return std::unexpected(RedisError::invalid_boolean);
        }
        RESPValue result; result.type = *type; result.boolean = (*line)[0] == 't'; return result;
    }
    if (*type == RESPType::double_value) {
        const auto line = read_line();
        if (!line) return std::unexpected(RedisError::invalid_double);
        std::string value(line->begin(), line->end());
        double parsed {};
        auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (error != std::errc {} || end != value.data() + value.size()) {
            if (value == "inf") parsed = std::numeric_limits<double>::infinity();
            else if (value == "-inf") parsed = -std::numeric_limits<double>::infinity();
            else if (value == "nan") parsed = std::numeric_limits<double>::quiet_NaN();
            else return std::unexpected(RedisError::invalid_double);
        }
        RESPValue result; result.type = *type; result.floating = parsed; return result;
    }
    if (depth >= max_nesting_depth) return std::unexpected(RedisError::nesting_depth_exceeded);

    const auto length = read_integer();
    if (!length) return std::unexpected(length.error());
    if (*type == RESPType::bulk_string || *type == RESPType::blob_error || *type == RESPType::verbatim_string) {
        if (*length < 0 || *length > max_bulk_length) {
            if (*type == RESPType::bulk_string && *length < 0) { RESPValue result; result.type = *type; return result; }
            return std::unexpected(*type == RESPType::blob_error ? RedisError::invalid_blob_error : RedisError::invalid_bulk_string);
        }
        const auto size = static_cast<std::size_t>(*length);
        if (size > buffer_.size() - position_) return std::unexpected(RedisError::invalid_response);
        const auto payload = buffer_.subspan(position_, size); position_ += size;
        const auto crlf = read_line();
        if (!crlf || !crlf->empty()) return std::unexpected(RedisError::invalid_bulk_string);
        RESPValue result; result.type = *type; result.bytes.assign(payload.begin(), payload.end());
        if (*type == RESPType::verbatim_string && result.bytes.size() >= 4 && result.bytes[3] == ':') {
            result.format.assign(result.bytes.begin(), result.bytes.begin() + 3);
            result.bytes.erase(result.bytes.begin(), result.bytes.begin() + 4);
        }
        return result;
    }
    // Per-type non-positive length rules, matching bun's read_value_with_depth:
    // `*-1` is a RESP2 null array (→ empty array); set/map/attribute reject
    // negative lengths; push rejects lengths <= 0.
    RESPValue result; result.type = *type;
    if (*type == RESPType::array) {
        if (*length < 0) return result;
    } else if (*type == RESPType::set) {
        if (*length < 0) return std::unexpected(RedisError::invalid_set);
    } else if (*type == RESPType::map) {
        if (*length < 0) return std::unexpected(RedisError::invalid_map);
    } else if (*type == RESPType::attribute) {
        if (*length < 0) return std::unexpected(RedisError::invalid_attribute);
    } else if (*type == RESPType::push) {
        if (*length <= 0) return std::unexpected(RedisError::invalid_push);
    }
    if (*type == RESPType::map || *type == RESPType::attribute) {
        result.entries.reserve(static_cast<std::size_t>(*length));
        for (std::int64_t index = 0; index < *length; ++index) {
            auto key = read_value_at(depth + 1); if (!key) return std::unexpected(key.error());
            auto value = read_value_at(depth + 1); if (!value) return std::unexpected(value.error());
            result.entries.emplace_back(std::make_shared<RESPValue>(std::move(*key)), std::make_shared<RESPValue>(std::move(*value)));
        }
        if (*type == RESPType::attribute) { auto value = read_value_at(depth + 1); if (!value) return std::unexpected(value.error()); result.value = std::make_shared<RESPValue>(std::move(*value)); }
    } else {
        result.children.reserve(static_cast<std::size_t>(*length));
        for (std::int64_t index = 0; index < *length; ++index) {
            auto child = read_value_at(depth + 1); if (!child) return std::unexpected(child.error());
            result.children.emplace_back(std::make_shared<RESPValue>(std::move(*child)));
        }
    }
    return result;
}

inline auto RESPValue::display() const -> std::string {
    if (type == RESPType::null) return "(nil)";
    if (type == RESPType::integer) return std::to_string(integer);
    if (type == RESPType::double_value) return std::to_string(floating);
    if (type == RESPType::boolean) return boolean ? "true" : "false";
    return std::string(bytes.begin(), bytes.end());
}

inline auto RESPValue::as_string() const -> std::optional<std::string> {
    switch (type) {
    case RESPType::simple_string:
    case RESPType::error:
    case RESPType::bulk_string:
    case RESPType::blob_error:
    case RESPType::verbatim_string:
    case RESPType::big_number:
        return std::string(bytes.begin(), bytes.end());
    default:
        return std::nullopt;
    }
}

inline auto RESPValue::as_integer() const -> std::optional<std::int64_t> {
    if (type == RESPType::integer) return integer;
    return std::nullopt;
}

inline auto RESPValue::as_double() const -> std::optional<double> {
    if (type == RESPType::double_value) return floating;
    if (type == RESPType::integer) return static_cast<double>(integer);
    return std::nullopt;
}

inline auto RESPValue::as_boolean() const -> std::optional<bool> {
    if (type == RESPType::boolean) return boolean;
    if (type == RESPType::integer) return integer > 0;  // RETURN_AS_BOOL semantics
    return std::nullopt;
}

inline auto RESPValue::error_message() const -> std::optional<std::string> {
    if (type == RESPType::error || type == RESPType::blob_error) {
        return std::string(bytes.begin(), bytes.end());
    }
    return std::nullopt;
}

inline auto RESPValue::apply_return_as_bool() -> void {
    if (type == RESPType::integer) { boolean = integer > 0; type = RESPType::boolean; }
}

// Outcome of an incremental ReplyScanner::scan pass. Mirrors bun's ScanResult
// (src/valkey/valkey_protocol.rs).
enum class ScanResult { complete, need_more_data };

// Incrementally locates the end of the next complete RESP reply across TCP
// segments without materializing any values. Ported from bun's `ReplyScanner`:
// persisting the byte offset and the stack of in-progress aggregates keeps each
// buffered byte examined a bounded number of times (avoids the O(N^2) re-parse
// a hostile server could otherwise force). `InvalidResponse` is the parser's
// "ran out of bytes" sentinel and maps to need_more_data.
class ReplyScanner {
public:
    auto reset() noexcept -> void { position_ = 0; stack_.clear(); crlfSkip_ = 0; }
    [[nodiscard]] auto position() const noexcept -> std::size_t { return position_; }
    [[nodiscard]] auto scan(std::span<const unsigned char> buffer) -> std::expected<ScanResult, RedisError>;

private:
    struct Cursor {
        std::span<const unsigned char> buffer;
        std::size_t pos;
        std::size_t crlfSkip;

        auto read_byte() -> std::expected<unsigned char, RedisError> {
            if (pos >= buffer.size()) return std::unexpected(RedisError::invalid_response);
            return buffer[pos++];
        }
        auto read_line() -> std::expected<std::span<const unsigned char>, RedisError> {
            const auto remaining = buffer.subspan(pos);
            const auto limit = std::min(remaining.size(), ValkeyReader::max_line_length + 1);
            const auto start = std::min(crlfSkip, limit);
            crlfSkip = 0;
            for (std::size_t index = start; index < limit; ++index) {
                if (remaining[index] == '\r' && remaining.size() > index + 1 && remaining[index + 1] == '\n') {
                    const auto result = remaining.first(index);
                    pos += index + 2;
                    return result;
                }
            }
            return std::unexpected(remaining.size() > ValkeyReader::max_line_length + 1
                ? RedisError::line_too_long : RedisError::invalid_response);
        }
        auto read_integer() -> std::expected<std::int64_t, RedisError> {
            const auto line = read_line();
            if (!line) return std::unexpected(line.error());
            std::string value(line->begin(), line->end());
            std::int64_t parsed {};
            auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (error != std::errc {} || end != value.data() + value.size()) {
                return std::unexpected(RedisError::invalid_integer);
            }
            return parsed;
        }
    };

    // Skip a single element at cursor.pos. Returns the child count for an
    // aggregate (0 for a scalar / empty aggregate), or an error.
    [[nodiscard]] static auto scan_one(Cursor& cursor, std::size_t depth)
        -> std::expected<std::uint64_t, RedisError>;

    std::size_t position_ { 0 };
    std::vector<std::uint64_t> stack_ {};
    std::size_t crlfSkip_ { 0 };
};

inline auto ReplyScanner::scan_one(Cursor& cursor, std::size_t depth)
    -> std::expected<std::uint64_t, RedisError> {
    const auto tag = cursor.read_byte();
    if (!tag) return std::unexpected(tag.error());
    const auto type = resp_type_from_byte(*tag);
    if (!type) return std::unexpected(RedisError::invalid_response_type);

    switch (*type) {
    case RESPType::simple_string:
    case RESPType::error:
    case RESPType::integer:
    case RESPType::null:
    case RESPType::double_value:
    case RESPType::boolean:
    case RESPType::big_number: {
        const auto line = cursor.read_line();
        if (!line) return std::unexpected(line.error());
        return std::uint64_t { 0 };
    }
    case RESPType::bulk_string:
    case RESPType::blob_error:
    case RESPType::verbatim_string: {
        const auto invalid = *type == RESPType::blob_error ? RedisError::invalid_blob_error
            : *type == RESPType::verbatim_string ? RedisError::invalid_verbatim_string
            : RedisError::invalid_bulk_string;
        const auto length = cursor.read_integer();
        if (!length) return std::unexpected(length.error());
        if (*length < 0) {
            if (*type == RESPType::bulk_string) return std::uint64_t { 0 };  // $-1 null
            return std::unexpected(invalid);
        }
        if (*length > ValkeyReader::max_bulk_length) return std::unexpected(invalid);
        const auto size = static_cast<std::size_t>(*length);
        if (cursor.buffer.size() - cursor.pos < size + 2) return std::unexpected(RedisError::invalid_response);
        if (cursor.buffer[cursor.pos + size] != '\r' || cursor.buffer[cursor.pos + size + 1] != '\n') {
            return std::unexpected(invalid);
        }
        cursor.pos += size + 2;
        return std::uint64_t { 0 };
    }
    case RESPType::array:
    case RESPType::set:
    case RESPType::push: {
        if (depth >= ValkeyReader::max_nesting_depth) return std::unexpected(RedisError::nesting_depth_exceeded);
        const auto length = cursor.read_integer();
        if (!length) return std::unexpected(length.error());
        if (*type == RESPType::array && *length < 0) return std::uint64_t { 0 };  // *-1 null
        if (*type == RESPType::set && *length < 0) return std::unexpected(RedisError::invalid_set);
        if (*type == RESPType::push && *length <= 0) return std::unexpected(RedisError::invalid_push);
        return static_cast<std::uint64_t>(*length);
    }
    case RESPType::map: {
        if (depth >= ValkeyReader::max_nesting_depth) return std::unexpected(RedisError::nesting_depth_exceeded);
        const auto length = cursor.read_integer();
        if (!length) return std::unexpected(length.error());
        if (*length < 0) return std::unexpected(RedisError::invalid_map);
        return static_cast<std::uint64_t>(*length) * 2;
    }
    case RESPType::attribute: {
        if (depth >= ValkeyReader::max_nesting_depth) return std::unexpected(RedisError::nesting_depth_exceeded);
        const auto length = cursor.read_integer();
        if (!length) return std::unexpected(length.error());
        if (*length < 0) return std::unexpected(RedisError::invalid_attribute);
        return static_cast<std::uint64_t>(*length) * 2 + 1;
    }
    }
    return std::unexpected(RedisError::invalid_response_type);
}

inline auto ReplyScanner::scan(std::span<const unsigned char> buffer)
    -> std::expected<ScanResult, RedisError> {
    for (;;) {
        Cursor cursor { buffer, position_, crlfSkip_ };
        const auto children = scan_one(cursor, stack_.size());
        if (!children) {
            if (children.error() == RedisError::invalid_response) {
                crlfSkip_ = cursor.pos == position_ + 1
                    ? (buffer.size() - cursor.pos == 0 ? 0 : buffer.size() - cursor.pos - 1)
                    : 0;
                return ScanResult::need_more_data;
            }
            return std::unexpected(children.error());
        }
        crlfSkip_ = 0;
        position_ = cursor.pos;
        if (*children > 0) { stack_.push_back(*children); continue; }
        while (!stack_.empty()) {
            if (--stack_.back() > 0) break;
            stack_.pop_back();
        }
        if (stack_.empty()) return ScanResult::complete;
    }
}

// Single-shot, http `parse_request`-style streaming entry point. Reports
// whether `buffer` holds a complete top-level reply, how many bytes it
// consumed, and (on completion) the materialized value.
enum class ParseStatus { complete, incomplete, invalid };

struct ParsedReply {
    ParseStatus status { ParseStatus::incomplete };
    std::size_t consumed { 0 };
    RESPValue value {};
    RedisError error { RedisError::invalid_response };
};

[[nodiscard]] inline auto parse_reply(std::span<const unsigned char> buffer) -> ParsedReply {
    ReplyScanner scanner;
    const auto scanned = scanner.scan(buffer);
    if (!scanned) return { ParseStatus::invalid, 0, {}, scanned.error() };
    if (*scanned == ScanResult::need_more_data) return { ParseStatus::incomplete, 0, {}, RedisError::invalid_response };
    ValkeyReader reader(buffer);
    auto value = reader.read_value();
    if (!value) return { ParseStatus::invalid, 0, {}, value.error() };
    return { ParseStatus::complete, reader.position(), std::move(*value), RedisError::invalid_response };
}

}
