// PostgreSQL type names, errors, and shared SQL result modes.
// Reference: bun-ref src/sql/postgres/PostgresTypes.rs and AnyPostgresError.rs;
// Zig counterparts are src/sql/postgres/PostgresTypes.zig and AnyPostgresError.zig.
export module mbun.postgres.types;

import std;

namespace mbun::postgres {

export enum class ResultMode : std::uint8_t { Objects, Values, Raw };

export enum class TypeId : std::uint32_t {
    Bool = 16, Bytea = 17, Char = 18, Name = 19, Int8 = 20, Int2 = 21,
    Int4 = 23, Text = 25, Json = 114, Float4 = 700, Float8 = 701,
    Varchar = 1043, Date = 1082, Timestamp = 1114, Timestamptz = 1184,
    Numeric = 1700, Jsonb = 3802,
};

export enum class ErrorCode : std::uint8_t {
    ConnectionClosed, ConnectionFailed, ConnectionRefused, ExpectedRequest,
    ExpectedStatement, InvalidBackendKeyData, InvalidBinaryData,
    InvalidByteSequence, InvalidByteSequenceForEncoding, InvalidCharacter,
    InvalidMessage, InvalidMessageLength, InvalidQueryBinding, InvalidServerKey,
    InvalidServerSignature, InvalidTimeFormat, JSError, JSTerminated,
    MultidimensionalArrayNotSupportedYet, NullsInArrayNotSupportedYet,
    OutOfMemory, Overflow, PBKDFD2, SaslSignatureMismatch,
    SaslSignatureInvalidBase64, ShortRead, TLSNotAvailable, TLSUpgradeFailed,
    TooManyParameters, UnexpectedMessage, UnknownAuthenticationMethod,
    UnsupportedAuthenticationMethod, UnsupportedByteaFormat,
    UnsupportedIntegerSize, UnsupportedArrayFormat, UnsupportedNumericFormat,
    UnknownFormatCode,
};

export constexpr std::string_view error_name(ErrorCode code) {
    constexpr std::array names {
        "ConnectionClosed", "ConnectionFailed", "ConnectionRefused", "ExpectedRequest",
        "ExpectedStatement", "InvalidBackendKeyData", "InvalidBinaryData",
        "InvalidByteSequence", "InvalidByteSequenceForEncoding", "InvalidCharacter",
        "InvalidMessage", "InvalidMessageLength", "InvalidQueryBinding", "InvalidServerKey",
        "InvalidServerSignature", "InvalidTimeFormat", "JSError", "JSTerminated",
        "MultidimensionalArrayNotSupportedYet", "NullsInArrayNotSupportedYet", "OutOfMemory",
        "Overflow", "PBKDFD2", "SASL_SIGNATURE_MISMATCH", "SASL_SIGNATURE_INVALID_BASE64",
        "ShortRead", "TLSNotAvailable", "TLSUpgradeFailed", "TooManyParameters",
        "UnexpectedMessage", "UNKNOWN_AUTHENTICATION_METHOD", "UNSUPPORTED_AUTHENTICATION_METHOD",
        "UnsupportedByteaFormat", "UnsupportedIntegerSize", "UnsupportedArrayFormat",
        "UnsupportedNumericFormat", "UnknownFormatCode",
    };
    auto index { static_cast<std::size_t>(code) };
    return index < names.size() ? std::string_view { names[index] } : std::string_view { "JSError" };
}

// The JS-visible `error.code` for each ErrorCode.
// PORT-SOURCE: bun-ref src/sql_jsc/postgres/error_jsc.rs postgres_error_to_js.
export constexpr std::string_view error_js_code(ErrorCode code) {
    constexpr std::array codes {
        "ERR_POSTGRES_CONNECTION_CLOSED", "ERR_POSTGRES_CONNECTION_FAILED",
        "ERR_POSTGRES_CONNECTION_REFUSED", "ERR_POSTGRES_EXPECTED_REQUEST",
        "ERR_POSTGRES_EXPECTED_STATEMENT", "ERR_POSTGRES_INVALID_BACKEND_KEY_DATA",
        "ERR_POSTGRES_INVALID_BINARY_DATA", "ERR_POSTGRES_INVALID_BYTE_SEQUENCE",
        "ERR_POSTGRES_INVALID_BYTE_SEQUENCE_FOR_ENCODING", "ERR_POSTGRES_INVALID_CHARACTER",
        "ERR_POSTGRES_INVALID_MESSAGE", "ERR_POSTGRES_INVALID_MESSAGE_LENGTH",
        "ERR_POSTGRES_INVALID_QUERY_BINDING", "ERR_POSTGRES_INVALID_SERVER_KEY",
        "ERR_POSTGRES_INVALID_SERVER_SIGNATURE", "ERR_POSTGRES_INVALID_TIME_FORMAT",
        "ERR_POSTGRES_JS_ERROR", "ERR_POSTGRES_JS_TERMINATED",
        "ERR_POSTGRES_MULTIDIMENSIONAL_ARRAY_NOT_SUPPORTED_YET",
        "ERR_POSTGRES_NULLS_IN_ARRAY_NOT_SUPPORTED_YET", "ERR_POSTGRES_OUT_OF_MEMORY",
        "ERR_POSTGRES_OVERFLOW", "ERR_POSTGRES_AUTHENTICATION_FAILED_PBKDF2",
        "ERR_POSTGRES_SASL_SIGNATURE_MISMATCH", "ERR_POSTGRES_SASL_SIGNATURE_INVALID_BASE64",
        "ERR_POSTGRES_SHORT_READ", "ERR_POSTGRES_TLS_NOT_AVAILABLE",
        "ERR_POSTGRES_TLS_UPGRADE_FAILED", "ERR_POSTGRES_TOO_MANY_PARAMETERS",
        "ERR_POSTGRES_UNEXPECTED_MESSAGE", "ERR_POSTGRES_UNKNOWN_AUTHENTICATION_METHOD",
        "ERR_POSTGRES_UNSUPPORTED_AUTHENTICATION_METHOD", "ERR_POSTGRES_UNSUPPORTED_BYTEA_FORMAT",
        "ERR_POSTGRES_UNSUPPORTED_INTEGER_SIZE", "ERR_POSTGRES_UNSUPPORTED_ARRAY_FORMAT",
        "ERR_POSTGRES_UNSUPPORTED_NUMERIC_FORMAT", "ERR_POSTGRES_UNKNOWN_FORMAT_CODE",
    };
    auto index { static_cast<std::size_t>(code) };
    return index < codes.size() ? std::string_view { codes[index] } : std::string_view { "ERR_POSTGRES_JS_ERROR" };
}

export struct PostgresErrorOptions {
    std::string_view code {};
    std::optional<std::string_view> severity {};
    std::optional<std::string_view> detail {};
    std::optional<std::string_view> hint {};
    std::optional<std::string_view> position {};
    std::optional<std::string_view> schema {};
    std::optional<std::string_view> table {};
    std::optional<std::string_view> column {};
    std::optional<std::string_view> constraint {};
    std::optional<std::string_view> routine {};
};

} // namespace mbun::postgres
