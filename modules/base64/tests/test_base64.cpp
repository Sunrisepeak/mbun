import std;
import mbun.base64;
namespace {
using namespace mbun::base64;
int checks{}; int failures{};
void check(bool ok, std::string_view name) { ++checks; if (!ok) { ++failures; if (failures < 20) std::println("FAIL {}", name); } }
std::span<const std::byte> bytes(std::string_view s) { return {reinterpret_cast<const std::byte*>(s.data()), s.size()}; }
std::string lenient(std::string_view s) { std::string out(decode_len_upper_bound(s.size()), '\0'); out.resize(decode_lenient(out, s, false)); return out; }
void base64_tests() {
    check(encode_alloc(bytes("")) == "", "empty encode"); check(encode_alloc(bytes("f")) == "Zg==", "one padding");
    check(encode_alloc(bytes("fo")) == "Zm8=", "two padding"); check(encode_alloc(bytes("foo")) == "Zm9v", "three");
    check(encode_alloc(bytes("foobar")) == "Zm9vYmFy", "multi"); check(encode_url_safe_alloc(bytes("f")) == "Zg", "url no padding");
    check(encode_url_safe_alloc(bytes("\xff\xff\xbe")) == "__--", "url alphabet"); check(decode_alloc("Zg==").value_or("x") == "f", "decode padding");
    check(decode_alloc("__--").value_or("x") == "\xff\xff\xbe", "decode url"); check(!decode_alloc("Zm9v*").has_value(), "strict invalid");
    check(lenient("Zm9v\x80YmFy") == "foobar", "ignore byte"); check(lenient("Zm9v*YmFy") == "foobar", "ignore punctuation");
    check(lenient(" Z m 9\tv\nY\rm F y ") == "foobar", "ignore whitespace"); check(lenient("Zm9v=YmFy") == "foo", "stop padding");
    check(lenient("=Zm9vYmFy") == "", "leading padding"); check(lenient("Zm9vYmFyA") == "foobar", "leftover"); check(lenient("-_+/") == "\xfb\xff\xbf", "mixed alphabets");
}
void hex_tests() {
    const std::string raw{"\0\x01\xab\xff", 4};
    check(hex_encode_alloc(bytes(raw)) == "0001abff", "hex encode"); check(hex_decode_alloc("") == "", "empty hex");
    check(hex_decode_alloc("0001AbFf") == raw, "hex decode"); check(hex_decode_alloc("A") == "", "odd hex");
    check(hex_decode_alloc("Abx") == "\xab", "hex trailing invalid"); check(hex_decode_alloc("ab\xff\xff" "cd") == "\xab", "hex ff");
    check(hex_decode_alloc("ab\u0100") == "\xab", "hex unicode"); check(hex_decode_alloc("123g45") == "\x12", "hex invalid pair");
}
}
int main() { base64_tests(); hex_tests(); std::println("base64 checks: {}, failures: {}", checks, failures); return failures == 0 ? 0 : 1; }
