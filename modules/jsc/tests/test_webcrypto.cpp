// Focused WebCrypto vertical-slice tests. These exercise the same runtime and
// builtin payload used by app/cli; no mock crypto implementation is involved.
import std;
import mbun.jsc.runtime;

namespace {

int gFailed { 0 };

void expect_number(std::string_view source, double expected, std::string_view name) {
    const auto result { mbun::jsc::runtime::eval_number(source) };
    if (!result || *result != expected) {
        ++gFailed;
        std::println("FAIL {}: {}", name,
                     result ? std::format("got {}, expected {}", *result, expected)
                            : result.error());
    }
}

}  // namespace

int main() {
    expect_number(
        "(()=>{const original=crypto.subtle;crypto.subtle=123;"
        "return crypto.subtle===original?1:0})()",
        1, "crypto.subtle ignores assignment");
    expect_number(
        "typeof SubtleCrypto==='function'&&SubtleCrypto.name==='SubtleCrypto'&&"
        "crypto.subtle instanceof SubtleCrypto?1:0",
        1, "SubtleCrypto global and singleton prototype");

    expect_number(
        "globalThis.__webcryptoHmacResult=0;"
        "(async()=>{try{"
        "const enc=new TextEncoder();"
        "const key=await crypto.subtle.importKey('raw',enc.encode('secret'),"
        "{name:'HMAC',hash:'SHA-256'},false,['sign','verify']);"
        "const data=enc.encode('hello world');"
        "const sig=await crypto.subtle.sign('HMAC',key,data);"
        "const bytes=new Uint8Array(sig);let hex='';"
        "for(const byte of bytes)hex+=byte.toString(16).padStart(2,'0');"
        "const valid=await crypto.subtle.verify('HMAC',key,sig,data);"
        "data[0]^=1;const invalid=await crypto.subtle.verify('HMAC',key,sig,data);"
        "__webcryptoHmacResult=(hex==='734cc62f32841568f45715aeb9f4d7891324e6d948e4c6c60c0621cdac48623a'"
        "&&valid===true&&invalid===false&&key.algorithm.name==='HMAC'"
        "&&key.algorithm.hash.name==='SHA-256'&&!key.extractable"
        "&&key instanceof CryptoKey&&key.constructor===CryptoKey"
        "&&!('__mbunWebCryptoNative' in globalThis))?1:-1;"
        "}catch(e){__webcryptoHmacResult=-2;}})();0",
        0, "schedule HMAC import/sign/verify");
    expect_number("__webcryptoHmacResult", 1, "HMAC known answer and verification");

    expect_number(
        "globalThis.__webcryptoNodeBridge=0;"
        "(async()=>{try{"
        "const {createHmac,KeyObject}=require('node:crypto');"
        "const key=await crypto.subtle.generateKey({name:'HMAC',hash:'SHA-256'},true,['sign']);"
        "const object=KeyObject.from(key);"
        "const data=new TextEncoder().encode('payload');"
        "const direct=createHmac('sha256',key).update(data).digest('hex');"
        "const bridged=createHmac('sha256',object).update(data).digest('hex');"
        "__webcryptoNodeBridge=direct===bridged?1:-1;"
        "}catch(e){__webcryptoNodeBridge=-2;}})();0",
        0, "schedule node:crypto CryptoKey bridge");
    expect_number("__webcryptoNodeBridge", 1,
                  "node:crypto accepts a branded WebCrypto CryptoKey");

    expect_number(
        "globalThis.__webcryptoRawAttack=0;"
        "(async()=>{try{"
        "const key=await crypto.subtle.importKey('raw',new Uint8Array([1,2,3]),"
        "{name:'HMAC',hash:'SHA-256'},false,['sign']);"
        "const hidden=!('_raw' in key)&&!Reflect.ownKeys(key).includes('_raw');"
        "key._raw=new Uint8Array([9,9,9]);"
        "const sig=await crypto.subtle.sign('HMAC',key,new Uint8Array([4,5,6]));"
        "const hex=Array.from(new Uint8Array(sig),b=>b.toString(16).padStart(2,'0')).join('');"
        "__webcryptoRawAttack=hidden&&hex==='52f0bd967282a27354acf04172d09644ad38d9411091496cd4b9e43c1b7eee15'?1:-1;"
        "}catch(e){__webcryptoRawAttack=-2;}})();0",
        0, "schedule hidden raw-key attack");
    expect_number("__webcryptoRawAttack", 1, "raw key material stays hidden and authoritative");

    expect_number(
        "globalThis.__webcryptoBrandAttack=0;"
        "(async()=>{let newRejected=false,callRejected=false,forgeryRejected=false;"
        "try{new CryptoKey()}catch(e){newRejected=e instanceof TypeError}"
        "try{CryptoKey()}catch(e){callRejected=e instanceof TypeError}"
        "try{const fake=Object.create(CryptoKey.prototype);"
        "Object.defineProperties(fake,{type:{value:'secret'},extractable:{value:false},"
        "algorithm:{value:{name:'HMAC',hash:{name:'SHA-256'}}},usages:{value:['sign']},"
        "_raw:{value:new Uint8Array([1,2,3])}});"
        "await crypto.subtle.sign('HMAC',fake,new Uint8Array([4]));"
        // A prototype-only forgery carries no key material, so it is rejected by
        // the binding's brand check, not by the algorithm policy: bun 1.4.0 says
        // TypeError("Argument 2 ('key') to SubtleCrypto.sign must be an instance
        // of CryptoKey"), verified against .mbun/bin/bun-rust. The HMAC-only
        // slice this test predates answered InvalidAccessError here, which no
        // bun build does. The real-key checks below still pin InvalidAccessError.
        "}catch(e){forgeryRejected=e instanceof TypeError}"
        "__webcryptoBrandAttack=newRejected&&callRejected&&forgeryRejected?1:-1;"
        "})().catch(()=>{__webcryptoBrandAttack=-2});0",
        0, "schedule CryptoKey constructor and forgery attacks");
    expect_number("__webcryptoBrandAttack", 1, "CryptoKey construction and forgery rejected");

    // The invariant is ENFORCEMENT, not immutability. node's `key.algorithm` is a
    // plain mutable object -- `test-webcrypto-internal-slots.mjs` assigns
    // `algorithm.name = 'ed25519'` and requires the write to stick -- so this case
    // must NOT assert Object.isFrozen. It previously did, that assertion encoded a
    // belief about node that is false, and satisfying it by freezing the public
    // copy cost that corpus file.
    //
    // What must hold, and what is checked below: however the caller rewrites the
    // public `algorithm` / `usages`, none of it reaches the internal slots. So
    // `type` and `extractable` (getters straight off the internal metadata) are
    // unchanged, a direct `usages` redefinition is refused, and above all signing
    // with a verify-only key still throws InvalidAccessError. The public copies are
    // allowed to lie; the crypto is not.
    expect_number(
        "globalThis.__webcryptoMetadataAttack=0;"
        "(async()=>{try{"
        "const key=await crypto.subtle.importKey('raw',new Uint8Array([1,2,3]),"
        "{name:'HMAC',hash:'SHA-256'},false,['verify']);"
        "try{key.type='public'}catch{}try{key.extractable=true}catch{}"
        "try{key.algorithm.name='AES-GCM'}catch{}try{key.algorithm.hash.name='SHA-1'}catch{}"
        "try{key.usages.push('sign')}catch{}try{key.usages=['sign']}catch{}"
        "try{Object.defineProperty(key,'usages',{value:['sign']})}catch{}"
        "let signRejected=false;try{await crypto.subtle.sign('HMAC',key,new Uint8Array([4]))}"
        "catch(e){signRejected=e.name==='InvalidAccessError'}"
        // A fresh key proves the internal slot was never touched: the mutations
        // above must not have taught the implementation the wrong algorithm.
        "const fresh=await crypto.subtle.importKey('raw',new Uint8Array([1,2,3]),"
        "{name:'HMAC',hash:'SHA-256'},false,['verify']);"
        "__webcryptoMetadataAttack=signRejected&&key.type==='secret'&&!key.extractable"
        "&&fresh.algorithm.name==='HMAC'&&fresh.algorithm.hash.name==='SHA-256'"
        "&&fresh.usages.length===1&&fresh.usages[0]==='verify'"
        "&&!Object.isExtensible(key)?1:-1;"
        "}catch(e){__webcryptoMetadataAttack=-2;}})();0",
        0, "schedule CryptoKey metadata and usage attacks");
    expect_number("__webcryptoMetadataAttack", 1,
                  "rewriting a CryptoKey's public metadata cannot reach the internal slots");

    expect_number(
        "globalThis.__webcryptoImportConversion=0;"
        "(async()=>{const d=new Uint8Array([1]);const a={name:'HMAC',hash:'SHA-256'};"
        "const rejects=async(args,name)=>{try{await crypto.subtle.importKey(...args);return false}"
        "catch(e){return e.name===name}};"
        "const omitted=await rejects(['raw',d,a,false],'TypeError');"
        "const mixed=await rejects(['RAW',d,a,false,['sign']],'TypeError');"
        "const boxed=await crypto.subtle.importKey(new String('raw'),d,a,false,['sign']);"
        "let conversions=0;"
        "const object=await crypto.subtle.importKey({toString(){conversions++;return 'raw'}},"
        "d,a,false,['verify']);"
        "let algorithmRead=false;"
        "const ordered=await rejects([{toString(){return 'RAW'}},d,"
        "{get name(){algorithmRead=true;return 'HMAC'},hash:'SHA-256'},false,['sign']],'TypeError');"
        "const invalid=await rejects(['raw',d,a,false,['SIGN']],'TypeError');"
        "const numeric=await rejects(['raw',d,a,false,[1]],'TypeError');"
        "const arrayLike=await rejects(['raw',d,a,false,{0:'sign',length:1}],'TypeError');"
        "const fromSet=await crypto.subtle.importKey('raw',d,a,false,new Set(['verify','sign']));"
        "const duplicate=await crypto.subtle.importKey('raw',d,a,false,['verify','sign','verify','sign']);"
        "__webcryptoImportConversion=omitted&&mixed&&boxed.usages.join(',')==='sign'"
        "&&object.usages.join(',')==='verify'&&conversions===1&&ordered&&!algorithmRead"
        "&&invalid&&numeric&&arrayLike"
        "&&fromSet.usages.join(',')==='sign,verify'&&duplicate.usages.join(',')==='sign,verify'?1:-1;"
        "})().catch(()=>{__webcryptoImportConversion=-2});0",
        0, "schedule importKey WebIDL conversion attacks");
    expect_number("__webcryptoImportConversion", 1, "importKey required args and WebIDL enums");

    if (gFailed != 0) {
        std::println("test_webcrypto: {} failed", gFailed);
        return 1;
    }
    std::println("test_webcrypto: ok");
    return 0;
}
