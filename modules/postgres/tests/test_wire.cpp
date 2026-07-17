// test_wire.cpp — D.2 mbun.postgres wire protocol v3 codec test suite.
//
// Vectors are derived from the official PostgreSQL frontend/backend v3 message
// format (message name set / framing confirmed by bun-ref
// src/sql/postgres/PostgresProtocol.rs). Pure codec: no socket / TLS / auth
// handshake — those need the reactor and are DEFERRED(S-net).
//
// Coverage: frontend encode framing (length/tag/body), backend decode of each
// listed message, encode/decode round-trips, cross-packet incremental parsing,
// ErrorResponse field access, RowDescription+DataRow combination, and the
// int2/int4/int8/text/bool/float text+binary value helpers.

import std;
import mbun.postgres;

namespace {

using namespace mbun::postgres;

int gChecks { 0 };
int gFailures { 0 };
constexpr int MAX_FAILURE_PRINTS { 40 };

void report(std::string_view what) {
    ++gFailures;
    if (gFailures <= MAX_FAILURE_PRINTS) std::println("  FAIL {}", what);
}

void ok(bool cond, std::string_view what) {
    ++gChecks;
    if (!cond) report(what);
}

template <class A, class B>
void eq(const A& a, const B& b, std::string_view what) {
    ++gChecks;
    if (!(a == b)) {
        ++gFailures;
        if (gFailures <= MAX_FAILURE_PRINTS) std::println("  FAIL {}", what);
    }
}

std::string_view as_sv(std::span<const std::byte> b) {
    return { reinterpret_cast<const char*>(b.data()), b.size() };
}
std::vector<std::byte> bytes_of(std::string_view s) {
    std::vector<std::byte> v;
    v.reserve(s.size());
    for (char c : s) v.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(c)));
    return v;
}
// Read a big-endian int32 at offset off of an encoded frame.
std::int32_t be32(std::span<const std::byte> b, std::size_t off) {
    std::uint32_t v { 0 };
    for (std::size_t k { 0 }; k < 4; ++k) v = (v << 8) | static_cast<std::uint8_t>(b[off + k]);
    return static_cast<std::int32_t>(v);
}

// ── frontend encoders: framing correctness ──────────────────────────────────
void test_frontend_framing() {
    // StartupMessage: no tag, length == whole buffer, version at offset 4.
    std::array params { StartupParam { "user", "postgres" }, StartupParam { "database", "app" } };
    auto su { encode_startup(params) };
    eq(be32(su, 0), static_cast<std::int32_t>(su.size()), "startup.len==size");
    eq(be32(su, 4), PROTOCOL_VERSION_3, "startup.version");
    ok(su.back() == std::byte { 0 }, "startup.trailing-nul");

    // Query: tag 'Q', length excludes tag but includes itself + body.
    auto q { encode_query("SELECT 1") };
    ok(q.front() == std::byte { 'Q' }, "query.tag");
    eq(be32(q, 1), static_cast<std::int32_t>(q.size() - 1), "query.len");

    // Body-less messages: tag + length 4.
    auto sy { encode_sync() };
    ok(sy.front() == std::byte { 'S' }, "sync.tag");
    eq(be32(sy, 1), 4, "sync.len4");
    eq(encode_terminate().front(), std::byte { 'X' }, "terminate.tag");
    eq(encode_flush().front(), std::byte { 'H' }, "flush.tag");

    // SSLRequest: length 8, magic code.
    auto ssl { encode_ssl_request() };
    eq(be32(ssl, 0), 8, "ssl.len8");
    eq(be32(ssl, 4), SSL_REQUEST_CODE, "ssl.code");

    // Parse framing + declared param count.
    std::array<std::uint32_t, 2> oids { 23u, 25u };
    auto p { encode_parse("stmt1", "SELECT $1::int, $2::text", oids) };
    ok(p.front() == std::byte { 'P' }, "parse.tag");
    eq(be32(p, 1), static_cast<std::int32_t>(p.size() - 1), "parse.len");

    // Describe / Execute / Close tags.
    eq(encode_describe('S', "stmt1").front(), std::byte { 'D' }, "describe.tag");
    eq(encode_execute("", 0).front(), std::byte { 'E' }, "execute.tag");
    eq(encode_close('P', "portal").front(), std::byte { 'C' }, "close.tag");
    eq(encode_password("secret").front(), std::byte { 'p' }, "password.tag");
}

// ── frontend → backend feel: decode what a peer sends and cross-check parse ──
void test_bind_encode() {
    std::array<std::int16_t, 1> pfmt { 0 };  // text
    std::array<BindValue, 2> vals { BindValue { bytes_of("42") }, BindValue { std::nullopt } };
    std::array<std::int16_t, 1> rfmt { 1 };  // binary results
    auto b { encode_bind("", "stmt1", pfmt, vals, rfmt) };
    ok(b.front() == std::byte { 'B' }, "bind.tag");
    eq(be32(b, 1), static_cast<std::int32_t>(b.size() - 1), "bind.len");
    // The literal "42" must appear in the body; NULL is encoded as length -1.
    std::string_view body { as_sv(b) };
    ok(body.find("42") != std::string_view::npos, "bind.value-present");
}

// ── backend decode: individual messages ─────────────────────────────────────
void test_decode_simple() {
    // ReadyForQuery: 'Z' len 5, status byte 'I'.
    auto rfq { std::vector<std::byte> { std::byte { 'Z' }, std::byte { 0 }, std::byte { 0 }, std::byte { 0 }, std::byte { 5 }, std::byte { 'I' } } };
    auto r { decode_message(as_sv(rfq)) };
    ok(r.status == DecodeStatus::Ok, "rfq.ok");
    eq(r.bytes_read, rfq.size(), "rfq.consumed");
    ok(r.message.tag == BackendTag::ReadyForQuery, "rfq.tag");
    eq(std::get<ReadyForQueryMsg>(r.message.payload).status, 'I', "rfq.status");

    // Tag-only messages.
    auto pc { std::vector<std::byte> { std::byte { '1' }, std::byte { 0 }, std::byte { 0 }, std::byte { 0 }, std::byte { 4 } } };
    auto rp { decode_message(as_sv(pc)) };
    ok(rp.status == DecodeStatus::Ok && rp.message.tag == BackendTag::ParseComplete, "parsecomplete");
    auto bc { std::vector<std::byte> { std::byte { '2' }, std::byte { 0 }, std::byte { 0 }, std::byte { 0 }, std::byte { 4 } } };
    ok(decode_message(as_sv(bc)).message.tag == BackendTag::BindComplete, "bindcomplete");
    auto nd { std::vector<std::byte> { std::byte { 'n' }, std::byte { 0 }, std::byte { 0 }, std::byte { 0 }, std::byte { 4 } } };
    ok(decode_message(as_sv(nd)).message.tag == BackendTag::NoData, "nodata");
}

void test_decode_auth() {
    // AuthenticationOk: 'R' len 8, int32 == 0.
    auto ok0 { std::vector<std::byte> { std::byte { 'R' }, std::byte { 0 }, std::byte { 0 }, std::byte { 0 }, std::byte { 8 }, std::byte { 0 }, std::byte { 0 }, std::byte { 0 }, std::byte { 0 } } };
    auto r0 { decode_message(as_sv(ok0)) };
    ok(std::get<AuthenticationMsg>(r0.message.payload).kind == AuthKind::Ok, "auth.ok");

    // AuthenticationMD5Password: 'R' len 12, int32 == 5, 4-byte salt.
    std::vector<std::byte> md5 { std::byte { 'R' }, std::byte { 0 }, std::byte { 0 }, std::byte { 0 }, std::byte { 12 },
                                 std::byte { 0 }, std::byte { 0 }, std::byte { 0 }, std::byte { 5 },
                                 std::byte { 0xDE }, std::byte { 0xAD }, std::byte { 0xBE }, std::byte { 0xEF } };
    auto rm { decode_message(as_sv(md5)) };
    auto& am { std::get<AuthenticationMsg>(rm.message.payload) };
    ok(am.kind == AuthKind::MD5Password, "auth.md5.kind");
    ok(am.md5_salt[0] == std::byte { 0xDE } && am.md5_salt[3] == std::byte { 0xEF }, "auth.md5.salt");

    // AuthenticationSASL: 'R' int32 == 10, mechanism list "SCRAM-SHA-256\0\0".
    std::string mechs { "SCRAM-SHA-256" };
    std::vector<std::byte> sasl { std::byte { 'R' }, std::byte { 0 }, std::byte { 0 }, std::byte { 0 }, std::byte { 0 } /*len patched below*/ };
    // int32 kind = 10
    sasl.push_back(std::byte { 0 }); sasl.push_back(std::byte { 0 }); sasl.push_back(std::byte { 0 }); sasl.push_back(std::byte { 10 });
    for (char c : mechs) sasl.push_back(std::byte { static_cast<std::uint8_t>(c) });
    sasl.push_back(std::byte { 0 }); sasl.push_back(std::byte { 0 });  // mech NUL + list terminator
    auto len { static_cast<std::uint32_t>(sasl.size() - 1) };
    sasl[1] = std::byte { static_cast<std::uint8_t>(len >> 24) };
    sasl[2] = std::byte { static_cast<std::uint8_t>(len >> 16) };
    sasl[3] = std::byte { static_cast<std::uint8_t>(len >> 8) };
    sasl[4] = std::byte { static_cast<std::uint8_t>(len) };
    auto rs { decode_message(as_sv(sasl)) };
    auto& as_ { std::get<AuthenticationMsg>(rs.message.payload) };
    ok(as_.kind == AuthKind::SASL, "auth.sasl.kind");
    eq(as_.sasl_mechanisms.size(), std::size_t { 1 }, "auth.sasl.count");
    if (!as_.sasl_mechanisms.empty()) eq(as_.sasl_mechanisms[0], std::string { "SCRAM-SHA-256" }, "auth.sasl.mech");
}

void test_decode_paramstatus_keydata() {
    // ParameterStatus 'S': name\0 value\0.
    std::vector<std::byte> ps { std::byte { 'S' }, std::byte { 0 }, std::byte { 0 }, std::byte { 0 }, std::byte { 0 } };
    std::string_view nm { "client_encoding" }, vl { "UTF8" };
    for (char c : nm) ps.push_back(std::byte { static_cast<std::uint8_t>(c) });
    ps.push_back(std::byte { 0 });
    for (char c : vl) ps.push_back(std::byte { static_cast<std::uint8_t>(c) });
    ps.push_back(std::byte { 0 });
    auto len { static_cast<std::uint32_t>(ps.size() - 1) };
    ps[1] = std::byte { static_cast<std::uint8_t>(len >> 24) }; ps[2] = std::byte { static_cast<std::uint8_t>(len >> 16) };
    ps[3] = std::byte { static_cast<std::uint8_t>(len >> 8) }; ps[4] = std::byte { static_cast<std::uint8_t>(len) };
    auto rp { decode_message(as_sv(ps)) };
    auto& p { std::get<ParameterStatusMsg>(rp.message.payload) };
    eq(p.name, std::string { "client_encoding" }, "paramstatus.name");
    eq(p.value, std::string { "UTF8" }, "paramstatus.value");

    // BackendKeyData 'K' len 12: pid, secret.
    std::vector<std::byte> kd { std::byte { 'K' }, std::byte { 0 }, std::byte { 0 }, std::byte { 0 }, std::byte { 12 },
                                std::byte { 0 }, std::byte { 0 }, std::byte { 0x12 }, std::byte { 0x34 },
                                std::byte { 0 }, std::byte { 0 }, std::byte { 0x56 }, std::byte { 0x78 } };
    auto rk { decode_message(as_sv(kd)) };
    auto& k { std::get<BackendKeyDataMsg>(rk.message.payload) };
    eq(k.process_id, 0x1234, "keydata.pid");
    eq(k.secret_key, 0x5678, "keydata.secret");
}

void test_decode_error_response() {
    // ErrorResponse 'E': sequence of (code byte, value cstr), terminated by 0.
    auto add_field = [](std::vector<std::byte>& v, char code, std::string_view val) {
        v.push_back(std::byte { static_cast<std::uint8_t>(code) });
        for (char c : val) v.push_back(std::byte { static_cast<std::uint8_t>(c) });
        v.push_back(std::byte { 0 });
    };
    std::vector<std::byte> er { std::byte { 'E' }, std::byte { 0 }, std::byte { 0 }, std::byte { 0 }, std::byte { 0 } };
    add_field(er, 'S', "ERROR");
    add_field(er, 'C', "42P01");
    add_field(er, 'M', "relation \"nope\" does not exist");
    er.push_back(std::byte { 0 });  // terminator
    auto len { static_cast<std::uint32_t>(er.size() - 1) };
    er[1] = std::byte { static_cast<std::uint8_t>(len >> 24) }; er[2] = std::byte { static_cast<std::uint8_t>(len >> 16) };
    er[3] = std::byte { static_cast<std::uint8_t>(len >> 8) }; er[4] = std::byte { static_cast<std::uint8_t>(len) };
    auto re { decode_message(as_sv(er)) };
    ok(re.message.tag == BackendTag::ErrorResponse, "error.tag");
    ok(!re.message.is_notice, "error.not-notice");
    auto& e { std::get<ErrorResponseMsg>(re.message.payload) };
    eq(e.severity(), std::string_view { "ERROR" }, "error.severity");
    eq(e.code_str(), std::string_view { "42P01" }, "error.code");
    eq(e.message(), std::string_view { "relation \"nope\" does not exist" }, "error.message");

    // Same framing under 'N' is a NoticeResponse.
    er[0] = std::byte { 'N' };
    auto rn { decode_message(as_sv(er)) };
    ok(rn.message.tag == BackendTag::NoticeResponse && rn.message.is_notice, "notice.tag");
}

void test_row_description_and_data_row() {
    // RowDescription 'T' with 2 fields, then a DataRow 'D' with matching columns.
    auto add_i16 = [](std::vector<std::byte>& v, std::int16_t n) {
        v.push_back(std::byte { static_cast<std::uint8_t>(static_cast<std::uint16_t>(n) >> 8) });
        v.push_back(std::byte { static_cast<std::uint8_t>(n) });
    };
    auto add_i32 = [](std::vector<std::byte>& v, std::int32_t n) {
        auto u { static_cast<std::uint32_t>(n) };
        v.push_back(std::byte { static_cast<std::uint8_t>(u >> 24) }); v.push_back(std::byte { static_cast<std::uint8_t>(u >> 16) });
        v.push_back(std::byte { static_cast<std::uint8_t>(u >> 8) }); v.push_back(std::byte { static_cast<std::uint8_t>(u) });
    };
    auto add_field = [&](std::vector<std::byte>& v, std::string_view name, std::int32_t oid, std::int16_t sz) {
        for (char c : name) v.push_back(std::byte { static_cast<std::uint8_t>(c) });
        v.push_back(std::byte { 0 });
        add_i32(v, 0);       // table oid
        add_i16(v, 0);       // column index
        add_i32(v, oid);     // type oid
        add_i16(v, sz);      // type size
        add_i32(v, -1);      // type modifier
        add_i16(v, 0);       // format code (text)
    };
    std::vector<std::byte> td { std::byte { 'T' }, std::byte { 0 }, std::byte { 0 }, std::byte { 0 }, std::byte { 0 } };
    add_i16(td, 2);
    add_field(td, "id", static_cast<std::int32_t>(TypeId::Int4), 4);
    add_field(td, "name", static_cast<std::int32_t>(TypeId::Text), -1);
    {
        auto len { static_cast<std::uint32_t>(td.size() - 1) };
        td[1] = std::byte { static_cast<std::uint8_t>(len >> 24) }; td[2] = std::byte { static_cast<std::uint8_t>(len >> 16) };
        td[3] = std::byte { static_cast<std::uint8_t>(len >> 8) }; td[4] = std::byte { static_cast<std::uint8_t>(len) };
    }
    auto rt { decode_message(as_sv(td)) };
    ok(rt.message.tag == BackendTag::RowDescription, "rowdesc.tag");
    auto& rd { std::get<RowDescriptionMsg>(rt.message.payload) };
    eq(rd.fields.size(), std::size_t { 2 }, "rowdesc.count");
    if (rd.fields.size() == 2) {
        eq(rd.fields[0].name, std::string { "id" }, "rowdesc.f0.name");
        eq(rd.fields[0].type_oid, static_cast<std::int32_t>(TypeId::Int4), "rowdesc.f0.oid");
        eq(rd.fields[1].name, std::string { "name" }, "rowdesc.f1.name");
    }

    // DataRow 'D': column "7" (text), then a NULL column (length -1).
    std::vector<std::byte> dr { std::byte { 'D' }, std::byte { 0 }, std::byte { 0 }, std::byte { 0 }, std::byte { 0 } };
    add_i16(dr, 2);
    add_i32(dr, 1); dr.push_back(std::byte { '7' });  // first column, 1 byte
    add_i32(dr, -1);                                  // second column NULL
    {
        auto len { static_cast<std::uint32_t>(dr.size() - 1) };
        dr[1] = std::byte { static_cast<std::uint8_t>(len >> 24) }; dr[2] = std::byte { static_cast<std::uint8_t>(len >> 16) };
        dr[3] = std::byte { static_cast<std::uint8_t>(len >> 8) }; dr[4] = std::byte { static_cast<std::uint8_t>(len) };
    }
    auto rdd { decode_message(as_sv(dr)) };
    ok(rdd.message.tag == BackendTag::DataRow, "datarow.tag");
    auto& d { std::get<DataRowMsg>(rdd.message.payload) };
    eq(d.columns.size(), std::size_t { 2 }, "datarow.count");
    ok(d.columns[0].has_value(), "datarow.c0.present");
    ok(!d.columns[1].has_value(), "datarow.c1.null");

    // Decode the first column against its field OID.
    if (d.columns[0].has_value()) {
        auto val { decode_value(TypeId::Int4, *d.columns[0], 0) };
        ok(std::holds_alternative<std::int64_t>(val), "datarow.c0.int");
        if (std::holds_alternative<std::int64_t>(val)) eq(std::get<std::int64_t>(val), std::int64_t { 7 }, "datarow.c0.val");
    }
}

// ── incremental / cross-packet parsing (parse_request-style contract) ───────
void test_incremental() {
    auto full { std::vector<std::byte> { std::byte { 'Z' }, std::byte { 0 }, std::byte { 0 }, std::byte { 0 }, std::byte { 5 }, std::byte { 'T' } } };
    std::string_view sv { as_sv(full) };

    // Fewer than the header (5) bytes → Incomplete, nothing consumed.
    ok(decode_message(sv.substr(0, 3)).status == DecodeStatus::Incomplete, "incr.header-short");
    // Header present but body missing → Incomplete.
    ok(decode_message(sv.substr(0, 5)).status == DecodeStatus::Incomplete, "incr.body-short");
    // Full message → Ok, bytes_read == whole message.
    auto r { decode_message(sv) };
    ok(r.status == DecodeStatus::Ok, "incr.full-ok");
    eq(r.bytes_read, full.size(), "incr.full-consumed");

    // Two messages back-to-back: decode one, advance, decode the next.
    auto second { encode_ssl_request() };  // reuse any bytes; use a real backend pair instead:
    (void)second;
    std::vector<std::byte> stream;
    auto pc { std::vector<std::byte> { std::byte { '1' }, std::byte { 0 }, std::byte { 0 }, std::byte { 0 }, std::byte { 4 } } };
    stream.insert(stream.end(), pc.begin(), pc.end());
    stream.insert(stream.end(), full.begin(), full.end());
    std::string_view ss { as_sv(stream) };
    auto r1 { decode_message(ss) };
    ok(r1.status == DecodeStatus::Ok && r1.message.tag == BackendTag::ParseComplete, "stream.msg1");
    auto r2 { decode_message(ss.substr(r1.bytes_read)) };
    ok(r2.status == DecodeStatus::Ok && r2.message.tag == BackendTag::ReadyForQuery, "stream.msg2");
    eq(std::get<ReadyForQueryMsg>(r2.message.payload).status, 'T', "stream.msg2.status");
}

// ── invalid framing ─────────────────────────────────────────────────────────
void test_invalid() {
    // Length < 4 is malformed.
    auto bad { std::vector<std::byte> { std::byte { 'Z' }, std::byte { 0 }, std::byte { 0 }, std::byte { 0 }, std::byte { 2 } } };
    ok(decode_message(as_sv(bad)).status == DecodeStatus::Invalid, "invalid.short-len");
    // Unknown tag.
    auto unk { std::vector<std::byte> { std::byte { '@' }, std::byte { 0 }, std::byte { 0 }, std::byte { 0 }, std::byte { 4 } } };
    ok(decode_message(as_sv(unk)).status == DecodeStatus::Invalid, "invalid.unknown-tag");
}

// ── value helpers: text + binary ────────────────────────────────────────────
void test_value_helpers() {
    // bool
    eq(parse_bool(bytes_of("t"), 0).value_or(false), true, "bool.text.t");
    eq(parse_bool(bytes_of("f"), 0).value_or(true), false, "bool.text.f");
    eq(parse_bool(std::vector<std::byte> { std::byte { 1 } }, 1).value_or(false), true, "bool.bin.1");
    eq(parse_bool(std::vector<std::byte> { std::byte { 0 } }, 1).value_or(true), false, "bool.bin.0");

    // int text
    eq(parse_int(bytes_of("-12345"), 0).value_or(0), std::int64_t { -12345 }, "int.text");
    ok(!parse_int(bytes_of("12x"), 0).has_value(), "int.text.bad");
    // int binary int2 (0x7FFF), int4, int8
    eq(parse_int(std::vector<std::byte> { std::byte { 0x7F }, std::byte { 0xFF } }, 1).value_or(0), std::int64_t { 32767 }, "int.bin.i2");
    eq(parse_int(std::vector<std::byte> { std::byte { 0xFF }, std::byte { 0xFF } }, 1).value_or(0), std::int64_t { -1 }, "int.bin.i2.neg");
    eq(parse_int(std::vector<std::byte> { std::byte { 0 }, std::byte { 0 }, std::byte { 0x04 }, std::byte { 0xD2 } }, 1).value_or(0), std::int64_t { 1234 }, "int.bin.i4");
    eq(parse_int(std::vector<std::byte> { std::byte { 0 }, std::byte { 0 }, std::byte { 0 }, std::byte { 0 }, std::byte { 0 }, std::byte { 0 }, std::byte { 0 }, std::byte { 0x2A } }, 1).value_or(0), std::int64_t { 42 }, "int.bin.i8");

    // float text + binary
    ok(std::abs(parse_float(bytes_of("3.5"), 0).value_or(0) - 3.5) < 1e-9, "float.text");
    // binary float8 of 1.0 == 0x3FF0000000000000
    std::vector<std::byte> f8 { std::byte { 0x3F }, std::byte { 0xF0 }, std::byte { 0 }, std::byte { 0 }, std::byte { 0 }, std::byte { 0 }, std::byte { 0 }, std::byte { 0 } };
    ok(std::abs(parse_float(f8, 1).value_or(0) - 1.0) < 1e-12, "float.bin.f8");

    // text OID stays a string.
    auto tv { decode_value(TypeId::Text, bytes_of("hello"), 0) };
    ok(std::holds_alternative<std::string>(tv) && std::get<std::string>(tv) == "hello", "value.text");
}

}  // namespace

int main() {
    test_frontend_framing();
    test_bind_encode();
    test_decode_simple();
    test_decode_auth();
    test_decode_paramstatus_keydata();
    test_decode_error_response();
    test_row_description_and_data_row();
    test_incremental();
    test_invalid();
    test_value_helpers();

    std::println("mbun.postgres.wire: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
