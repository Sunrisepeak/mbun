// PostgreSQL wire protocol v3 codec — pure logic, injection-style (no socket/TLS).
//
// Frontend message encoders + incremental backend message decoder. The decoder
// mirrors mbun.http parse_request: it consumes a byte buffer, reports how many
// bytes it consumed, and asks for more when a message spans packets — so the
// transport can be driven by an event loop later. Real socket/TLS/auth handshake
// is DEFERRED(S-net): those need the reactor; this file is codec-only.
//
// Blueprint: bun-ref src/sql/postgres/PostgresProtocol.rs (message set / framing)
// aligned to the official PostgreSQL v3 frontend/backend wire format. Byte order
// is network (big-endian); message length includes the 4-byte length field and,
// for tagged messages, excludes the 1-byte type tag.
export module mbun.postgres.wire;

import std;
import mbun.postgres.types;

namespace mbun::postgres {

// ── shared byte primitives ──────────────────────────────────────────────────

export inline constexpr std::int32_t PROTOCOL_VERSION_3 { 196608 };  // 3.0 << 16
export inline constexpr std::int32_t SSL_REQUEST_CODE { 80877103 };

inline void put_u8_(std::vector<std::byte>& b, std::uint8_t v) {
    b.push_back(std::byte { v });
}
inline void put_i16_(std::vector<std::byte>& b, std::int16_t v) {
    auto u { static_cast<std::uint16_t>(v) };
    put_u8_(b, static_cast<std::uint8_t>(u >> 8));
    put_u8_(b, static_cast<std::uint8_t>(u));
}
inline void put_i32_(std::vector<std::byte>& b, std::int32_t v) {
    auto u { static_cast<std::uint32_t>(v) };
    put_u8_(b, static_cast<std::uint8_t>(u >> 24));
    put_u8_(b, static_cast<std::uint8_t>(u >> 16));
    put_u8_(b, static_cast<std::uint8_t>(u >> 8));
    put_u8_(b, static_cast<std::uint8_t>(u));
}
inline void put_cstr_(std::vector<std::byte>& b, std::string_view s) {
    for (char c : s) put_u8_(b, static_cast<std::uint8_t>(c));
    put_u8_(b, 0);
}
inline void put_bytes_(std::vector<std::byte>& b, std::span<const std::byte> s) {
    b.insert(b.end(), s.begin(), s.end());
}
// Overwrite the 4 length bytes at `at` with (buf.size() - at), big-endian.
inline void patch_len_(std::vector<std::byte>& b, std::size_t at) {
    auto len { static_cast<std::uint32_t>(b.size() - at) };
    b[at + 0] = std::byte { static_cast<std::uint8_t>(len >> 24) };
    b[at + 1] = std::byte { static_cast<std::uint8_t>(len >> 16) };
    b[at + 2] = std::byte { static_cast<std::uint8_t>(len >> 8) };
    b[at + 3] = std::byte { static_cast<std::uint8_t>(len) };
}

// ── frontend encoders ───────────────────────────────────────────────────────

export struct StartupParam {
    std::string name;
    std::string value;
};

// StartupMessage: no type tag; body is protocol version + key/value cstrs + NUL.
export inline std::vector<std::byte> encode_startup(std::span<const StartupParam> params) {
    std::vector<std::byte> b;
    std::size_t at { b.size() };
    put_i32_(b, 0);  // length placeholder
    put_i32_(b, PROTOCOL_VERSION_3);
    for (const auto& p : params) {
        put_cstr_(b, p.name);
        put_cstr_(b, p.value);
    }
    put_u8_(b, 0);  // terminating empty key
    patch_len_(b, at);
    return b;
}

// SSLRequest: no type tag; length 8 then the magic request code.
export inline std::vector<std::byte> encode_ssl_request() {
    std::vector<std::byte> b;
    put_i32_(b, 8);
    put_i32_(b, SSL_REQUEST_CODE);
    return b;
}

// Generic tagged-message helper: write tag, length placeholder; caller fills body.
inline std::vector<std::byte> tagged_(char tag, auto&& fill_body) {
    std::vector<std::byte> b;
    put_u8_(b, static_cast<std::uint8_t>(tag));
    std::size_t at { b.size() };
    put_i32_(b, 0);
    fill_body(b);
    patch_len_(b, at);
    return b;
}

// Simple Query ('Q'): the SQL string, NUL-terminated.
export inline std::vector<std::byte> encode_query(std::string_view sql) {
    return tagged_('Q', [&](std::vector<std::byte>& b) { put_cstr_(b, sql); });
}

// Parse ('P'): statement name, query, parameter type OIDs.
export inline std::vector<std::byte> encode_parse(std::string_view name, std::string_view query,
                                                  std::span<const std::uint32_t> param_oids) {
    return tagged_('P', [&](std::vector<std::byte>& b) {
        put_cstr_(b, name);
        put_cstr_(b, query);
        put_i16_(b, static_cast<std::int16_t>(param_oids.size()));
        for (auto oid : param_oids) put_i32_(b, static_cast<std::int32_t>(oid));
    });
}

// A Bind parameter value: nullopt == SQL NULL (encoded as length -1).
export using BindValue = std::optional<std::vector<std::byte>>;

// Bind ('B'): portal, statement, param format codes, param values, result codes.
export inline std::vector<std::byte> encode_bind(std::string_view portal, std::string_view statement,
                                                 std::span<const std::int16_t> param_formats,
                                                 std::span<const BindValue> params,
                                                 std::span<const std::int16_t> result_formats) {
    return tagged_('B', [&](std::vector<std::byte>& b) {
        put_cstr_(b, portal);
        put_cstr_(b, statement);
        put_i16_(b, static_cast<std::int16_t>(param_formats.size()));
        for (auto f : param_formats) put_i16_(b, f);
        put_i16_(b, static_cast<std::int16_t>(params.size()));
        for (const auto& v : params) {
            if (!v.has_value()) { put_i32_(b, -1); continue; }
            put_i32_(b, static_cast<std::int32_t>(v->size()));
            put_bytes_(b, *v);
        }
        put_i16_(b, static_cast<std::int16_t>(result_formats.size()));
        for (auto f : result_formats) put_i16_(b, f);
    });
}

// Describe ('D'): kind 'S' (prepared statement) or 'P' (portal), plus name.
export inline std::vector<std::byte> encode_describe(char kind, std::string_view name) {
    return tagged_('D', [&](std::vector<std::byte>& b) { put_u8_(b, static_cast<std::uint8_t>(kind)); put_cstr_(b, name); });
}

// Execute ('E'): portal name, max rows (0 == unlimited).
export inline std::vector<std::byte> encode_execute(std::string_view portal, std::int32_t max_rows = 0) {
    return tagged_('E', [&](std::vector<std::byte>& b) { put_cstr_(b, portal); put_i32_(b, max_rows); });
}

// Close ('C'): kind 'S' or 'P', plus name.
export inline std::vector<std::byte> encode_close(char kind, std::string_view name) {
    return tagged_('C', [&](std::vector<std::byte>& b) { put_u8_(b, static_cast<std::uint8_t>(kind)); put_cstr_(b, name); });
}

// Body-less frontend messages.
export inline std::vector<std::byte> encode_sync() { return tagged_('S', [](std::vector<std::byte>&) {}); }
export inline std::vector<std::byte> encode_flush() { return tagged_('H', [](std::vector<std::byte>&) {}); }
export inline std::vector<std::byte> encode_terminate() { return tagged_('X', [](std::vector<std::byte>&) {}); }

// PasswordMessage ('p'): cleartext or MD5-hashed password string, NUL-terminated.
export inline std::vector<std::byte> encode_password(std::string_view password) {
    return tagged_('p', [&](std::vector<std::byte>& b) { put_cstr_(b, password); });
}

// SASLInitialResponse ('p'): mechanism name + initial client response (-1 if absent).
export inline std::vector<std::byte> encode_sasl_initial(std::string_view mechanism,
                                                         std::span<const std::byte> initial) {
    return tagged_('p', [&](std::vector<std::byte>& b) {
        put_cstr_(b, mechanism);
        put_i32_(b, static_cast<std::int32_t>(initial.size()));
        put_bytes_(b, initial);
    });
}

// SASLResponse ('p'): raw SASL data filling the message body (no length prefix).
export inline std::vector<std::byte> encode_sasl_response(std::span<const std::byte> data) {
    return tagged_('p', [&](std::vector<std::byte>& b) { put_bytes_(b, data); });
}

// ── backend decoder ─────────────────────────────────────────────────────────

export enum class DecodeStatus : std::uint8_t {
    Ok,          // one full message decoded
    Incomplete,  // need more bytes (message spans packets)
    Invalid,     // malformed framing / length
};

export enum class BackendTag : char {
    Authentication = 'R', ParameterStatus = 'S', BackendKeyData = 'K', ReadyForQuery = 'Z',
    RowDescription = 'T', DataRow = 'D', CommandComplete = 'C', ErrorResponse = 'E',
    NoticeResponse = 'N', ParseComplete = '1', BindComplete = '2', CloseComplete = '3',
    NoData = 'n', ParameterDescription = 't', EmptyQueryResponse = 'I', PortalSuspended = 's',
    NotificationResponse = 'A',
};

// Authentication request sub-kinds (the int32 that follows the 'R' header).
export enum class AuthKind : std::int32_t {
    Ok = 0, KerberosV5 = 2, CleartextPassword = 3, MD5Password = 5, SCMCredential = 6,
    GSS = 7, GSSContinue = 8, SSPI = 9, SASL = 10, SASLContinue = 11, SASLFinal = 12,
};

export struct AuthenticationMsg {
    AuthKind kind { AuthKind::Ok };
    std::array<std::byte, 4> md5_salt {};        // valid when kind == MD5Password
    std::vector<std::string> sasl_mechanisms {}; // valid when kind == SASL
    std::vector<std::byte> data {};              // SASLContinue / SASLFinal payload
};

export struct ParameterStatusMsg { std::string name; std::string value; };
export struct BackendKeyDataMsg { std::int32_t process_id {}; std::int32_t secret_key {}; };
export struct ReadyForQueryMsg { char status {}; };  // 'I' idle, 'T' in-tx, 'E' failed-tx

export struct FieldDescriptionWire {
    std::string name;
    std::int32_t table_oid {};
    std::int16_t column_index {};
    std::int32_t type_oid {};
    std::int16_t type_size {};
    std::int32_t type_modifier {};
    std::int16_t format_code {};
};
export struct RowDescriptionMsg { std::vector<FieldDescriptionWire> fields; };

// Each column: nullopt == SQL NULL (wire length -1); otherwise raw column bytes.
export struct DataRowMsg { std::vector<std::optional<std::vector<std::byte>>> columns; };
export struct CommandCompleteMsg { std::string tag; };
export struct ParameterDescriptionMsg { std::vector<std::uint32_t> type_oids; };
export struct NotificationResponseMsg { std::int32_t process_id {}; std::string channel; std::string payload; };

export struct ErrorField { char code {}; std::string value; };
export struct ErrorResponseMsg {
    std::vector<ErrorField> fields;
    [[nodiscard]] std::string_view field(char code) const {
        for (const auto& f : fields) if (f.code == code) return f.value;
        return {};
    }
    [[nodiscard]] std::string_view severity() const { return field('S'); }
    [[nodiscard]] std::string_view code_str() const { return field('C'); }
    [[nodiscard]] std::string_view message() const { return field('M'); }
    [[nodiscard]] std::string_view detail() const { return field('D'); }
    [[nodiscard]] std::string_view hint() const { return field('H'); }
};

export using BackendPayload = std::variant<
    std::monostate,  // tag-only: ParseComplete/BindComplete/CloseComplete/NoData/EmptyQueryResponse
    AuthenticationMsg, ParameterStatusMsg, BackendKeyDataMsg, ReadyForQueryMsg,
    RowDescriptionMsg, DataRowMsg, CommandCompleteMsg, ParameterDescriptionMsg,
    NotificationResponseMsg, ErrorResponseMsg>;

export struct BackendMessage {
    BackendTag tag {};
    bool is_notice { false };  // true when tag == 'N' (NoticeResponse shares ErrorResponseMsg)
    BackendPayload payload {};
};

export struct DecodeResult {
    DecodeStatus status { DecodeStatus::Incomplete };
    std::size_t bytes_read { 0 };  // total bytes consumed (tag + length + body) on Ok
    BackendMessage message {};
};

// Bounds-checked big-endian cursor over the message body. Non-exported but
// external-linkage so the exported inline decoder may name it.
struct Cursor {
    std::string_view b;
    std::size_t i { 0 };
    bool bad { false };

    [[nodiscard]] std::size_t remaining() const { return i <= b.size() ? b.size() - i : 0; }
    std::uint8_t u8() {
        if (i + 1 > b.size()) { bad = true; return 0; }
        return static_cast<std::uint8_t>(b[i++]);
    }
    std::int16_t i16() {
        if (i + 2 > b.size()) { bad = true; return 0; }
        auto hi { static_cast<std::uint16_t>(static_cast<std::uint8_t>(b[i])) };
        auto lo { static_cast<std::uint16_t>(static_cast<std::uint8_t>(b[i + 1])) };
        i += 2;
        return static_cast<std::int16_t>((hi << 8) | lo);
    }
    std::int32_t i32() {
        if (i + 4 > b.size()) { bad = true; return 0; }
        std::uint32_t v { 0 };
        for (int k { 0 }; k < 4; ++k) v = (v << 8) | static_cast<std::uint8_t>(b[i + static_cast<std::size_t>(k)]);
        i += 4;
        return static_cast<std::int32_t>(v);
    }
    std::string cstr() {
        std::size_t start { i };
        while (i < b.size() && b[i] != '\0') ++i;
        if (i >= b.size()) { bad = true; return {}; }
        std::string s { b.substr(start, i - start) };
        ++i;  // skip NUL
        return s;
    }
    std::vector<std::byte> bytes(std::size_t n) {
        std::vector<std::byte> out;
        if (i + n > b.size()) { bad = true; return out; }
        out.reserve(n);
        for (std::size_t k { 0 }; k < n; ++k) out.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(b[i + k])));
        i += n;
        return out;
    }
};

// Decode a single backend message from the front of `buf`. Returns Incomplete
// (with bytes_read == 0) when fewer than a full message is buffered, so the
// caller can retry after appending more bytes. On Ok, bytes_read is the number
// of bytes to drop from the front before decoding the next message.
export inline DecodeResult decode_message(std::string_view buf) {
    DecodeResult r;
    if (buf.size() < 5) { r.status = DecodeStatus::Incomplete; return r; }
    char tag { buf[0] };
    // PG message length is a SIGNED Int32 (protocol §55.2.1); any value below the
    // 4 bytes the field itself occupies — including negatives on the wire — is a
    // malformed frame, not a request for more bytes. ref: bun-ref treats the
    // length as signed and rejects < 4 as InvalidMessageLength.
    std::uint32_t raw_len { 0 };
    for (int k { 1 }; k <= 4; ++k) raw_len = (raw_len << 8) | static_cast<std::uint8_t>(buf[static_cast<std::size_t>(k)]);
    std::int32_t len { static_cast<std::int32_t>(raw_len) };
    if (len < 4) { r.status = DecodeStatus::Invalid; return r; }
    std::size_t total { 1 + static_cast<std::size_t>(len) };  // tag + (length field counts itself)
    if (buf.size() < total) { r.status = DecodeStatus::Incomplete; return r; }

    std::string_view body { buf.substr(5, len - 4) };
    Cursor c { body, 0, false };
    BackendMessage m;

    switch (tag) {
    case 'R': {
        m.tag = BackendTag::Authentication;
        AuthenticationMsg a;
        auto raw { c.i32() };
        a.kind = static_cast<AuthKind>(raw);
        if (a.kind == AuthKind::MD5Password) {
            auto salt { c.bytes(4) };
            for (std::size_t k { 0 }; k < 4 && k < salt.size(); ++k) a.md5_salt[k] = salt[k];
        } else if (a.kind == AuthKind::SASL) {
            while (c.remaining() > 0 && !c.bad) {
                std::string mech { c.cstr() };
                if (mech.empty()) break;  // final NUL terminator
                a.sasl_mechanisms.push_back(std::move(mech));
            }
        } else if (a.kind == AuthKind::SASLContinue || a.kind == AuthKind::SASLFinal) {
            a.data = c.bytes(c.remaining());
        }
        m.payload = std::move(a);
        break;
    }
    case 'S': {
        m.tag = BackendTag::ParameterStatus;
        ParameterStatusMsg p;
        p.name = c.cstr();
        p.value = c.cstr();
        m.payload = std::move(p);
        break;
    }
    case 'K': {
        m.tag = BackendTag::BackendKeyData;
        BackendKeyDataMsg k;
        k.process_id = c.i32();
        k.secret_key = c.i32();
        m.payload = k;
        break;
    }
    case 'Z': {
        m.tag = BackendTag::ReadyForQuery;
        ReadyForQueryMsg z;
        z.status = static_cast<char>(c.u8());
        m.payload = z;
        break;
    }
    case 'T': {
        m.tag = BackendTag::RowDescription;
        RowDescriptionMsg rd;
        auto n { c.i16() };
        for (std::int16_t k { 0 }; k < n && !c.bad; ++k) {
            FieldDescriptionWire f;
            f.name = c.cstr();
            f.table_oid = c.i32();
            f.column_index = c.i16();
            f.type_oid = c.i32();
            f.type_size = c.i16();
            f.type_modifier = c.i32();
            f.format_code = c.i16();
            rd.fields.push_back(std::move(f));
        }
        m.payload = std::move(rd);
        break;
    }
    case 'D': {
        m.tag = BackendTag::DataRow;
        DataRowMsg dr;
        auto n { c.i16() };
        for (std::int16_t k { 0 }; k < n && !c.bad; ++k) {
            auto vlen { c.i32() };
            if (vlen < 0) dr.columns.emplace_back(std::nullopt);
            else dr.columns.emplace_back(c.bytes(static_cast<std::size_t>(vlen)));
        }
        m.payload = std::move(dr);
        break;
    }
    case 'C': {
        m.tag = BackendTag::CommandComplete;
        CommandCompleteMsg cc;
        cc.tag = c.cstr();
        m.payload = std::move(cc);
        break;
    }
    case 'E':
    case 'N': {
        m.tag = tag == 'E' ? BackendTag::ErrorResponse : BackendTag::NoticeResponse;
        m.is_notice = tag == 'N';
        ErrorResponseMsg e;
        while (c.remaining() > 0 && !c.bad) {
            auto code { c.u8() };
            if (code == 0) break;  // field-list terminator
            ErrorField f;
            f.code = static_cast<char>(code);
            f.value = c.cstr();
            e.fields.push_back(std::move(f));
        }
        m.payload = std::move(e);
        break;
    }
    case 't': {
        m.tag = BackendTag::ParameterDescription;
        ParameterDescriptionMsg pd;
        auto n { c.i16() };
        for (std::int16_t k { 0 }; k < n && !c.bad; ++k) pd.type_oids.push_back(static_cast<std::uint32_t>(c.i32()));
        m.payload = std::move(pd);
        break;
    }
    case 'A': {
        m.tag = BackendTag::NotificationResponse;
        NotificationResponseMsg nr;
        nr.process_id = c.i32();
        nr.channel = c.cstr();
        nr.payload = c.cstr();
        m.payload = std::move(nr);
        break;
    }
    case '1': m.tag = BackendTag::ParseComplete; break;
    case '2': m.tag = BackendTag::BindComplete; break;
    case '3': m.tag = BackendTag::CloseComplete; break;
    case 'n': m.tag = BackendTag::NoData; break;
    case 'I': m.tag = BackendTag::EmptyQueryResponse; break;
    case 's': m.tag = BackendTag::PortalSuspended; break;
    default:
        r.status = DecodeStatus::Invalid;
        return r;
    }

    if (c.bad) { r.status = DecodeStatus::Invalid; return r; }
    r.status = DecodeStatus::Ok;
    r.bytes_read = total;
    r.message = std::move(m);
    return r;
}

// ── column value helpers (text + binary) ────────────────────────────────────

// Text format: 't'/'f'. Binary format: single non-zero byte == true.
export inline std::optional<bool> parse_bool(std::span<const std::byte> v, std::int16_t format) {
    if (format == 0) {
        if (v.size() != 1) return std::nullopt;
        char c { static_cast<char>(v[0]) };
        if (c == 't' || c == 'T' || c == '1') return true;
        if (c == 'f' || c == 'F' || c == '0') return false;
        return std::nullopt;
    }
    if (v.size() != 1) return std::nullopt;
    return v[0] != std::byte { 0 };
}

// int2/int4/int8 in either text (base-10) or binary (big-endian two's complement).
export inline std::optional<std::int64_t> parse_int(std::span<const std::byte> v, std::int16_t format) {
    if (format == 0) {
        std::string s;
        s.reserve(v.size());
        for (auto b : v) s.push_back(static_cast<char>(b));
        std::int64_t out { 0 };
        auto* first { s.data() };
        auto* last { s.data() + s.size() };
        auto [ptr, ec] { std::from_chars(first, last, out) };
        if (ec != std::errc {} || ptr != last) return std::nullopt;
        return out;
    }
    if (v.size() != 2 && v.size() != 4 && v.size() != 8) return std::nullopt;
    std::uint64_t u { 0 };
    for (auto b : v) u = (u << 8) | static_cast<std::uint8_t>(b);
    // sign-extend from the byte width
    if (v.size() == 2) return static_cast<std::int64_t>(static_cast<std::int16_t>(u));
    if (v.size() == 4) return static_cast<std::int64_t>(static_cast<std::int32_t>(u));
    return static_cast<std::int64_t>(u);
}

// float4/float8 in text (base-10) or binary (IEEE-754 big-endian).
export inline std::optional<double> parse_float(std::span<const std::byte> v, std::int16_t format) {
    if (format == 0) {
        std::string s;
        s.reserve(v.size());
        for (auto b : v) s.push_back(static_cast<char>(b));
        double out { 0 };
        auto* first { s.data() };
        auto* last { s.data() + s.size() };
        auto [ptr, ec] { std::from_chars(first, last, out) };
        if (ec != std::errc {} || ptr != last) return std::nullopt;
        return out;
    }
    if (v.size() == 4) {
        std::uint32_t u { 0 };
        for (auto b : v) u = (u << 8) | static_cast<std::uint8_t>(b);
        return static_cast<double>(std::bit_cast<float>(u));
    }
    if (v.size() == 8) {
        std::uint64_t u { 0 };
        for (auto b : v) u = (u << 8) | static_cast<std::uint8_t>(b);
        return std::bit_cast<double>(u);
    }
    return std::nullopt;
}

// Decode a raw column value into a RowValue-compatible variant, dispatched by OID
// and format code. text/varchar/etc. stay as strings. RowValue lives in
// mbun.postgres.row, so return the primitive variant here to avoid a cycle.
export using WireValue = std::variant<std::monostate, bool, std::int64_t, double, std::string, std::vector<std::byte>>;

export inline WireValue decode_value(TypeId oid, std::span<const std::byte> v, std::int16_t format) {
    switch (oid) {
    case TypeId::Bool:
        if (auto b { parse_bool(v, format) }) return *b;
        return std::monostate {};
    case TypeId::Int2:
    case TypeId::Int4:
    case TypeId::Int8:
        if (auto n { parse_int(v, format) }) return *n;
        return std::monostate {};
    case TypeId::Float4:
    case TypeId::Float8:
        if (auto d { parse_float(v, format) }) return *d;
        return std::monostate {};
    case TypeId::Bytea:
        return std::vector<std::byte>(v.begin(), v.end());
    default: {
        std::string s;
        s.reserve(v.size());
        for (auto b : v) s.push_back(static_cast<char>(b));
        return s;
    }
    }
}

}  // namespace mbun::postgres
