// neutral_literal_seam.cppm — JSC-neutral literal materialization seam.
//
// This is deliberately not Bun's `ExprData` → `JSValue` bridge: it neither
// consumes `mbun.ast::Arena`/`Node` nor allocates or protects a JSC value.
// It is a neutral test seam for nested literal conversion only. The real
// bridge remains DEFERRED until an AST adapter and JSC ownership contract exist.
//
// ref: bun-ref/src/js_parser_jsc/expr_jsc.rs and
// bun-zig-src/src/js_parser_jsc/expr_jsc.zig.
export module mbun.neutral_literal_seam;

import std;

export namespace mbun::neutral_literal_seam {

enum class LiteralKind : std::uint8_t {
    Null,
    Undefined,
    Boolean,
    Number,
    String,
    Array,
    Object,
    InlinedEnum,
    Identifier,
    Unsupported,
};

struct Literal {
    LiteralKind kind { LiteralKind::Unsupported };
    bool booleanValue { false };
    double numberValue { 0 };
    std::string stringValue;
    std::vector<Literal> items;
    std::vector<std::pair<std::string, Literal>> properties;

    static Literal null() { return { LiteralKind::Null }; }
    static Literal undefined() { return { LiteralKind::Undefined }; }
    static Literal boolean(bool value) { return { LiteralKind::Boolean, value }; }
    static Literal number(double value) {
        Literal out { LiteralKind::Number };
        out.numberValue = value;
        return out;
    }
    static Literal string(std::string value) {
        Literal out { LiteralKind::String };
        out.stringValue = std::move(value);
        return out;
    }
    static Literal array(std::vector<Literal> value) {
        Literal out { LiteralKind::Array };
        out.items = std::move(value);
        return out;
    }
    static Literal object(std::vector<std::pair<std::string, Literal>> value) {
        Literal out { LiteralKind::Object };
        out.properties = std::move(value);
        return out;
    }
    static Literal inlined_enum(Literal value) {
        Literal out { LiteralKind::InlinedEnum };
        out.items.push_back(std::move(value));
        return out;
    }
};

struct Value {
    LiteralKind kind { LiteralKind::Unsupported };
    bool booleanValue { false };
    double numberValue { 0 };
    std::string stringValue;
    std::vector<Value> items;
    std::vector<std::pair<std::string, Value>> properties;
};

enum class Error : std::uint8_t { CannotConvertIdentifier, CannotConvertArgument, StackOverflow };

using Result = std::expected<Value, Error>;

Result materialize(const Literal& literal, std::size_t depth = 0, std::size_t max_depth = 512) {
    if (depth >= max_depth) return std::unexpected(Error::StackOverflow);
    switch (literal.kind) {
        case LiteralKind::Null:
        case LiteralKind::Undefined:
        case LiteralKind::Boolean:
        case LiteralKind::Number:
        case LiteralKind::String: {
            Value out { literal.kind };
            out.booleanValue = literal.booleanValue;
            out.numberValue = literal.numberValue;
            out.stringValue = literal.stringValue;
            return out;
        }
        case LiteralKind::InlinedEnum:
            if (literal.items.size() != 1) return std::unexpected(Error::CannotConvertArgument);
            return materialize(literal.items.front(), depth + 1, max_depth);
        case LiteralKind::Identifier:
            return std::unexpected(Error::CannotConvertIdentifier);
        case LiteralKind::Array: {
            Value out { LiteralKind::Array };
            out.items.reserve(literal.items.size());
            for (const auto& item : literal.items) {
                auto value { materialize(item, depth + 1, max_depth) };
                if (!value) return std::unexpected(value.error());
                out.items.push_back(std::move(*value));
            }
            return out;
        }
        case LiteralKind::Object: {
            Value out { LiteralKind::Object };
            out.properties.reserve(literal.properties.size());
            for (const auto& [key, item] : literal.properties) {
                auto value { materialize(item, depth + 1, max_depth) };
                if (!value) return std::unexpected(value.error());
                out.properties.emplace_back(key, std::move(*value));
            }
            return out;
        }
        case LiteralKind::Unsupported:
            return std::unexpected(Error::CannotConvertArgument);
    }
    return std::unexpected(Error::CannotConvertArgument);
}

} // namespace mbun::neutral_literal_seam
