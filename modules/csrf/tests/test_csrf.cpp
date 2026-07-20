// Engine-level tests for mbun.csrf, off the JS engine. The expected behaviour
// mirrors bun's CSRF suite (compat/bun/test/js/bun/util/csrf.test.ts) and the
// reference implementation (compat/bun/src/csrf/lib.rs) where they translate 1:1.
import std;
import mbun.csrf;

namespace {

using namespace mbun::csrf;

int checks { 0 };
int failures { 0 };

void check(bool value, std::string_view label) {
    ++checks;
    if (!value) {
        ++failures;
        std::println("FAIL: {}", label);
    }
}

void check_eq(std::string_view actual, std::string_view expected, std::string_view label) {
    ++checks;
    if (actual != expected) {
        ++failures;
        std::println("FAIL: {}\n  expected: {}\n  actual:   {}", label, expected, actual);
    }
}

constexpr std::string_view SECRET { "this-is-my-super-secure-secret-key" };

// A fixed, non-zero clock: generate()/verify() treat nowMs == 0 as "read the
// system clock", so every deterministic case pins an explicit timestamp.
constexpr std::uint64_t T0 { 1'700'000'000'000ULL };
constexpr std::uint64_t DAY_MS { 24ULL * 60 * 60 * 1000 };

std::string gen(std::string_view secret, GenerateOptions options, std::string_view label) {
    auto token { generate(secret, options) };
    ++checks;
    if (!token) {
        ++failures;
        std::println("FAIL: generate failed for {} (error {})", label, static_cast<int>(token.error()));
        return {};
    }
    return *token;
}

bool ok(std::string_view token, std::string_view secret, VerifyOptions options,
        std::string_view label) {
    auto result { verify(token, secret, options) };
    ++checks;
    if (!result) {
        ++failures;
        std::println("FAIL: verify errored for {} (error {})", label, static_cast<int>(result.error()));
        return false;
    }
    return *result;
}

// Flips one character of the token at `index` so the decoded bytes are
// guaranteed to differ (the last base64 character carries dropped bits, so
// tampering must happen away from the tail).
std::string flip_at(std::string token, std::size_t index) {
    token[index] = token[index] == 'A' ? 'B' : 'A';
    return token;
}

bool all_of_charset(std::string_view text, std::string_view charset) {
    return !text.empty()
        && std::ranges::all_of(text, [&](char c) { return charset.find(c) != std::string_view::npos; });
}

void test_generate_defaults() {
    const auto token { gen(SECRET, { .nowMs = T0 }, "default options") };
    check(!token.empty(), "default token is non-empty");
    // 8 timestamp + 16 nonce + 8 expiresIn + 32 HMAC-SHA256 = 64 bytes.
    check(token.size() == 86, "default token is 86 base64url chars (64 raw bytes)");
    check(all_of_charset(token, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-"),
          "default token matches /^[A-Za-z0-9_-]+$/");

    // The nonce makes every token unique even at a pinned timestamp.
    const auto other { gen(SECRET, { .nowMs = T0 }, "second default token") };
    check(token != other, "two tokens at the same timestamp differ (nonce entropy)");
    check(ok(other, SECRET, { .nowMs = T0 }, "second default token"), "second token verifies too");
}

void test_roundtrip() {
    const auto token { gen(SECRET, { .nowMs = T0 }, "round-trip") };
    check(ok(token, SECRET, { .nowMs = T0 }, "round-trip"), "generate/verify round-trip is valid");
    // Still valid well inside the default 24h window.
    check(ok(token, SECRET, { .nowMs = T0 + 60'000 }, "round-trip later"),
          "token still valid one minute later");
    // Surrounding whitespace is trimmed before decoding (bun trims "\r\n\t \x0b").
    check(ok(" \t" + token + "\r\n", SECRET, { .nowMs = T0 }, "whitespace-padded"),
          "leading/trailing whitespace is trimmed");
}

void test_tampered_token() {
    const auto token { gen(SECRET, { .nowMs = T0 }, "tamper base") };
    // Signature region: raw bytes 32.. → base64 chars 43...
    check(!ok(flip_at(token, 60), SECRET, { .nowMs = T0 }, "tampered signature"),
          "tampered signature is rejected");
    // Payload region: nonce bytes 8..24 → base64 chars ~11..32.
    check(!ok(flip_at(token, 20), SECRET, { .nowMs = T0 }, "tampered nonce"),
          "tampered payload/nonce is rejected");
    // bun's own mutation: replace the last five characters.
    // The final base64 character only carries bits the decoder drops, so the
    // replacement is anchored on a character that provably changes a byte.
    std::string replaced { token.substr(0, token.size() - 5) + "XXXXX" };
    replaced[replaced.size() - 5] = token[token.size() - 5] == 'X' ? 'Y' : 'X';
    check(!ok(replaced, SECRET, { .nowMs = T0 }, "suffix replaced"),
          "token with last 5 chars replaced is rejected");
    // Dropping bytes shortens the token below the 64-byte minimum.
    check(!ok(token.substr(0, 40), SECRET, { .nowMs = T0 }, "truncated"),
          "truncated token is rejected");
    // Extra trailing data changes the signature length.
    check(!ok(token + "AAAA", SECRET, { .nowMs = T0 }, "extended"),
          "token with appended data is rejected");
}

void test_wrong_secret() {
    const auto token { gen(SECRET, { .nowMs = T0 }, "wrong-secret base") };
    check(!ok(token, "wrong-secret", { .nowMs = T0 }, "wrong secret"),
          "verification is sensitive to the secret");
    // One-character difference must fail too.
    check(!ok(token, "this-is-my-super-secure-secret-keY", { .nowMs = T0 }, "off-by-one secret"),
          "single-character secret difference is rejected");
    // A secret longer than the HMAC block size is hashed down first; it must
    // still round-trip and still reject a different long secret.
    const std::string longSecret(200, 'x');
    std::string otherLong(200, 'x');
    otherLong.back() = 'y';
    const auto longToken { gen(longSecret, { .nowMs = T0 }, "long secret") };
    check(ok(longToken, longSecret, { .nowMs = T0 }, "long secret"),
          "secret longer than the HMAC block size round-trips");
    check(!ok(longToken, otherLong, { .nowMs = T0 }, "other long secret"),
          "different long secret is rejected");
}

void test_expiry() {
    // expiresIn honoured: 1ms token is dead 10ms later (bun: "tokens expire").
    const auto shortToken { gen(SECRET, { .expiresInMs = 1, .nowMs = T0 }, "1ms expiry") };
    check(ok(shortToken, SECRET, { .nowMs = T0 }, "1ms expiry, immediate"),
          "1ms token is valid at issue time");
    check(!ok(shortToken, SECRET, { .nowMs = T0 + 10 }, "1ms expiry, later"),
          "1ms token is expired 10ms later");

    // Exactly at the boundary is still valid (now > timestamp + expiresIn).
    const auto boundary { gen(SECRET, { .expiresInMs = 100, .nowMs = T0 }, "boundary") };
    check(ok(boundary, SECRET, { .nowMs = T0 + 100 }, "boundary, at edge"),
          "token is valid exactly at timestamp + expiresIn");
    check(!ok(boundary, SECRET, { .nowMs = T0 + 101 }, "boundary, past edge"),
          "token is invalid one ms past timestamp + expiresIn");

    // maxAge honoured independently of the token's own expiry (bun:
    // "verification respects maxAge parameter").
    const auto normal { gen(SECRET, { .nowMs = T0 }, "maxAge base") };
    check(ok(normal, SECRET, { .maxAgeMs = 1, .nowMs = T0 }, "maxAge 1, immediate"),
          "maxAge=1 accepts a token issued now");
    check(!ok(normal, SECRET, { .maxAgeMs = 1, .nowMs = T0 + 10 }, "maxAge 1, later"),
          "maxAge=1 rejects a 10ms-old token with a 24h expiry");
    check(!ok(normal, SECRET, { .nowMs = T0 + DAY_MS + 1 }, "past default window"),
          "default 24h window rejects a day-old token");

    // expiresIn == 0 disables the token-side expiry; maxAge == 0 disables the
    // caller-side one. With both off, an ancient token still verifies.
    const auto eternal { gen(SECRET, { .expiresInMs = 0, .nowMs = T0 }, "no expiry") };
    check(ok(eternal, SECRET, { .maxAgeMs = 0, .nowMs = T0 + 10 * DAY_MS }, "no expiry"),
          "expiresIn=0 with maxAge=0 never expires");
    check(!ok(eternal, SECRET, { .maxAgeMs = 1000, .nowMs = T0 + 10 * DAY_MS }, "no expiry, maxAge"),
          "expiresIn=0 still obeys the caller's maxAge");
}

void test_encodings() {
    const auto base64Token { gen(SECRET, { .encoding = TokenFormat::Base64, .nowMs = T0 }, "base64") };
    const auto base64UrlToken { gen(SECRET, { .encoding = TokenFormat::Base64Url, .nowMs = T0 }, "base64url") };
    const auto hexToken { gen(SECRET, { .encoding = TokenFormat::Hex, .nowMs = T0 }, "hex") };

    check(all_of_charset(base64Token, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/="),
          "base64 token matches /^[A-Za-z0-9+/]+={0,2}$/");
    check(all_of_charset(hexToken, "0123456789abcdef"), "hex token matches /^[0-9a-f]+$/");
    check(hexToken.size() == 128, "hex token is 128 chars (64 raw bytes)");

    check(ok(base64Token, SECRET, { .encoding = TokenFormat::Base64, .nowMs = T0 }, "base64"),
          "base64 token round-trips");
    check(ok(base64UrlToken, SECRET, { .encoding = TokenFormat::Base64Url, .nowMs = T0 }, "base64url"),
          "base64url token round-trips");
    check(ok(hexToken, SECRET, { .encoding = TokenFormat::Hex, .nowMs = T0 }, "hex"),
          "hex token round-trips");

    // bun: "handle bad decoding" — a hex token decoded as base64url is garbage
    // and must not verify.
    check(!ok(hexToken, SECRET, { .encoding = TokenFormat::Base64Url, .nowMs = T0 }, "hex as base64url"),
          "hex token does not verify under the default base64url decoding");
    // ...and the mirror case: a base64url token read as hex.
    check(!ok(base64UrlToken, SECRET, { .encoding = TokenFormat::Hex, .nowMs = T0 }, "base64url as hex"),
          "base64url token does not verify under hex decoding");
}

void test_algorithms() {
    struct Case { Algorithm algorithm; std::string_view name; std::size_t digestBytes; };
    const std::array<Case, 4> cases {
        Case { Algorithm::Sha256, "sha256", 32 },
        Case { Algorithm::Sha384, "sha384", 48 },
        Case { Algorithm::Sha512, "sha512", 64 },
        Case { Algorithm::Sha512_256, "sha512-256", 32 },
    };
    for (const auto& c : cases) {
        const auto token { gen(SECRET, { .encoding = TokenFormat::Hex, .algorithm = c.algorithm,
                                         .nowMs = T0 }, c.name) };
        check(token.size() == (32 + c.digestBytes) * 2,
              std::format("{} token is 32-byte payload + {}-byte digest", c.name, c.digestBytes));
        check(ok(token, SECRET, { .encoding = TokenFormat::Hex, .algorithm = c.algorithm, .nowMs = T0 },
                 c.name),
              std::format("{} token round-trips", c.name));
        check(!ok(token, "wrong-secret", { .encoding = TokenFormat::Hex, .algorithm = c.algorithm,
                                           .nowMs = T0 }, c.name),
              std::format("{} token rejects the wrong secret", c.name));
    }
    // Verifying under a different algorithm must fail (digest length and/or
    // value differ).
    const auto sha512Token { gen(SECRET, { .algorithm = Algorithm::Sha512, .nowMs = T0 }, "sha512 x-check") };
    check(!ok(sha512Token, SECRET, { .algorithm = Algorithm::Sha256, .nowMs = T0 }, "sha512 as sha256"),
          "sha512 token does not verify as sha256");
    const auto sha256Token { gen(SECRET, { .algorithm = Algorithm::Sha256, .nowMs = T0 }, "sha256 x-check") };
    check(!ok(sha256Token, SECRET, { .algorithm = Algorithm::Sha512_256, .nowMs = T0 }, "sha256 as sha512-256"),
          "sha256 token does not verify as sha512-256 (same length, different digest)");
}

void test_session_id() {
    constexpr std::string_view SESSION { "user-session-1" };
    const auto bound { gen(SECRET, { .sessionId = SESSION, .nowMs = T0 }, "session-bound") };
    check(ok(bound, SECRET, { .sessionId = SESSION, .nowMs = T0 }, "same session"),
          "token bound to a sessionId verifies for the same sessionId");
    check(!ok(bound, SECRET, { .sessionId = "victim-session", .nowMs = T0 }, "other session"),
          "token bound to a sessionId does not verify for a different sessionId");
    // Fail-closed in both directions.
    check(!ok(bound, SECRET, { .nowMs = T0 }, "bound without session"),
          "session-bound token does not verify without a sessionId");
    const auto unbound { gen(SECRET, { .nowMs = T0 }, "unbound") };
    check(!ok(unbound, SECRET, { .sessionId = SESSION, .nowMs = T0 }, "unbound with session"),
          "unbound token does not verify with a sessionId");
    // The sessionId is mixed into the HMAC but never written to the token, so
    // the encoded length is unchanged.
    check(bound.size() == unbound.size(), "sessionId does not change the token length");
    // Composes with encoding + algorithm (bun: "sessionId composes with ...").
    const auto composed { gen(SECRET, { .encoding = TokenFormat::Hex, .algorithm = Algorithm::Sha512,
                                        .sessionId = SESSION, .nowMs = T0 }, "composed") };
    check(ok(composed, SECRET, { .encoding = TokenFormat::Hex, .algorithm = Algorithm::Sha512,
                                 .sessionId = SESSION, .nowMs = T0 }, "composed"),
          "sessionId composes with hex encoding and sha512");
    check(!ok(composed, SECRET, { .encoding = TokenFormat::Hex, .algorithm = Algorithm::Sha512,
                                  .sessionId = "other-session", .nowMs = T0 }, "composed, other session"),
          "composed token rejects a different sessionId");
}

void test_empty_input() {
    // bun throws on an empty secret / empty token; mbun surfaces the same
    // conditions as typed errors instead.
    auto emptySecret { generate("", { .nowMs = T0 }) };
    check(!emptySecret.has_value() && emptySecret.error() == Error::EmptySecret,
          "generate with an empty secret returns Error::EmptySecret");

    const auto token { gen(SECRET, { .nowMs = T0 }, "empty-input base") };
    auto emptyToken { verify("", SECRET, { .nowMs = T0 }) };
    check(!emptyToken.has_value() && emptyToken.error() == Error::EmptyToken,
          "verify with an empty token returns Error::EmptyToken");

    auto emptyVerifySecret { verify(token, "", { .nowMs = T0 }) };
    check(!emptyVerifySecret.has_value() && emptyVerifySecret.error() == Error::EmptySecret,
          "verify with an empty secret returns Error::EmptySecret");

    // Whitespace-only token decodes to nothing → invalid, not an error.
    check(!ok("   \r\n\t", SECRET, { .nowMs = T0 }, "whitespace-only token"),
          "whitespace-only token is rejected as invalid");

    // A one-byte secret is legal and round-trips.
    const auto tiny { gen("x", { .nowMs = T0 }, "one-byte secret") };
    check(ok(tiny, "x", { .nowMs = T0 }, "one-byte secret"), "one-byte secret round-trips");
}

void test_malformed_input() {
    const std::array<std::string_view, 6> garbage {
        "not-a-token",                       // too short once decoded
        "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!",  // characters outside the alphabet
        "AAAA",                              // valid base64, far below 64 bytes
        "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@",
        "-_-_-_-_",                          // base64url-only chars, too short
        "0123456789abcdef",                  // hex-looking but too short
    };
    for (auto text : garbage) {
        check(!ok(text, SECRET, { .nowMs = T0 }, text), std::format("malformed token rejected: {}", text));
    }

    // Hex decoding rejects odd lengths and non-hex digits outright.
    const auto hexToken { gen(SECRET, { .encoding = TokenFormat::Hex, .nowMs = T0 }, "hex malformed base") };
    check(!ok(hexToken.substr(0, hexToken.size() - 1), SECRET,
              { .encoding = TokenFormat::Hex, .nowMs = T0 }, "odd-length hex"),
          "odd-length hex token is rejected");
    auto nonHex { hexToken };
    nonHex[10] = 'z';
    check(!ok(nonHex, SECRET, { .encoding = TokenFormat::Hex, .nowMs = T0 }, "non-hex digit"),
          "hex token with a non-hex digit is rejected");

    // base64url-only characters are not valid under strict base64 decoding.
    auto base64UrlToken { gen(SECRET, { .encoding = TokenFormat::Base64Url, .nowMs = T0 }, "b64url alphabet") };
    if (base64UrlToken.find('-') != std::string::npos || base64UrlToken.find('_') != std::string::npos) {
        check(!ok(base64UrlToken, SECRET, { .encoding = TokenFormat::Base64, .nowMs = T0 }, "url chars as base64"),
              "base64url alphabet characters are rejected by the base64 decoder");
    }

    // A handcrafted all-zero payload decodes fine but has no valid signature.
    check(!ok(std::string(86, 'A'), SECRET, { .nowMs = T0 }, "all-zero token"),
          "handcrafted all-zero token is rejected");
    // ...as does an all-0xff payload, which also exercises the expiry overflow
    // guard (timestamp + expiresIn would wrap).
    check(!ok(std::string(128, 'f'), SECRET, { .encoding = TokenFormat::Hex, .nowMs = T0 }, "all-ff token"),
          "handcrafted all-0xff token is rejected without overflowing the expiry check");
}

}  // namespace

int main() {
    test_generate_defaults();
    test_roundtrip();
    test_tampered_token();
    test_wrong_secret();
    test_expiry();
    test_encodings();
    test_algorithms();
    test_session_id();
    test_empty_input();
    test_malformed_input();
    std::println("test_csrf: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}
