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
    // Member tests bypass the CLI's resolve_dialect() dispatch. Select Node
    // before the first eval initializes the singleton runtime so these probes
    // exercise the same builtin branches as the compat/node corpus.
    mbun::jsc::runtime::set_dialect(mbun::jsc::runtime::Dialect::Node);
    expect_number(
        "globalThis.__mbunDialect === 'node' ? 1 : 0",
        1, "node compatibility probes run in the Node dialect");
    expect_number(
        "(()=>{try{const repl=require('node:repl');const stream=require('node:stream');"
        "const io=new stream.PassThrough();const server=repl.start({input:io,output:io,terminal:false,prompt:''});"
        "let calls=0,result=-1;server.eval('1+1\\n',server.context,'REPL1',(err,value)=>{calls++;result=err?0:value===2?1:0});"
        "server.close();return calls===1&&result===1?1:0}catch{return -1}})()",
        1, "node REPL default evaluation calls back exactly once despite unavailable legacy RegExp captures");
    expect_number(
        "(()=>{const repl=require('node:repl'),stream=require('node:stream'),io=new stream.PassThrough();"
        "const server=repl.start({input:io,output:io,terminal:false,prompt:'',useGlobal:true});"
        "const original=RegExp.prototype.exec;let calls=0,value=-1,thrown='';"
        "RegExp.prototype.exec=()=>{throw new Error('poison-exec')};"
        "try{server.eval('1+1\\n',server.context,'REPL1',(err,result)=>{calls++;value=err?0:result})}"
        "catch(err){thrown=err&&err.message}finally{RegExp.prototype.exec=original;server.close()}"
        "return calls===1&&value===2&&thrown===''?1:0})()",
        1, "node REPL capture restoration uses the primordial RegExp exec");
    expect_number(
        "(()=>{const repl=require('node:repl'),stream=require('node:stream'),io=new stream.PassThrough();"
        "const server=repl.start({input:io,output:io,terminal:false,prompt:''});"
        "const original=Object.getOwnPropertyDescriptor(RegExp,'$1');"
        "const marker=new TypeError('RegExp.$N getters require RegExp constructor as |this|');"
        "let calls=0,thrown=null;Object.defineProperty(RegExp,'$1',{configurable:true,get(){throw marker}});"
        "try{server.eval('1+1\\n',server.context,'REPL1',()=>{calls++})}catch(err){thrown=err}"
        "finally{Object.defineProperty(RegExp,'$1',original);server.close()}"
        "return calls===0&&thrown===marker?1:0})()",
        1, "node REPL propagates a user capture getter even when its message matches issue 65");
    expect_number(
        "(()=>{try{void RegExp.$1}catch{return 1}"
        "const repl=require('node:repl'),stream=require('node:stream'),io=new stream.PassThrough();"
        "const server=repl.start({input:io,output:io,terminal:false,prompt:'',useGlobal:true});"
        "let first=0,second=0,value;server.eval('/(alpha)/.exec(\\\"alpha\\\");0\\n',server.context,'REPL1',(err)=>{first++;if(err)value=err});"
        "/(outside)/.exec('outside');server.eval('RegExp.$1\\n',server.context,'REPL2',(err,result)=>{second++;value=err||result});"
        "server.close();return first===1&&second===1&&value==='alpha'?1:0})()",
        1, "node REPL restores the prior capture before the next evaluation when legacy captures are supported");
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
