// test_argon2.cpp — mbun.crypto.argon2 RFC 9106 §5 known-answer vectors.
//
// RFC 9106 test parameters: p=4, t=3, m=32, tagLen=32, version 0x13,
// password=32×0x01, salt=16×0x02, secret=8×0x03, ad=12×0x04. Passing all
// three variants proves the BlaMka compression, multi-lane addressing, and
// blake2b_long are all correct. Plus PHC hash/verify roundtrip (p=1).

import std;
import mbun.crypto;

namespace {

using namespace mbun::crypto;

int g_failed { 0 };

std::span<const std::uint8_t> bytes(std::string_view s) {
    return { reinterpret_cast<const std::uint8_t*>(s.data()), s.size() };
}

std::string to_hex(std::span<const std::uint8_t> b) {
    static constexpr char H[] { "0123456789abcdef" };
    std::string s;
    for (std::uint8_t v : b) {
        s.push_back(H[v >> 4]);
        s.push_back(H[v & 0x0f]);
    }
    return s;
}

void check_vector(std::string_view name, argon2::Type type, std::string_view want) {
    const std::vector<std::uint8_t> pw(32, 0x01);
    const std::vector<std::uint8_t> salt(16, 0x02);
    const std::vector<std::uint8_t> secret(8, 0x03);
    const std::vector<std::uint8_t> ad(12, 0x04);
    const std::vector<std::uint8_t> tag { argon2::raw(type, 32, 3, 4, 32, pw, salt, secret, ad) };
    const std::string got { to_hex(tag) };
    if (got != want) {
        ++g_failed;
        std::println("FAIL argon2 {} vector: got {} want {}", name, got, want);
    }
}

}  // namespace

int main() {
    // RFC 9106 §5.1–5.3.
    check_vector("argon2d", argon2::Type::D,
                 "512b391b6f1162975371d3091973429"
                 "4f868e3be3984f3c1a13a4db9fabe4acb");
    check_vector("argon2i", argon2::Type::I,
                 "c814d9d1dc7f37aa13f0d77f2494bda1"
                 "c8de6b016dd388d29952a4c4672b6ce8");
    check_vector("argon2id", argon2::Type::ID,
                 "0d640df58d78766c08c037a34a8b53c9"
                 "d01ef0452d75b65eb52520e96b01e659");

    // PHC roundtrip with small params (p=1).
    {
        const std::string h { argon2::hash_encoded(argon2::Type::ID, 8, 1, 1, bytes("hey"),
                                                   bytes("0123456789abcdef")) };
        if (!h.starts_with("$argon2id$v=19$m=8,t=1,p=1$")) {
            ++g_failed;
            std::println("FAIL argon2 PHC encode: {}", h);
        }
        if (argon2::verify(bytes("hey"), h) != argon2::VerifyResult::Match) {
            ++g_failed;
            std::println("FAIL argon2 verify match");
        }
        if (argon2::verify(bytes("hello"), h) != argon2::VerifyResult::Mismatch) {
            ++g_failed;
            std::println("FAIL argon2 verify mismatch");
        }
    }

    // WeakParameters ceilings (t>2^16, m>2^22, p>64).
    {
        const std::string base {
            "$argon2id$v=19$m=8,t=1,p=1$MDEyMzQ1Njc4OWFiY2RlZg$"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaao"
        };
        auto tamper = [&](std::string_view from, std::string_view to) {
            std::string s { base };
            const std::size_t p { s.find(from) };
            s.replace(p, from.size(), to);
            return s;
        };
        if (argon2::verify(bytes("x"), tamper("t=1", "t=100000"))
            != argon2::VerifyResult::WeakParameters) {
            ++g_failed;
            std::println("FAIL argon2 weak t");
        }
        if (argon2::verify(bytes("x"), tamper("m=8", "m=4294967294"))
            != argon2::VerifyResult::WeakParameters) {
            ++g_failed;
            std::println("FAIL argon2 weak m");
        }
        if (argon2::verify(bytes("x"), tamper("p=1", "p=65"))
            != argon2::VerifyResult::WeakParameters) {
            ++g_failed;
            std::println("FAIL argon2 weak p");
        }
    }

    if (g_failed == 0) {
        std::println("argon2: all vectors passed");
        return 0;
    }
    std::println("argon2: {} FAILED", g_failed);
    return 1;
}
