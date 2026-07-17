import std;
import mbun.runtime_crypto;

namespace {

int checks {};
int failures {};

void check(bool value, std::string_view name) {
    ++checks;
    if (!value) {
        ++failures;
        std::println("FAIL {}", name);
    }
}

std::span<const std::uint8_t> bytes(std::string_view value) {
    return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}

}  // namespace

int main() {
    using namespace mbun::runtime_crypto;

    auto hash_result { hash_binding("wyhash", 0, bytes("abc")) };
    check(hash_result && hash_result->is64, "hash binding preserves u64 result");
    check(hash_default(bytes("abc"), 0) == hash_result->value, "default hash uses wyhash");
    check(!hash_binding("", 0, bytes("abc")), "hash binding rejects missing algorithm");
    check(!hash_binding("missing", 0, bytes("abc")), "hash binding rejects unknown algorithm");

    auto digest_result { digest_binding("SHA256") };
    check(digest_result && digest_result->digest_length() == 32, "digest lookup is case insensitive");
    if (digest_result) {
        digest_result->update(bytes("ab"));
        auto copy { digest_result->copy() };
        digest_result->update(bytes("c"));
        check(digest_result->digest(DigestEncoding::Hex) ==
                  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              "digest streaming");
        check(copy.digest(DigestEncoding::Hex) ==
                  "fb8e20fc2e4c3f248c60c39bd652f3c1347298bb977b8b4d5903b85055620603",
              "digest copy snapshots state");
    }
    check(!digest_binding("sha3-256"), "deferred digest remains unsupported");

    DeferredBackend deferred;
    PureDigestBackend pure;
    check(!deferred.capabilities().supports(BackendOperation::hmac), "deferred backend reports no HMAC");
    check(!require_backend(deferred, BackendOperation::pbkdf2), "deferred backend gives typed error");
    check(require_backend(pure, BackendOperation::digest).has_value(), "pure backend exposes digest capability");

    std::println("runtime_crypto checks: {} failures: {}", checks, failures);
    return failures == 0 ? 0 : 1;
}
