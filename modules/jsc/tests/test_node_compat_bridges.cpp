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
        "(()=>{const b=require('node:buffer');let calls=0;"
        "const count={[Symbol.toPrimitive](){++calls;return String(b.constants.MAX_STRING_LENGTH+1)}};"
        "try{' '.repeat(count);return 0}catch(e){return e instanceof RangeError&&calls===1?1:0}})()",
        1, "node buffer string limit coerces repeat count exactly once");
    expect_number(
        "(()=>{let bigint=false,symbol=false;"
        "try{'x'.repeat(1n)}catch(e){bigint=e instanceof TypeError}"
        "try{'x'.repeat(Symbol())}catch(e){symbol=e instanceof TypeError}"
        "return bigint&&symbol?1:0})()",
        1, "node buffer repeat preserves BigInt and Symbol type errors");
    expect_number(
        "(()=>{try{const b=require('node:buffer'),old=b.kMaxLength;"
        "b.kMaxLength=64;const z=require('node:zlib');b.kMaxLength=old;"
        "z.gunzipSync(Buffer.from('H4sIAAAAAAAAA0tMHFgAAIw2K/GAAAAA','base64'));return 0}"
        "catch(e){return e instanceof RangeError&&e.code==='ERR_BUFFER_TOO_LARGE'?1:0}})()",
        1, "node zlib captures the buffer limit at its require edge");
    expect_number(
        "(()=>{const z=require('node:zlib');let reads=0;const opts={get maxOutputLength(){++reads;return 64}};"
        "try{z.gunzipSync(Buffer.from('H4sIAAAAAAAAA0tMHFgAAIw2K/GAAAAA','base64'),opts);return 0}"
        "catch(e){return e.code==='ERR_BUFFER_TOO_LARGE'&&reads===1?1:0}})()",
        1, "node zlib preserves explicit output limits without mutating options");
    expect_number(
        "typeof globalThis.__mbunZlibArmError==='undefined'?1:0",
        1, "node zlib require-edge accessor installs without diagnostics");
    expect_number(
        "(()=>{try{const a=require('node:assert');const x=new a.Assert({strict:false});"
        "x.equal(2,'2');let constructed=false;try{a.Assert()}catch(e){constructed=e.code==='ERR_CONSTRUCT_CALL_REQUIRED'}"
        "return constructed&&x.deepEqual!==x.deepStrictEqual&&typeof x.partialDeepStrictEqual==='undefined'?1:0}catch{return -1}})()",
        1, "Assert instances retain the proven constructor and strict option surface");
    expect_number(
        "(()=>{try{const a=require('node:assert');const x=new a.Assert({diff:'full'});"
        "try{x.strictEqual(1,2)}catch(e){return e instanceof a.AssertionError&&e.diff==='full'?1:0}"
        "return 0}catch{return -1}})()",
        1, "Assert instance assertions retain the configured diff mode");
    expect_number(
        "(()=>{try{const a=require('node:assert');const {strictEqual}=new a.Assert({diff:'full'});"
        "try{strictEqual(1,2)}catch(e){return e instanceof a.AssertionError&&e.diff==='simple'?1:0}"
        "return 0}catch{return -1}})()",
        1, "destructured Assert methods use default options");
    expect_number(
        "(()=>{try{const a=require('node:assert');const x=new a.Assert({diff:'full'});const original=new Error('original');"
        "try{x.fail(original)}catch(e){return e===original&&!Object.prototype.hasOwnProperty.call(e,'diff')?1:0}"
        "return 0}catch{return -1}})()",
        1, "Assert methods preserve non-AssertionError objects");

    if (failures != 0) {
        std::println("test_node_compat_bridges: {} failed", failures);
        return 1;
    }
    std::println("test_node_compat_bridges: ok");
    return 0;
}
