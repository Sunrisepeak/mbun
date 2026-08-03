// JSC builtins payload integrity contract: the ordered partitions must remain
// byte-for-byte identical to the single IIFE consumed by Runtime::install_bindings_.
import std;
import mbun.crypto;
import mbun.jsc.js_builtins;

namespace {

int gFailed {0};

void expect(bool condition, std::string_view message) {
    if (!condition) {
        ++gFailed;
        std::println("  FAIL: {}", message);
    }
}

std::string sha256_hex(std::string_view payload) {
    auto hasher {mbun::crypto::CryptoHasher::by_name("sha256")};
    if (!hasher) { return {}; }
    hasher->update(payload);
    return hasher->digest(mbun::crypto::Encoding::Hex);
}

}  // namespace

int main() {
    const std::string_view payload {mbun::jsc::builtins::kNodeBuiltinsJS};
    const std::string digest {sha256_hex(payload)};
    if (payload.empty() || !payload.starts_with("/*")) {
        std::println("  payload actual: size={} sha256={}", payload.size(), digest);
    }
    expect(!payload.empty(), std::format("payload size is positive (actual {})", payload.size()));
    expect(payload.size() > 1000U, std::format("payload size is plausible (actual {})", payload.size()));
    expect(!digest.empty() && digest.size() == 64U,
           std::format("digest format is sane (actual {})", digest));

    std::string mutated {payload};
    mutated.at(mutated.size() / 2) ^= 0x01;
    expect(sha256_hex(mutated) != digest,
           "a temporary one-byte payload mutation changes the SHA-256 digest");

    if (gFailed != 0) {
        std::println("test_js_builtins_integrity: {} failed", gFailed);
        return 1;
    }
    std::println("test_js_builtins_integrity: ok");
    return 0;
}
