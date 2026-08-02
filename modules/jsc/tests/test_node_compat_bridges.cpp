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
    expect_number(
        "(()=>{try{const b=require('node:buffer');"
        "' '.repeat(b.constants.MAX_STRING_LENGTH+1);return 0}"
        "catch(e){return e instanceof RangeError&&e.message==='Invalid string length'?1:0}})()",
        1, "node buffer string limit is enforced before JSC allocation");
    expect_number(
        "(()=>{try{const b=require('node:buffer'),old=b.kMaxLength;"
        "b.kMaxLength=64;const z=require('node:zlib');b.kMaxLength=old;"
        "z.gunzipSync(Buffer.from('H4sIAAAAAAAAA0tMHFgAAIw2K/GAAAAA','base64'));return 0}"
        "catch(e){return e instanceof RangeError&&e.code==='ERR_BUFFER_TOO_LARGE'?1:0}})()",
        1, "node zlib captures the buffer limit at its require edge");

    if (failures != 0) {
        std::println("test_node_compat_bridges: {} failed", failures);
        return 1;
    }
    std::println("test_node_compat_bridges: ok");
    return 0;
}
