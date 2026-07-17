export module mbun.runtime_valkey.result;

import std;

export namespace mbun::runtime_valkey {

enum class RedisError : unsigned char {
    invalid_response, invalid_response_type, invalid_integer, invalid_double, invalid_boolean,
    invalid_bulk_string, invalid_blob_error, invalid_verbatim_string, invalid_array, invalid_map,
    invalid_set, invalid_attribute, invalid_push, line_too_long, nesting_depth_exceeded,
};

enum class RESPType : unsigned char {
    simple_string = '+', error = '-', integer = ':', bulk_string = '$', array = '*', null_value = '_',
    double_value = ',', boolean = '#', blob_error = '!', verbatim_string = '=', map = '%', set = '~',
    attribute = '|', push = '>', big_number = '(',
};

struct RESPValue {
    using Child = std::shared_ptr<RESPValue>;
    RESPType type { RESPType::null_value };
    std::string bytes {};
    std::int64_t integer { 0 };
    double floating { 0.0 };
    bool boolean { false };
    std::string format {};
    std::vector<Child> children {};
    std::vector<std::pair<Child, Child>> entries {};
    Child value {};
};

class Reader {
public:
    static constexpr std::size_t max_nesting_depth = 128;
    static constexpr std::size_t max_line_length = 512 * 1024;
    static constexpr std::int64_t max_bulk_length = 512LL * 1024 * 1024;

    explicit Reader(std::string_view buffer) : buffer_(buffer) {}
    [[nodiscard]] auto position() const noexcept -> std::size_t { return position_; }
    [[nodiscard]] auto read_value() -> std::expected<RESPValue, RedisError> { return read_value_at_(0); }

private:
    [[nodiscard]] auto line_() -> std::expected<std::string_view, RedisError> {
        const auto remaining = buffer_.substr(position_);
        const auto limit = std::min(remaining.size(), max_line_length + 1);
        for (std::size_t i { 0 }; i + 1 < limit; ++i) {
            if (remaining[i] == '\r' && remaining[i + 1] == '\n') {
                position_ += i + 2;
                return remaining.substr(0, i);
            }
        }
        return std::unexpected(remaining.size() > max_line_length + 1
                ? RedisError::line_too_long : RedisError::invalid_response);
    }

    [[nodiscard]] auto integer_() -> std::expected<std::int64_t, RedisError> {
        const auto line = line_();
        if (!line) return std::unexpected(RedisError::invalid_integer);
        std::int64_t value {};
        const auto [end, error] = std::from_chars(line->data(), line->data() + line->size(), value);
        if (error != std::errc {} || end != line->data() + line->size())
            return std::unexpected(RedisError::invalid_integer);
        return value;
    }

    [[nodiscard]] auto read_value_at_(std::size_t depth) -> std::expected<RESPValue, RedisError> {
        if (position_ >= buffer_.size()) return std::unexpected(RedisError::invalid_response);
        const auto tag = static_cast<RESPType>(static_cast<unsigned char>(buffer_[position_++]));
        const auto line_value = [&]() { return line_(); };
        if (tag == RESPType::simple_string || tag == RESPType::error || tag == RESPType::big_number) {
            const auto line = line_value();
            if (!line) return std::unexpected(line.error());
            return RESPValue { tag, std::string(*line) };
        }
        if (tag == RESPType::integer) {
            const auto value = integer_();
            if (!value) return std::unexpected(value.error());
            RESPValue result; result.type = tag; result.integer = *value; return result;
        }
        if (tag == RESPType::null_value) {
            const auto line = line_value();
            if (!line || !line->empty()) return std::unexpected(RedisError::invalid_response);
            return RESPValue {};
        }
        if (tag == RESPType::boolean) {
            const auto line = line_value();
            if (!line || line->size() != 1 || ((*line)[0] != 't' && (*line)[0] != 'f'))
                return std::unexpected(RedisError::invalid_boolean);
            RESPValue result; result.type = tag; result.boolean = (*line)[0] == 't'; return result;
        }
        if (tag == RESPType::double_value) {
            const auto line = line_value();
            if (!line) return std::unexpected(RedisError::invalid_double);
            double value {};
            const auto [end, error] = std::from_chars(line->data(), line->data() + line->size(), value);
            if (error != std::errc {} || end != line->data() + line->size()) {
                if (*line == "inf") value = std::numeric_limits<double>::infinity();
                else if (*line == "-inf") value = -std::numeric_limits<double>::infinity();
                else if (*line == "nan") value = std::numeric_limits<double>::quiet_NaN();
                else return std::unexpected(RedisError::invalid_double);
            }
            RESPValue result; result.type = tag; result.floating = value; return result;
        }
        if (depth >= max_nesting_depth) return std::unexpected(RedisError::nesting_depth_exceeded);
        const auto length = integer_();
        if (!length) return std::unexpected(length.error());
        if (tag == RESPType::bulk_string || tag == RESPType::blob_error || tag == RESPType::verbatim_string) {
            if (tag == RESPType::bulk_string && *length < 0) { RESPValue result; result.type = tag; return result; }
            if (*length < 0 || *length > max_bulk_length) return std::unexpected(RedisError::invalid_bulk_string);
            const auto size = static_cast<std::size_t>(*length);
            if (size > buffer_.size() - position_) return std::unexpected(RedisError::invalid_response);
            RESPValue result; result.type = tag; result.bytes = std::string(buffer_.substr(position_, size)); position_ += size;
            const auto crlf = line_();
            if (!crlf || !crlf->empty()) return std::unexpected(RedisError::invalid_bulk_string);
            if (tag == RESPType::verbatim_string) {
                if (result.bytes.size() < 4 || result.bytes[3] != ':') return std::unexpected(RedisError::invalid_verbatim_string);
                result.format = result.bytes.substr(0, 3); result.bytes.erase(0, 4);
            }
            return result;
        }
        if (*length < 0) return std::unexpected(RedisError::invalid_array);
        RESPValue result; result.type = tag;
        if (tag == RESPType::map || tag == RESPType::attribute) {
            result.entries.reserve(static_cast<std::size_t>(*length));
            for (std::int64_t i {}; i < *length; ++i) {
                auto key = read_value_at_(depth + 1); if (!key) return std::unexpected(key.error());
                auto value = read_value_at_(depth + 1); if (!value) return std::unexpected(value.error());
                result.entries.emplace_back(std::make_shared<RESPValue>(std::move(*key)), std::make_shared<RESPValue>(std::move(*value)));
            }
            if (tag == RESPType::attribute) { auto value = read_value_at_(depth + 1); if (!value) return std::unexpected(value.error()); result.value = std::make_shared<RESPValue>(std::move(*value)); }
        } else {
            if (tag == RESPType::push && *length <= 0) return std::unexpected(RedisError::invalid_push);
            result.children.reserve(static_cast<std::size_t>(*length));
            for (std::int64_t i {}; i < *length; ++i) { auto child = read_value_at_(depth + 1); if (!child) return std::unexpected(child.error()); result.children.emplace_back(std::make_shared<RESPValue>(std::move(*child))); }
        }
        return result;
    }
    std::string_view buffer_ {};
    std::size_t position_ { 0 };
};

enum class ConversionMode : unsigned char { string, buffer };
struct JsValue {
    using Array = std::vector<std::shared_ptr<JsValue>>;
    using Object = std::vector<std::pair<std::shared_ptr<JsValue>, std::shared_ptr<JsValue>>>;
    std::variant<std::monostate, bool, std::int64_t, double, std::string, Array, Object> value {};
};

[[nodiscard]] inline auto to_js_value(const RESPValue& source, ConversionMode mode = ConversionMode::string) -> JsValue {
    JsValue result;
    switch (source.type) {
    case RESPType::null_value: return result;
    case RESPType::boolean: result.value = source.boolean; return result;
    case RESPType::integer: result.value = source.integer; return result;
    case RESPType::double_value: result.value = source.floating; return result;
    case RESPType::simple_string: case RESPType::error: case RESPType::bulk_string: case RESPType::blob_error:
    case RESPType::verbatim_string: case RESPType::big_number:
        result.value = source.bytes; return result;
    case RESPType::array: case RESPType::set: case RESPType::push: {
        JsValue::Array array; array.reserve(source.children.size());
        for (const auto& child : source.children) array.emplace_back(std::make_shared<JsValue>(to_js_value(*child, mode)));
        result.value = std::move(array); return result;
    }
    case RESPType::map: case RESPType::attribute: {
        JsValue::Object object; object.reserve(source.entries.size());
        for (const auto& [key, value] : source.entries) object.emplace_back(std::make_shared<JsValue>(to_js_value(*key, mode)), std::make_shared<JsValue>(to_js_value(*value, mode)));
        result.value = std::move(object); return result;
    }
    }
    return result;
}

}
