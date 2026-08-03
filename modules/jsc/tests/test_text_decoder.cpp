// Focused TextEncoder/TextDecoder semantic checks derived from
// compat/bun/test/js/web/encoding/text-decoder.test.js and Bun's TextDecoder runtime.
import std;
import mbun.jsc.runtime;

namespace {

int gFailed {0};

void expect_js(std::string_view source, std::string_view description) {
    const auto result {mbun::jsc::runtime::eval_number(source)};
    if (!result || *result != 1.0) {
        ++gFailed;
        std::println("  FAIL: {}", description);
    }
}

}  // namespace

int main() {
    expect_js(
        "(()=>{const {isUTF16String}=require('bun:internal-for-testing').jscInternals;"
        "return isUTF16String(new TextDecoder().decode(Uint8Array.of(0xf0,0x9f,0x98,0x80)))"
        "?1:0})()",
        "bun:internal-for-testing observes TextDecoder emoji output as UTF-16");
    expect_js(
        "(()=>{const {isUTF16String}=require('bun:internal-for-testing').jscInternals;"
        "const rope='a'.repeat(64)+'\\u0100'.repeat(64);return isUTF16String(rope)"
        "&&!isUTF16String('latin1')?1:0})()",
        "bun:internal-for-testing materializes a UTF-16 rope without leaking an exception");
    expect_js(
        "(()=>{const {isUTF16String}=require('bun:internal-for-testing').jscInternals;"
        "return isUTF16String('latin1')?0:1})()",
        "bun:internal-for-testing observes Latin-1 strings as 8-bit");
    expect_js(
        "(()=>{const {isUTF16String}=require('bun:internal-for-testing').jscInternals;"
        "try{isUTF16String(42);return 0}catch(e){return e instanceof TypeError"
        "&&e.message==='Expected a string'?1:0}})()",
        "bun:internal-for-testing rejects non-string values");
    expect_js(
        "(()=>{const {isUTF16String}=require('bun:internal-for-testing').jscInternals;"
        "try{isUTF16String(new String('x'));return 0}catch(e){return e instanceof TypeError"
        "&&e.message==='Expected a string'?1:0}})()",
        "bun:internal-for-testing rejects boxed String objects");
    expect_js(
        "(()=>{const {isUTF16String}=require('bun:internal-for-testing').jscInternals;"
        "class DerivedString extends String{};try{isUTF16String(new DerivedString('x'));return 0}"
        "catch(e){return e instanceof TypeError&&e.message==='Expected a string'?1:0}})()",
        "bun:internal-for-testing rejects derived String objects");
    expect_js(
        "(()=>{const d=new TextDecoder();let s=d.decode(Uint8Array.of(0xf0,0x9f),"
        "{stream:true});s+=d.decode(Uint8Array.of(0x92),{stream:true});"
        "s+=d.decode(Uint8Array.of(0xa9));return s==='💩'?1:0})()",
        "UTF-8 sequence survives multiple streaming chunks");
    expect_js(
        "(()=>{const d=new TextDecoder('utf-16le');let s=d.decode(Uint8Array.of(0x00,0xd8),"
        "{stream:true});s+=d.decode(Uint8Array.of(0x00,0xdc));return s==='𐀀'?1:0})()",
        "UTF-16 surrogate pair survives a streaming boundary");
    expect_js(
        "(()=>{const d=new TextDecoder();let s=d.decode(Uint8Array.of(0x41),{stream:true});"
        "try{d.decode(123)}catch(e){}s+=d.decode(Uint8Array.of(0xef,0xbb,0xbf,0x42));"
        "return s==='A\\uFEFFB'?1:0})()",
        "invalid input throws without ending the active stream");
    expect_js(
        "(()=>{try{new TextDecoder('definitely-not-an-encoding');return 0}"
        "catch(e){return e instanceof RangeError?1:0}})()",
        "unsupported encoding labels throw RangeError");
    expect_js(
        "(()=>{try{new TextDecoder('utf-8',{fatal:true}).decode(Uint8Array.of(0xc0));return 0}"
        "catch(e){return e.code==='ERR_ENCODING_INVALID_ENCODED_DATA'?1:0}})()",
        "fatal UTF-8 errors expose Bun's error code");
    expect_js(
        "(()=>{const b=new TextEncoder().encode('\\ud800');"
        "return b.length===3&&b[0]===0xef&&b[1]===0xbf&&b[2]===0xbd?1:0})()",
        "TextEncoder replaces an isolated lead surrogate");
    expect_js(
        "(()=>{const b=new Uint8Array([0x41]);const s=new TextDecoder().decode(b,{"
        "get stream(){b.buffer.transfer();return false}});return s===''?1:0})()",
        "TextDecoder reads the input after a stream getter detaches its buffer");
    expect_js(
        "(()=>{const a=new TextDecoderStream('utf-8',null);"
        "const b=new TextDecoderStream('utf-8',{fatal:1,ignoreBOM:{}});"
        "return !a.fatal&&!a.ignoreBOM&&b.fatal&&b.ignoreBOM?1:0})()",
        "TextDecoderStream applies Web IDL option defaults and boolean coercion");

    if (gFailed != 0) {
        std::println("test_text_decoder: {} failed", gFailed);
        return 1;
    }
    std::println("test_text_decoder: ok");
    return 0;
}
