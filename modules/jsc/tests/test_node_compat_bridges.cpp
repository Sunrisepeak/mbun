// Focused Node-compatibility probes for small runtime binding seams.
import std;
import mbun.jsc.runtime;

namespace {

int failures { 0 };

void expect_number(std::string_view source, double expected, std::string_view name) {
    const auto result { mbun::jsc::runtime::eval_number(source) };
    if (!result || *result != expected) {
        ++failures;
        std::println("FAIL {}: {}", name,
                     result ? std::format("got {}, expected {}", *result, expected)
                            : result.error());
    }
}

}  // namespace

int main() {
    expect_number(
        "(()=>{try{const fs=require('node:fs');"
        "const p=require('node:util').promisify(fs.exists);"
        "return p.name==='exists'?1:0}catch{return -1}})()",
        1, "custom promisify preserves the original function name");
    expect_number(
        "(()=>{try{const b=process.binding('inspector');"
        "return b&&typeof b==='object'?1:0}catch{return -1}})()",
        1, "the inspector binding allowlist has a namespace");

    if (failures != 0) {
        std::println("test_node_compat_bridges: {} failed", failures);
        return 1;
    }
    std::println("test_node_compat_bridges: ok");
    return 0;
}
