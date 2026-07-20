// node:buffer completion partition: the raw <enc>Write / <enc>Slice bindings
// (utf8Write, asciiWrite, latin1Write, base64Write, base64urlWrite, hexWrite,
// ucs2Write, utf16leWrite and the matching Slices), a node-exact
// Buffer.prototype.write/toString/fill, the missing variable-width and BigInt
// integer accessors, swap16/32/64, allocUnsafeSlow/poolSize/copyBytesFrom/
// isEncoding statics, Buffer.from(arrayBuffer, byteOffset, length) bounds, and
// the node:buffer module-level isAscii/isUtf8/SlowBuffer/INSPECT_MAX_BYTES.
//
// It also owns the ENCODING-AWARE statics Buffer.from(string, encoding) and
// Buffer.byteLength(string, encoding). The underlying class (process_web.cppm's
// `class Buffer extends Uint8Array`) resolves neither: its from() has no
// base64url arm and utf8-encodes anything it does not recognise, and its
// byteLength() takes no encoding parameter at all -- both fail silently, with
// the wrong length and no throw. See "static (Buffer.from/byteLength)" below.
//
// Buffer here is a JS class over Uint8Array, so everything can be done by
// augmenting Buffer.prototype in place. Only Buffer itself is re-bound (a thin
// wrapper sharing the same .prototype) so the deprecated `new Buffer(ab, o, l)`
// form takes the same bounds-checked path as Buffer.from.
//
// NOTE: appended AFTER the master builtins IIFE; self-contained IIFE that
// re-binds G = globalThis. Top level must never throw.
//
// Blueprint: node lib/buffer.js + lib/internal/buffer.js and node_buffer.cc
// binding semantics (strict JS-wrapper family utf8/latin1/ascii vs clamping
// C++-binding family for the rest), as pinned down by bun's
// test/js/node/buffer.test.js battery.
export module mbun.jsc.js_builtins:node_buffer_extra;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kNodeBufferExtraJS = R"JS(
(function () {
  const G = globalThis;
  try {
    const M = G.__mbunNativeModules;
    const OrigBuffer = G.Buffer;
    if (!OrigBuffer || !OrigBuffer.prototype) return;
    const proto = OrigBuffer.prototype;

    // ------------------------------------------------------------- errors
    // node's determineSpecificType (lib/internal/errors.js): every primitive
    // except undefined reports its VALUE too -- "type number (123)", not the
    // bare "type number". Functions report `function <name>` (bun emits the
    // trailing space for an anonymous one).
    const received = (v) => {
      if (v === undefined) return "undefined";
      if (v === null) return "null";
      const t = typeof v;
      if (t === "string") return "type string ('" + (v.length > 28 ? v.slice(0, 25) + "..." : v) + "')";
      if (t === "function") return "function " + (v.name || "");
      if (t === "object") {
        const c = v.constructor && v.constructor.name;
        return "an instance of " + (c || "Object");
      }
      const ins = t === "bigint" ? String(v) + "n" : t === "symbol" ? v.toString() : String(v);
      return "type " + t + " (" + (ins.length > 28 ? ins.slice(0, 25) + "..." : ins) + ")";
    };
    const errArgType = (name, type, value) => {
      const e = new TypeError(`The "${name}" argument must be of type ${type}. Received ` + received(value));
      e.code = "ERR_INVALID_ARG_TYPE";
      return e;
    };
    // node phrases class-valued arg errors as "must be an instance of X".
    const errArgInstance = (name, cls, value) => {
      const e = new TypeError(`The "${name}" argument must be an instance of ${cls}. Received ` + received(value));
      e.code = "ERR_INVALID_ARG_TYPE";
      return e;
    };
    const isU8 = (v) => v instanceof Uint8Array;
    const errOutOfRange = (name, range, value) => {
      const v = typeof value === "bigint" ? value + "n" : String(value);
      const e = new RangeError(`The value of "${name}" is out of range. It must be ${range}. Received ` + v);
      e.code = "ERR_OUT_OF_RANGE";
      return e;
    };
    const errIndexOutOfRange = () => {
      const e = new RangeError("Index out of range");
      e.code = "ERR_OUT_OF_RANGE";
      return e;
    };
    const errBufferOOB = (name) => {
      const e = new RangeError(name
        ? `"${name}" is outside of buffer bounds`
        : "Attempt to access memory outside buffer bounds");
      e.code = "ERR_BUFFER_OUT_OF_BOUNDS";
      return e;
    };
    const errUnknownEncoding = (enc) => {
      const e = new TypeError("Unknown encoding: " + enc);
      e.code = "ERR_UNKNOWN_ENCODING";
      return e;
    };
    // WTF::String::MaxLength (== INT32_MAX). Decoding a buffer whose output
    // would exceed it must throw ERR_STRING_TOO_LONG *before* any allocation
    // (jsc/bindings/JSBuffer.cpp:jsBufferToStringFromBytes); otherwise a 2 GiB
    // buffer drags the process into an OOM instead of a catchable error.
    const MAX_STRING_LENGTH = 0x7fffffff;
    const errStringTooLong = () => {
      const e = new Error("Cannot create a string longer than " + MAX_STRING_LENGTH + " characters");
      e.code = "ERR_STRING_TOO_LONG";
      return e;
    };
    const errInvalidBufferSize = (bits) => {
      const e = new RangeError(`Buffer size must be a multiple of ${bits}-bits`);
      e.code = "ERR_INVALID_BUFFER_SIZE";
      return e;
    };
    const validateNumber = (value, name) => {
      if (typeof value !== "number") throw errArgType(name, "number", value);
    };
    const boundsError = (value, length, type) => {
      if (Math.floor(value) !== value) {
        validateNumber(value, type || "offset");
        throw errOutOfRange(type || "offset", "an integer", value);
      }
      if (length < 0) throw errBufferOOB();
      throw errOutOfRange(type || "offset", `>= ${type ? 1 : 0} and <= ${length}`, value);
    };
    const trunc0 = (v) => { const n = Math.trunc(+v); return Number.isNaN(n) ? 0 : n; };
    const isDetached = (buf) => {
      try { return !!(buf.buffer && buf.buffer.detached === true); } catch (_) { return false; }
    };

    // ------------------------------------------------------- raw encoders
    // All raw fns take (buf, string, offset, max) with sanitized non-negative
    // integer offset/max (max already clamped to the space left) and return
    // the number of bytes written.
    function rawUtf8Write(buf, str, offset, max) {
      let w = 0;
      const len = str.length;
      for (let i = 0; i < len; i++) {
        let c = str.charCodeAt(i);
        if (c >= 0xd800 && c <= 0xdbff) {
          const n = i + 1 < len ? str.charCodeAt(i + 1) : 0;
          if (n >= 0xdc00 && n <= 0xdfff) { c = 0x10000 + ((c - 0xd800) << 10) + (n - 0xdc00); i++; }
          else c = 0xfffd;
        } else if (c >= 0xdc00 && c <= 0xdfff) c = 0xfffd;
        if (c < 0x80) {
          if (w + 1 > max) break;
          buf[offset + w++] = c;
        } else if (c < 0x800) {
          if (w + 2 > max) break;
          buf[offset + w++] = 0xc0 | (c >> 6);
          buf[offset + w++] = 0x80 | (c & 63);
        } else if (c < 0x10000) {
          if (w + 3 > max) break;
          buf[offset + w++] = 0xe0 | (c >> 12);
          buf[offset + w++] = 0x80 | ((c >> 6) & 63);
          buf[offset + w++] = 0x80 | (c & 63);
        } else {
          if (w + 4 > max) break;
          buf[offset + w++] = 0xf0 | (c >> 18);
          buf[offset + w++] = 0x80 | ((c >> 12) & 63);
          buf[offset + w++] = 0x80 | ((c >> 6) & 63);
          buf[offset + w++] = 0x80 | (c & 63);
        }
      }
      return w;
    }
    function rawLatin1Write(buf, str, offset, max) {
      const n = Math.min(max, str.length);
      for (let i = 0; i < n; i++) buf[offset + i] = str.charCodeAt(i) & 0xff;
      return n;
    }
    function rawUcs2Write(buf, str, offset, max) {
      const units = Math.min(str.length, Math.floor(max / 2));
      for (let i = 0; i < units; i++) {
        const c = str.charCodeAt(i);
        buf[offset + 2 * i] = c & 0xff;
        buf[offset + 2 * i + 1] = c >> 8;
      }
      return units * 2;
    }
    const hexVal = (c) => {
      if (c >= 48 && c <= 57) return c - 48;
      if (c >= 97 && c <= 102) return c - 87;
      if (c >= 65 && c <= 70) return c - 55;
      return -1;
    };
    function rawHexWrite(buf, str, offset, max) {
      const pairs = Math.min(Math.floor(str.length / 2), max);
      let i = 0;
      for (; i < pairs; i++) {
        const hi = hexVal(str.charCodeAt(2 * i));
        const lo = hexVal(str.charCodeAt(2 * i + 1));
        if (hi < 0 || lo < 0) break;
        buf[offset + i] = (hi << 4) | lo;
      }
      return i;
    }
    const B64V = (() => {
      const t = new Int8Array(128).fill(-1);
      const A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
      for (let i = 0; i < A.length; i++) t[A.charCodeAt(i)] = i;
      t[43] = 62; t[47] = 63;     // + /
      t[45] = 62; t[95] = 63;     // - _ (base64url)
      return t;
    })();
    function rawBase64Write(buf, str, offset, max) {
      let w = 0, acc = 0, bits = 0;
      for (let i = 0; i < str.length && w < max; i++) {
        const c = str.charCodeAt(i);
        if (c === 61) break; // '='
        const v = c < 128 ? B64V[c] : -1;
        if (v < 0) continue; // lenient: skip whitespace/invalid
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
          bits -= 8;
          buf[offset + w++] = (acc >> bits) & 0xff;
        }
      }
      return w;
    }

    // ------------------------------------------------------- raw decoders
    // All take (buf, start, end) with 0 <= start < end <= live length.
    function rawUtf8Slice(buf, s, e) {
      return new TextDecoder("utf-8").decode(buf.subarray(s, e));
    }
    function rawLatin1Slice(buf, s, e) {
      let out = "";
      for (let i = s; i < e; i += 4096)
        out += String.fromCharCode.apply(null, buf.subarray(i, Math.min(i + 4096, e)));
      return out;
    }
    function rawAsciiSlice(buf, s, e) {
      const a = new Array(e - s);
      for (let i = s; i < e; i++) a[i - s] = buf[i] & 0x7f;
      let out = "";
      for (let i = 0; i < a.length; i += 4096)
        out += String.fromCharCode.apply(null, a.slice(i, i + 4096));
      return out;
    }
    function rawUcs2Slice(buf, s, e) {
      const units = Math.floor((e - s) / 2);
      let out = "";
      for (let i = 0; i < units; i++)
        out += String.fromCharCode(buf[s + 2 * i] | (buf[s + 2 * i + 1] << 8));
      return out;
    }
    const HEXP = Array.from({ length: 256 }, (_, b) => b.toString(16).padStart(2, "0"));
    function rawHexSlice(buf, s, e) {
      const parts = new Array(e - s);
      for (let i = s; i < e; i++) parts[i - s] = HEXP[buf[i]];
      return parts.join("");
    }
    const B64A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const B64UA = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    function b64Encode(buf, s, e, alphabet, pad) {
      const parts = [];
      let i = s;
      for (; i + 2 < e; i += 3) {
        const n = (buf[i] << 16) | (buf[i + 1] << 8) | buf[i + 2];
        parts.push(alphabet[(n >> 18) & 63] + alphabet[(n >> 12) & 63] + alphabet[(n >> 6) & 63] + alphabet[n & 63]);
      }
      const rem = e - i;
      if (rem === 1) {
        const n = buf[i] << 16;
        parts.push(alphabet[(n >> 18) & 63] + alphabet[(n >> 12) & 63] + (pad ? "==" : ""));
      } else if (rem === 2) {
        const n = (buf[i] << 16) | (buf[i + 1] << 8);
        parts.push(alphabet[(n >> 18) & 63] + alphabet[(n >> 12) & 63] + alphabet[(n >> 6) & 63] + (pad ? "=" : ""));
      }
      return parts.join("");
    }
    const rawBase64Slice = (buf, s, e) => b64Encode(buf, s, e, B64A, true);
    const rawBase64urlSlice = (buf, s, e) => b64Encode(buf, s, e, B64UA, false);

    // --------------------------------------------- binding-style wrappers
    // Strict family (utf8/latin1/ascii): node's JS wrapper. Raw (untruncated)
    // comparisons, so a NaN offset/length slips through the bounds checks and
    // only truncates to 0 at write time.
    function makeStrictWrite(raw, name) {
      const fn = function (string, offset, length) {
        if (typeof string !== "string") throw errArgType("argument", "string", string);
        const len = this.length;
        offset = offset === undefined ? 0 : +offset;
        if (offset < 0 || offset > len) throw errBufferOOB();
        length = length === undefined ? len - offset : +length;
        if (length < 0 || length > len - offset) throw errBufferOOB();
        if (isDetached(this)) throw new TypeError("Cannot write to a detached ArrayBuffer");
        let off = trunc0(offset), max = trunc0(length);
        const avail = this.length - off;
        if (max > avail) max = avail;
        if (max < 0) max = 0;
        return raw(this, string, off, max);
      };
      Object.defineProperty(fn, "name", { value: name, configurable: true });
      return fn;
    }
    // Clamping family (ucs2/utf16le/base64/base64url/hex): the C++
    // binding. Coerces then clamps an oversized length; a negative offset or
    // length is ERR_OUT_OF_RANGE, an offset past the end ERR_BUFFER_OUT_OF_BOUNDS.
    function makeClampWrite(raw, name) {
      const fn = function (string, offset, length) {
        if (typeof string !== "string") throw errArgType("argument", "string", string);
        const len0 = this.length;
        const offN = offset === undefined ? 0 : +offset;
        if (offN < 0) throw errIndexOutOfRange();
        const off = trunc0(offN);
        if (off > len0) throw errBufferOOB();
        let max;
        if (length === undefined) max = len0 - off;
        else {
          const lenN = +length;
          if (lenN < 0) throw errIndexOutOfRange();
          max = trunc0(lenN);
        }
        if (isDetached(this)) throw new TypeError("Cannot write to a detached ArrayBuffer");
        const avail = this.length - off;
        if (max > avail) max = avail;
        if (max < 0) max = 0;
        return raw(this, string, off, max);
      };
      Object.defineProperty(fn, "name", { value: name, configurable: true });
      return fn;
    }
    // <enc>Slice: negative index throws, start >= end short-circuits to ""
    // without a range check, then end past the (pre-coercion) length throws.
    function makeSlice(decode, name) {
      const fn = function (start, end) {
        const len0 = this.length;
        const s = start === undefined ? 0 : trunc0(start);
        const e = end === undefined ? len0 : trunc0(end);
        if (s < 0 || e < 0) throw errIndexOutOfRange();
        if (s >= e) return "";
        if (e > len0) throw errIndexOutOfRange();
        const live = this.length;
        const ee = e > live ? live : e;
        if (s >= ee) return "";
        return decode(this, s, ee);
      };
      Object.defineProperty(fn, "name", { value: name, configurable: true });
      return fn;
    }
    // NOTE: trunc0 keeps +/-Infinity (Math.trunc(Infinity) === Infinity), so
    // an Infinity offset lands in the "past the end" branches as node does.

    proto.utf8Write = makeStrictWrite(rawUtf8Write, "utf8Write");
    proto.latin1Write = makeStrictWrite(rawLatin1Write, "latin1Write");
    proto.asciiWrite = makeStrictWrite(rawLatin1Write, "asciiWrite");
    proto.ucs2Write = makeClampWrite(rawUcs2Write, "ucs2Write");
    proto.utf16leWrite = makeClampWrite(rawUcs2Write, "utf16leWrite");
    proto.hexWrite = makeClampWrite(rawHexWrite, "hexWrite");
    proto.base64Write = makeClampWrite(rawBase64Write, "base64Write");
    proto.base64urlWrite = makeClampWrite(rawBase64Write, "base64urlWrite");

    proto.utf8Slice = makeSlice(rawUtf8Slice, "utf8Slice");
    proto.latin1Slice = makeSlice(rawLatin1Slice, "latin1Slice");
    proto.asciiSlice = makeSlice(rawAsciiSlice, "asciiSlice");
    const rawUcs2SliceEven = (buf, s, e) => rawUcs2Slice(buf, s, s + (((e - s) >>> 1) << 1));
    proto.ucs2Slice = makeSlice(rawUcs2SliceEven, "ucs2Slice");
    proto.utf16leSlice = makeSlice(rawUcs2SliceEven, "utf16leSlice");
    proto.hexSlice = makeSlice(rawHexSlice, "hexSlice");
    proto.base64Slice = makeSlice(rawBase64Slice, "base64Slice");
    proto.base64urlSlice = makeSlice(rawBase64urlSlice, "base64urlSlice");

    // ------------------------------------------------- encoding dispatch
    const OPS = {};
    // `cap` = the largest input byte count whose decoded output still fits in a
    // JS string, mirroring jsBufferToStringFromBytes' per-encoding checks.
    const defOps = (names, write, slice, cap) => {
      for (const n of names) OPS[n] = { write, slice, cap: cap === undefined ? MAX_STRING_LENGTH : cap };
    };
    defOps(["utf8", "utf-8"], "utf8Write", "utf8Slice");
    defOps(["ascii"], "asciiWrite", "asciiSlice");
    defOps(["latin1", "binary"], "latin1Write", "latin1Slice");
    defOps(["base64"], "base64Write", "base64Slice", ((MAX_STRING_LENGTH / 4) | 0) * 3);
    defOps(["base64url"], "base64urlWrite", "base64urlSlice", ((MAX_STRING_LENGTH / 4) | 0) * 3);
    defOps(["ucs2", "ucs-2", "utf16le", "utf-16le"], "ucs2Write", "ucs2Slice");
    defOps(["hex"], "hexWrite", "hexSlice", (MAX_STRING_LENGTH / 2) | 0);
    // NB: no "utf16be" row. It is not an encoding: BufferEncodingType has no
    // such member, so bun and node both throw ERR_UNKNOWN_ENCODING from
    // write/toString/fill and report isEncoding("utf16be") === false.
    const getOps = (enc) => {
      if (typeof enc !== "string") return undefined;
      return OPS[enc] || OPS[enc.toLowerCase()];
    };
    // ------------------------------------ static (Buffer.from/byteLength)
    // The statics do NOT share the OPS table above. bun resolves an encoding
    // *name* through parseEnumerationFromView<BufferEncodingType>
    // (jsc/bindings/JSBufferEncodingType.cpp:103), which knows only the eight
    // BufferEncodingType members and rejects any name of length <= 2. Two
    // consequences the OPS table gets wrong if reused here:
    //   * ""        -> unknown (node accepts "" as utf8 for from(); bun throws)
    //   * "utf16be" -> unknown, and not an encoding at all (see below)
    const STATIC_ENC = Object.assign(Object.create(null), {
      "utf8": "utf8", "utf-8": "utf8",
      "ucs2": "utf16le", "ucs-2": "utf16le", "utf16le": "utf16le", "utf-16le": "utf16le",
      "latin1": "latin1", "binary": "latin1",
      "ascii": "ascii",
      "base64": "base64", "base64url": "base64url",
      "hex": "hex",
    });
    // Null-prototype map, so "constructor"/"toString" resolve to undefined.
    const parseStaticEnc = (enc) => {
      if (typeof enc !== "string" || enc.length <= 2) return undefined;
      const c = STATIC_ENC[enc];
      return c !== undefined ? c : STATIC_ENC[enc.toLowerCase()];
    };
    const STATIC_WRITE = Object.assign(Object.create(null), {
      "utf8": "utf8Write", "utf16le": "ucs2Write", "latin1": "latin1Write",
      "ascii": "asciiWrite", "base64": "base64Write", "base64url": "base64urlWrite",
      "hex": "hexWrite",
    });
    // Mirrors rawUtf8Write's surrogate handling (pair -> code point, lone
    // surrogate -> U+FFFD) so the allocation is exactly what the write fills.
    function utf8ByteLength(str) {
      const len = str.length;
      let n = 0;
      for (let i = 0; i < len; i++) {
        let c = str.charCodeAt(i);
        if (c >= 0xd800 && c <= 0xdbff) {
          const nx = i + 1 < len ? str.charCodeAt(i + 1) : 0;
          if (nx >= 0xdc00 && nx <= 0xdfff) { c = 0x10000 + ((c - 0xd800) << 10) + (nx - 0xdc00); i++; }
          else c = 0xfffd;
        } else if (c >= 0xdc00 && c <= 0xdfff) c = 0xfffd;
        n += c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
      }
      return n;
    }
    // Blueprint: Bun::byteLength (jsc/bindings/JSBuffer.cpp:154). base64/base64url
    // strip up to two '=' then return (len*3)/4 -- an UPPER BOUND, not the decoded
    // size: byteLength("!!!!AAAA","base64url") is 6 while from() yields 3 bytes.
    // Math.floor rather than >>2 because `n*3` for a >= 2^30 length would overflow
    // the int32 coercion a shift performs.
    const byteLengthFor = (str, enc) => {
      const len = str.length;
      if (len === 0) return 0;
      if (enc === "utf16le") return len * 2;
      if (enc === "latin1" || enc === "ascii") return len;
      if (enc === "hex") return len >>> 1;
      if (enc === "base64" || enc === "base64url") {
        let n = len;
        if (str.charCodeAt(n - 1) === 61) { n--; if (n > 1 && str.charCodeAt(n - 1) === 61) n--; }
        return Math.floor((n * 3) / 4);
      }
      return utf8ByteLength(str);
    };
    // Blueprint: constructBufferFromStringAndEncoding (JSBuffer.cpp:630). A
    // NON-string encoding is ignored and treated as utf8 (Buffer.from("x", 5) is
    // [120]); only an unresolvable *string* is ERR_UNKNOWN_ENCODING.
    const TE = new G.TextEncoder();
    function fromString(string, encoding) {
      let enc;
      if (typeof encoding !== "string") enc = "utf8";
      else {
        enc = parseStaticEnc(encoding);
        if (enc === undefined) throw errUnknownEncoding(encoding);
      }
      // Hot path. TextEncoder is native and already returns an exactly-sized
      // fresh Uint8Array, so re-tagging it beats sizing + filling by hand (two
      // JS passes) AND beats the old `new Buffer(TE.encode(s))`, which encoded
      // natively and then copied. Its lone-surrogate -> U+FFFD substitution is
      // the same one rawUtf8Write/utf8ByteLength implement.
      if (enc === "utf8") return asBuf(TE.encode(string));
      const cap = byteLengthFor(string, enc);
      const buf = asBuf(new Uint8Array(cap));
      if (cap === 0) return buf;
      const written = buf[STATIC_WRITE[enc]](string, 0, cap);
      if (written === cap) return buf;
      // node lib/buffer.js fromString: a lenient base64/hex decode can stop short
      // of the upper bound, so hand back a view of what was actually written.
      return asBuf(new Uint8Array(buf.buffer, buf.byteOffset, written));
    }

    const normalizeEncoding = (enc) => {
      if (enc === undefined || enc === null) return "utf8";
      return getOps(enc) ? (typeof enc === "string" ? enc.toLowerCase() : undefined) : undefined;
    };

    const validateOffset = (value, name, min, max) => {
      if (typeof value !== "number") throw errArgType(name, "number", value);
      if (!Number.isInteger(value)) throw errOutOfRange(name, "an integer", value);
      if (value < min || value > max) throw errOutOfRange(name, `>= ${min} && <= ${max}`, value);
    };

    // ------------------------------------------------ write() / toString()
    proto.write = function write(string, offset, length, encoding) {
      if (offset === undefined) {
        return this.utf8Write(string, 0, this.length);
      }
      if (length === undefined && typeof offset === "string") {
        encoding = offset;
        length = this.length;
        offset = 0;
      } else {
        validateOffset(offset, "offset", 0, this.length);
        const remaining = this.length - offset;
        if (length === undefined) length = remaining;
        else if (typeof length === "string") {
          encoding = length;
          length = remaining;
        } else {
          validateOffset(length, "length", 0, this.length);
          if (length > remaining) length = remaining;
        }
      }
      if (!encoding || encoding === "utf8") return this.utf8Write(string, offset, length);
      if (encoding === "ascii") return this.asciiWrite(string, offset, length);
      const ops = getOps(encoding);
      if (ops === undefined) throw errUnknownEncoding(encoding);
      return this[ops.write](string, offset, length);
    };

    proto.toString = function toString(encoding, start, end) {
      const len = this.length;
      const ops = encoding === undefined ? OPS.utf8 : getOps(encoding);
      if (ops === undefined) throw errUnknownEncoding(encoding);
      if (len === 0) return "";
      let s = start === undefined ? 0 : trunc0(start);
      if (s <= 0) s = 0;
      else if (s >= len) return "";
      let e = end === undefined ? len : trunc0(end);
      if (e > len) e = len;
      if (e <= s) return "";
      if (e - s > ops.cap) throw errStringTooLong();
      return this[ops.slice](s, e);
    };

    // -------------------------------------------------------------- fill()
    proto.fill = function fill(value, offset, end, encoding) {
      const buf = this;
      // Pre-coercion length: Node-compat upper bound for `end` and the default
      // write extent. The real write range is re-clamped against a single
      // post-coercion length read right before the fill (TOCTOU guard).
      const limit = buf.length;

      // Node only reinterprets a string offset/end as the encoding when the
      // fill value is itself a string; the offset-slot branch also drops end.
      if (typeof value === "string") {
        if (offset === undefined || typeof offset === "string") {
          encoding = offset;
          offset = undefined;
          end = undefined;
        } else if (typeof end === "string") {
          encoding = end;
          end = undefined;
        }
      }

      // 1. Encoding parse FIRST (string value only). Coercing an object
      // encoding runs its toString — the first user-JS-visible call, which
      // may detach/resize buf; the post-coercion clamp below catches that.
      let normalized = "utf8";
      if (typeof value === "string") {
        let encStr;
        if (encoding === undefined || encoding === null) encStr = undefined;
        else if (typeof encoding === "string") encStr = encoding.length ? encoding : undefined;
        else encStr = String(encoding);
        if (encStr !== undefined) {
          normalized = normalizeEncoding(encStr);
          if (normalized === undefined) throw errUnknownEncoding(encStr);
        }
      }

      // 2. Pure offset/end coercion (no user JS beyond validation). Node reads
      // `end` only once `offset` is present; otherwise end is ignored.
      let off = 0;
      let e = limit;
      if (offset !== undefined) {
        validateOffset(offset, "offset", 0, 0x7fffffff);
        off = offset;
        if (end !== undefined) {
          validateOffset(end, "end", 0, limit);
          e = end;
        }
      }

      // 3. Short-circuit an empty/inverted range BEFORE coercing value, so a
      // throwing valueOf / empty view / detached view stays a no-op.
      if (off >= e) return buf;

      // 4. Value coercion per branch (may detach/resize via valueOf/toString).
      let pattern;
      if (typeof value === "string") {
        if (value.length === 0) pattern = 0;
        else if (value.length === 1 && (normalized === "utf8" || normalized === "latin1" || normalized === "binary" || normalized === "ascii")) {
          const code = value.charCodeAt(0);
          if (code < 128 || normalized !== "utf8") pattern = code & 0xff;
        }
        if (pattern === undefined) {
          const tmp = OrigBuffer.alloc(Math.max(value.length * 4, 8));
          const n = tmp[OPS[normalized].write](value, 0, tmp.length);
          if (n === 0) pattern = 0;
          else pattern = tmp.subarray(0, n);
        }
      } else if (value instanceof Uint8Array) {
        if (isDetached(value)) throw new TypeError("Uint8Array is detached");
        if (value.length === 0) {
          const err = new TypeError("Buffer cannot be empty");
          err.code = "ERR_INVALID_ARG_VALUE";
          throw err;
        }
        pattern = value;
      } else {
        pattern = (+value) & 0xff;
      }

      // 5. Post-coercion clamp: re-read length once, after every side effect,
      // and fold a detach (length 0) or shrink into the range.
      const postLimit = buf.length;
      if (off > postLimit) off = postLimit;
      if (e > postLimit) e = postLimit;
      if (off >= e) return buf;

      if (typeof pattern === "number") {
        Uint8Array.prototype.fill.call(buf, pattern, off, e);
      } else if (pattern.length === 0) {
        Uint8Array.prototype.fill.call(buf, 0, off, e);
      } else {
        for (let i = off, j = 0; i < e; i++, j = (j + 1) % pattern.length)
          buf[i] = pattern[j];
      }
      return buf;
    };

    // -------------------------------------------- variable-width integers
    const checkBounds = (buf, offset, byteLength) => {
      validateNumber(offset, "offset");
      if (buf[offset] === undefined || buf[offset + byteLength] === undefined)
        boundsError(offset, buf.length - (byteLength + 1));
    };
    const checkInt = (value, min, max, buf, offset, byteLength) => {
      if (value > max || value < min) {
        const n = typeof min === "bigint" ? "n" : "";
        throw errOutOfRange("value", `>= ${min}${n} and <= ${max}${n}`, value);
      }
      checkBounds(buf, offset, byteLength);
    };
    const validateByteLength = (byteLength) => {
      if (typeof byteLength !== "number" || !Number.isInteger(byteLength) || byteLength < 1 || byteLength > 6)
        boundsError(byteLength, 6, "byteLength");
    };
    proto.writeUIntLE = function writeUIntLE(value, offset, byteLength) {
      validateByteLength(byteLength);
      value = +value;
      checkInt(value, 0, 2 ** (8 * byteLength) - 1, this, offset, byteLength - 1);
      let mul = 1;
      this[offset] = value & 0xff;
      for (let i = 1; i < byteLength; i++) {
        mul *= 0x100;
        this[offset + i] = Math.floor(value / mul) & 0xff;
      }
      return offset + byteLength;
    };
    proto.writeUIntBE = function writeUIntBE(value, offset, byteLength) {
      validateByteLength(byteLength);
      value = +value;
      checkInt(value, 0, 2 ** (8 * byteLength) - 1, this, offset, byteLength - 1);
      let mul = 1;
      this[offset + byteLength - 1] = value & 0xff;
      for (let i = byteLength - 2; i >= 0; i--) {
        mul *= 0x100;
        this[offset + i] = Math.floor(value / mul) & 0xff;
      }
      return offset + byteLength;
    };
    proto.writeIntLE = function writeIntLE(value, offset, byteLength) {
      validateByteLength(byteLength);
      value = +value;
      const limit = 2 ** (8 * byteLength - 1);
      checkInt(value, -limit, limit - 1, this, offset, byteLength - 1);
      let mul = 1, sub = 0;
      this[offset] = value & 0xff;
      for (let i = 1; i < byteLength; i++) {
        mul *= 0x100;
        if (value < 0 && sub === 0 && this[offset + i - 1] !== 0) sub = 1;
        this[offset + i] = (Math.trunc(value / mul) - sub) & 0xff;
      }
      return offset + byteLength;
    };
    proto.writeIntBE = function writeIntBE(value, offset, byteLength) {
      validateByteLength(byteLength);
      value = +value;
      const limit = 2 ** (8 * byteLength - 1);
      checkInt(value, -limit, limit - 1, this, offset, byteLength - 1);
      let mul = 1, sub = 0;
      this[offset + byteLength - 1] = value & 0xff;
      for (let i = byteLength - 2; i >= 0; i--) {
        mul *= 0x100;
        if (value < 0 && sub === 0 && this[offset + i + 1] !== 0) sub = 1;
        this[offset + i] = (Math.trunc(value / mul) - sub) & 0xff;
      }
      return offset + byteLength;
    };
    proto.readIntLE = function readIntLE(offset, byteLength) {
      validateByteLength(byteLength);
      checkBounds(this, offset, byteLength - 1);
      let val = this[offset], mul = 1;
      for (let i = 1; i < byteLength; i++) {
        mul *= 0x100;
        val += this[offset + i] * mul;
      }
      if (val >= mul * 0x80) val -= 2 ** (8 * byteLength);
      return val;
    };
    proto.readIntBE = function readIntBE(offset, byteLength) {
      validateByteLength(byteLength);
      checkBounds(this, offset, byteLength - 1);
      let val = this[offset + byteLength - 1], mul = 1;
      for (let i = byteLength - 2; i >= 0; i--) {
        mul *= 0x100;
        val += this[offset + i] * mul;
      }
      if (val >= mul * 0x80) val -= 2 ** (8 * byteLength);
      return val;
    };
    proto.writeUintLE = proto.writeUIntLE;
    proto.writeUintBE = proto.writeUIntBE;
    // Variable-width unsigned reads: node validates byteLength + offset bounds
    // before touching memory (process_web's naive versions did neither).
    proto.readUIntLE = function readUIntLE(offset, byteLength) {
      validateByteLength(byteLength);
      checkBounds(this, offset, byteLength - 1);
      let val = this[offset], mul = 1;
      for (let i = 1; i < byteLength; i++) { mul *= 0x100; val += this[offset + i] * mul; }
      return val;
    };
    proto.readUIntBE = function readUIntBE(offset, byteLength) {
      validateByteLength(byteLength);
      checkBounds(this, offset, byteLength - 1);
      let val = this[offset + byteLength - 1], mul = 1;
      for (let i = byteLength - 2; i >= 0; i--) { mul *= 0x100; val += this[offset + i] * mul; }
      return val;
    };
    proto.readUintLE = proto.readUIntLE;
    proto.readUintBE = proto.readUIntBE;

    // ---------------------------------------- fixed-width integer/float I/O
    // process_web installs naive fixed-width read/write accessors that skip
    // node's ERR_INVALID_ARG_TYPE (non-number offset) / ERR_OUT_OF_RANGE /
    // ERR_BUFFER_OUT_OF_BOUNDS argument validation. Re-install them here (this
    // partition loads after process_web) with node internal/buffer.js checks.
    const dvFixed = (buf) => new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
    proto.readUInt8 = function readUInt8(offset = 0) { checkBounds(this, offset, 0); return this[offset]; };
    proto.readInt8 = function readInt8(offset = 0) { checkBounds(this, offset, 0); const v = this[offset]; return v < 128 ? v : v - 256; };
    proto.readUInt16LE = function readUInt16LE(offset = 0) { checkBounds(this, offset, 1); return dvFixed(this).getUint16(offset, true); };
    proto.readUInt16BE = function readUInt16BE(offset = 0) { checkBounds(this, offset, 1); return dvFixed(this).getUint16(offset, false); };
    proto.readInt16LE = function readInt16LE(offset = 0) { checkBounds(this, offset, 1); return dvFixed(this).getInt16(offset, true); };
    proto.readInt16BE = function readInt16BE(offset = 0) { checkBounds(this, offset, 1); return dvFixed(this).getInt16(offset, false); };
    proto.readUInt32LE = function readUInt32LE(offset = 0) { checkBounds(this, offset, 3); return dvFixed(this).getUint32(offset, true); };
    proto.readUInt32BE = function readUInt32BE(offset = 0) { checkBounds(this, offset, 3); return dvFixed(this).getUint32(offset, false); };
    proto.readInt32LE = function readInt32LE(offset = 0) { checkBounds(this, offset, 3); return dvFixed(this).getInt32(offset, true); };
    proto.readInt32BE = function readInt32BE(offset = 0) { checkBounds(this, offset, 3); return dvFixed(this).getInt32(offset, false); };
    proto.readFloatLE = function readFloatLE(offset = 0) { checkBounds(this, offset, 3); return dvFixed(this).getFloat32(offset, true); };
    proto.readFloatBE = function readFloatBE(offset = 0) { checkBounds(this, offset, 3); return dvFixed(this).getFloat32(offset, false); };
    proto.readDoubleLE = function readDoubleLE(offset = 0) { checkBounds(this, offset, 7); return dvFixed(this).getFloat64(offset, true); };
    proto.readDoubleBE = function readDoubleBE(offset = 0) { checkBounds(this, offset, 7); return dvFixed(this).getFloat64(offset, false); };
    proto.writeUInt8 = function writeUInt8(value, offset = 0) { value = +value; checkInt(value, 0, 0xff, this, offset, 0); this[offset] = value; return offset + 1; };
    proto.writeInt8 = function writeInt8(value, offset = 0) { value = +value; checkInt(value, -0x80, 0x7f, this, offset, 0); dvFixed(this).setInt8(offset, value); return offset + 1; };
    proto.writeUInt16LE = function writeUInt16LE(value, offset = 0) { value = +value; checkInt(value, 0, 0xffff, this, offset, 1); dvFixed(this).setUint16(offset, value, true); return offset + 2; };
    proto.writeUInt16BE = function writeUInt16BE(value, offset = 0) { value = +value; checkInt(value, 0, 0xffff, this, offset, 1); dvFixed(this).setUint16(offset, value, false); return offset + 2; };
    proto.writeInt16LE = function writeInt16LE(value, offset = 0) { value = +value; checkInt(value, -0x8000, 0x7fff, this, offset, 1); dvFixed(this).setInt16(offset, value, true); return offset + 2; };
    proto.writeInt16BE = function writeInt16BE(value, offset = 0) { value = +value; checkInt(value, -0x8000, 0x7fff, this, offset, 1); dvFixed(this).setInt16(offset, value, false); return offset + 2; };
    proto.writeUInt32LE = function writeUInt32LE(value, offset = 0) { value = +value; checkInt(value, 0, 0xffffffff, this, offset, 3); dvFixed(this).setUint32(offset, value, true); return offset + 4; };
    proto.writeUInt32BE = function writeUInt32BE(value, offset = 0) { value = +value; checkInt(value, 0, 0xffffffff, this, offset, 3); dvFixed(this).setUint32(offset, value, false); return offset + 4; };
    proto.writeInt32LE = function writeInt32LE(value, offset = 0) { value = +value; checkInt(value, -0x80000000, 0x7fffffff, this, offset, 3); dvFixed(this).setInt32(offset, value, true); return offset + 4; };
    proto.writeInt32BE = function writeInt32BE(value, offset = 0) { value = +value; checkInt(value, -0x80000000, 0x7fffffff, this, offset, 3); dvFixed(this).setInt32(offset, value, false); return offset + 4; };
    proto.writeFloatLE = function writeFloatLE(value, offset = 0) { value = +value; checkBounds(this, offset, 3); dvFixed(this).setFloat32(offset, value, true); return offset + 4; };
    proto.writeFloatBE = function writeFloatBE(value, offset = 0) { value = +value; checkBounds(this, offset, 3); dvFixed(this).setFloat32(offset, value, false); return offset + 4; };
    proto.writeDoubleLE = function writeDoubleLE(value, offset = 0) { value = +value; checkBounds(this, offset, 7); dvFixed(this).setFloat64(offset, value, true); return offset + 8; };
    proto.writeDoubleBE = function writeDoubleBE(value, offset = 0) { value = +value; checkBounds(this, offset, 7); dvFixed(this).setFloat64(offset, value, false); return offset + 8; };
    // node exposes each unsigned accessor under both UInt and Uint spellings.
    for (const m of ["readUInt8", "readUInt16LE", "readUInt16BE", "readUInt32LE", "readUInt32BE",
                     "writeUInt8", "writeUInt16LE", "writeUInt16BE", "writeUInt32LE", "writeUInt32BE"])
      proto[m.replace("UInt", "Uint")] = proto[m];

    // ---------------------------------------------------- BigInt accessors
    const dvOf = (buf) => new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
    const makeBigWrite = (name, signed, le) => {
      const min = signed ? -(2n ** 63n) : 0n;
      const max = signed ? 2n ** 63n - 1n : 2n ** 64n - 1n;
      const fn = function (value, offset = 0) {
        if (typeof value !== "bigint") throw errArgType("value", "bigint", value);
        checkInt(value, min, max, this, offset, 7);
        const dv = dvOf(this);
        if (signed) dv.setBigInt64(offset, value, le);
        else dv.setBigUint64(offset, value, le);
        return offset + 8;
      };
      Object.defineProperty(fn, "name", { value: name, configurable: true });
      return fn;
    };
    const makeBigRead = (name, signed, le) => {
      const fn = function (offset = 0) {
        checkBounds(this, offset, 7);
        const dv = dvOf(this);
        return signed ? dv.getBigInt64(offset, le) : dv.getBigUint64(offset, le);
      };
      Object.defineProperty(fn, "name", { value: name, configurable: true });
      return fn;
    };
    proto.writeBigInt64LE = makeBigWrite("writeBigInt64LE", true, true);
    proto.writeBigInt64BE = makeBigWrite("writeBigInt64BE", true, false);
    proto.writeBigUInt64LE = makeBigWrite("writeBigUInt64LE", false, true);
    proto.writeBigUInt64BE = makeBigWrite("writeBigUInt64BE", false, false);
    proto.readBigInt64LE = makeBigRead("readBigInt64LE", true, true);
    proto.readBigInt64BE = makeBigRead("readBigInt64BE", true, false);
    proto.readBigUInt64LE = makeBigRead("readBigUInt64LE", false, true);
    proto.readBigUInt64BE = makeBigRead("readBigUInt64BE", false, false);
    proto.writeBigUint64LE = proto.writeBigUInt64LE;
    proto.writeBigUint64BE = proto.writeBigUInt64BE;
    proto.readBigUint64LE = proto.readBigUInt64LE;
    proto.readBigUint64BE = proto.readBigUInt64BE;

    // ---------------------------------------------------------- swaps etc.
    proto.swap16 = function swap16() {
      const len = this.length;
      if (len % 2 !== 0) throw errInvalidBufferSize(16);
      for (let i = 0; i < len; i += 2) { const t = this[i]; this[i] = this[i + 1]; this[i + 1] = t; }
      return this;
    };
    proto.swap32 = function swap32() {
      const len = this.length;
      if (len % 4 !== 0) throw errInvalidBufferSize(32);
      for (let i = 0; i < len; i += 4) {
        let t = this[i]; this[i] = this[i + 3]; this[i + 3] = t;
        t = this[i + 1]; this[i + 1] = this[i + 2]; this[i + 2] = t;
      }
      return this;
    };
    proto.swap64 = function swap64() {
      const len = this.length;
      if (len % 8 !== 0) throw errInvalidBufferSize(64);
      for (let i = 0; i < len; i += 8)
        for (let j = 0; j < 4; j++) { const t = this[i + j]; this[i + j] = this[i + 7 - j]; this[i + 7 - j] = t; }
      return this;
    };
    const INSPECT_MAX = { value: 50 };
    proto.inspect = function inspect() {
      const max = INSPECT_MAX.value;
      let s = rawHexSlice(this, 0, Math.min(this.length, max)).replace(/(.{2})(?=.)/g, "$1 ");
      if (this.length > max) s += " ... " + (this.length - max) + " more bytes";
      return "<Buffer" + (s ? " " + s : "") + ">";
    };

    // ---------------------------------------- Buffer.prototype.compare
    // node lib/buffer.js: validate end offsets against buffer length
    // (ERR_OUT_OF_RANGE) BEFORE the start>=end early return; start offsets have
    // no upper bound. process_web.cppm's base compare clamps via subarray and
    // never throws.
    proto.compare = function compare(target, targetStart, targetEnd, sourceStart, sourceEnd) {
      if (!isU8(target))
        throw errArgInstance("target", "Buffer or Uint8Array", target);
      const src = this;
      // node validateOffset: number + integer + range, NO coercion (a string or
      // { valueOf } offset throws ERR_INVALID_ARG_TYPE / ERR_OUT_OF_RANGE).
      if (targetStart === undefined) targetStart = 0;
      else validateOffset(targetStart, "targetStart", 0, 0x7fffffff);
      if (targetEnd === undefined) targetEnd = target.length;
      else validateOffset(targetEnd, "targetEnd", 0, target.length);
      if (sourceStart === undefined) sourceStart = 0;
      else validateOffset(sourceStart, "sourceStart", 0, 0x7fffffff);
      if (sourceEnd === undefined) sourceEnd = src.length;
      else validateOffset(sourceEnd, "sourceEnd", 0, src.length);
      if (sourceStart >= sourceEnd) return targetStart >= targetEnd ? 0 : -1;
      if (targetStart >= targetEnd) return 1;
      const a = src.subarray(sourceStart, sourceEnd), b = target.subarray(targetStart, targetEnd);
      const n = Math.min(a.length, b.length);
      for (let i = 0; i < n; i++) { if (a[i] < b[i]) return -1; if (a[i] > b[i]) return 1; }
      return a.length < b.length ? -1 : a.length > b.length ? 1 : 0;
    };
    // node validates the argument type; process_web's base equals did not.
    proto.equals = function equals(otherBuffer) {
      if (!isU8(otherBuffer))
        throw errArgInstance("otherBuffer", "Buffer or Uint8Array", otherBuffer);
      if (this === otherBuffer) return true;
      if (this.length !== otherBuffer.length) return false;
      for (let i = 0; i < this.length; i++) if (this[i] !== otherBuffer[i]) return false;
      return true;
    };
    // ----------------------------------- Buffer.from(ab, byteOffset, length)
    const asBuf = (u8) => { Object.setPrototypeOf(u8, proto); return u8; };
    const isAnyArrayBuffer = (v) => {
      if (v === null || typeof v !== "object") return false;
      try { Object.getOwnPropertyDescriptor(ArrayBuffer.prototype, "byteLength").get.call(v); return true; } catch (_) {}
      if (typeof G.SharedArrayBuffer === "function") {
        try { Object.getOwnPropertyDescriptor(G.SharedArrayBuffer.prototype, "byteLength").get.call(v); return true; } catch (_) {}
      }
      return false;
    };
    function fromArrayBuffer(obj, byteOffset, length) {
      // node lib/buffer.js fromArrayBuffer: NaN offset -> 0; bounds compare the
      // UN-truncated offset/length; NaN or <= 0 length clamps to 0; no length
      // makes a length-tracking view.
      if (byteOffset === undefined) byteOffset = 0;
      else { byteOffset = +byteOffset; if (Number.isNaN(byteOffset)) byteOffset = 0; }
      const maxLength = obj.byteLength - byteOffset;
      if (maxLength < 0) throw errBufferOOB("offset");
      if (length === undefined) return asBuf(new Uint8Array(obj, byteOffset));
      length = +length;
      if (length > 0) {
        if (length > maxLength) throw errBufferOOB("length");
      } else length = 0;
      return asBuf(new Uint8Array(obj, byteOffset, length));
    }
    const origFrom = OrigBuffer.from;
    // Strings route through fromString, NOT origFrom: the underlying class only
    // knows hex/base64/utf16le/latin1 and silently utf8-encodes anything else,
    // so Buffer.from(s, "base64url") used to return the source text's bytes.
    const newFrom = function from(value, encodingOrOffset, length) {
      if (typeof value === "string") return fromString(value, encodingOrOffset);
      if (isAnyArrayBuffer(value)) return fromArrayBuffer(value, encodingOrOffset, length);
      // node lib/buffer.js Buffer.from(object): valueOf coercion, then (array-like
      // handled by origFrom below), then Symbol.toPrimitive('string').
      if (value !== null && typeof value === "object" &&
          value.length === undefined && !ArrayBuffer.isView(value) &&
          !(value.type === "Buffer" && Array.isArray(value.data))) {
        const vo = typeof value.valueOf === "function" ? value.valueOf() : value;
        if (vo !== null && vo !== value && (typeof vo === "string" || typeof vo === "object"))
          return from(vo, encodingOrOffset, length);
        const sp = value[Symbol.toPrimitive];
        if (typeof sp === "function") {
          const prim = sp.call(value, "string");
          if (typeof prim === "string") return fromString(prim, encodingOrOffset);
        }
      }
      return origFrom.call(OrigBuffer, value, encodingOrOffset, length);
    };

    // Thin callable wrapper sharing OrigBuffer.prototype so the deprecated
    // `new Buffer(str, enc)` / `new Buffer(ab, offset, length)` forms take the
    // same paths as Buffer.from.
    const BufferW = function Buffer(value, encodingOrOffset, length) {
      if (typeof value === "string") return fromString(value, encodingOrOffset);
      if (isAnyArrayBuffer(value)) return fromArrayBuffer(value, encodingOrOffset, length);
      if (new.target) return Reflect.construct(OrigBuffer, [value, encodingOrOffset, length]);
      return OrigBuffer(value, encodingOrOffset, length);
    };
    for (const k of Object.getOwnPropertyNames(OrigBuffer)) {
      if (k === "prototype" || k === "name" || k === "length" || k === "arguments" || k === "caller") continue;
      try { BufferW[k] = OrigBuffer[k]; } catch (_) {}
    }
    BufferW.prototype = proto;
    BufferW.from = newFrom;
    // MUST be assigned after the copy loop above, which clobbers it with the
    // underlying class's one-argument byteLength (it drops `encoding` entirely).
    // Blueprint: jsBufferConstructorFunction_byteLengthBody (JSBuffer.cpp:793).
    // parseEnumeration is NON-throwing here: an unknown or absent encoding falls
    // back to utf8, so Buffer.byteLength("x","bogus") is 1 even though
    // Buffer.from("x","bogus") throws ERR_UNKNOWN_ENCODING on the same argument.
    BufferW.byteLength = function byteLength(string, encoding) {
      if (typeof string !== "string") {
        if (isAnyArrayBuffer(string) || ArrayBuffer.isView(string)) return string.byteLength;
        throw errArgType("string", "string or an instance of Buffer or ArrayBuffer", string);
      }
      const enc = parseStaticEnc(encoding);
      return byteLengthFor(string, enc === undefined ? "utf8" : enc);
    };
    BufferW.poolSize = 8192;
    BufferW.allocUnsafeSlow = function allocUnsafeSlow(size) { return OrigBuffer.allocUnsafe(size); };
    // Same resolver as from()/byteLength() -- bun's isEncoding is a bare
    // parseEnumeration<BufferEncodingType> null-check (JSBuffer.cpp).
    BufferW.isEncoding = function isEncoding(encoding) {
      return parseStaticEnc(encoding) !== undefined;
    };
    BufferW.copyBytesFrom = function copyBytesFrom(view, offset, length) {
      const isTA = (() => {
        try {
          return Object.getOwnPropertyDescriptor(Object.getPrototypeOf(Uint8Array.prototype), Symbol.toStringTag)
            .get.call(view) !== undefined;
        } catch (_) { return false; }
      })();
      if (!isTA) throw errArgType("view", "TypedArray", view);
      const viewLength = view.length;
      if (offset !== undefined) {
        if (typeof offset !== "number") throw errArgType("offset", "number", offset);
        if (!Number.isInteger(offset) || offset < 0) throw errOutOfRange("offset", ">= 0", offset);
      } else offset = 0;
      if (offset >= viewLength) return OrigBuffer.alloc(0);
      let endEl;
      if (length !== undefined) {
        if (typeof length !== "number") throw errArgType("length", "number", length);
        if (!Number.isInteger(length) || length < 0) throw errOutOfRange("length", ">= 0", length);
        endEl = Math.min(viewLength, offset + length);
      } else endEl = viewLength;
      const sub = view.subarray(offset, endEl);
      const bytes = new Uint8Array(sub.buffer, sub.byteOffset, sub.length * (view.BYTES_PER_ELEMENT || 1));
      const out = OrigBuffer.allocUnsafe(bytes.length);
      out.set(bytes);
      return out;
    };
    Object.defineProperty(BufferW, "name", { value: "Buffer", configurable: true });
    // node's Buffer extends Uint8Array, so it inherits %TypedArray%'s
    // `get [Symbol.species]() { return this; }` and `Buffer[Symbol.species] ===
    // Buffer`. This wrapper is a plain function, so the accessor has to be
    // restated — npm `ws` binds `const FastBuffer = Buffer[Symbol.species]` at
    // load time and constructs every parsed frame slice through it
    // (ws/lib/receiver.js consume()), which throws on an undefined species.
    Object.defineProperty(BufferW, Symbol.species, { get() { return this; }, configurable: true });
    // Buffer.of(...items) — the %TypedArray%.of analogue, returns a Buffer of the
    // given byte values. ref: node lib/buffer.js Buffer.of.
    BufferW.of = function of(...items) { return newFrom(items); };

    // Buffer.compare(buf1, buf2): node type-checks both args (process_web's
    // static coerced a non-Buffer via Buffer.from and never threw).
    BufferW.compare = function compare(buf1, buf2) {
      if (!isU8(buf1)) throw errArgInstance("buf1", "Buffer or Uint8Array", buf1);
      if (!isU8(buf2)) throw errArgInstance("buf2", "Buffer or Uint8Array", buf2);
      if (buf1 === buf2) return 0;
      return proto.compare.call(buf1, buf2);
    };
    // Buffer.concat(list[, totalLength]): validate list is an Array and every
    // entry a Buffer/Uint8Array; totalLength must be a non-negative integer.
    // Sizes/copies use byteLength so a spoofed `.length` getter cannot expose
    // uninitialized memory (test-buffer-concat).
    const kMaxLength = 0x7fffffff;
    BufferW.concat = function concat(list, length) {
      if (!Array.isArray(list)) throw errArgInstance("list", "Array", list);
      if (list.length === 0) return OrigBuffer.alloc(0);
      let total;
      if (length === undefined) {
        total = 0;
        for (let i = 0; i < list.length; i++) total += (list[i] && list[i].byteLength) || 0;
      } else {
        validateOffset(length, "length", 0, kMaxLength);
        total = length;
      }
      const buffer = OrigBuffer.allocUnsafe(total);
      let pos = 0;
      for (let i = 0; i < list.length; i++) {
        const buf = list[i];
        if (!isU8(buf)) throw errArgInstance("list[" + i + "]", "Buffer or Uint8Array", buf);
        if (pos >= total) continue;
        const take = Math.min(buf.byteLength, total - pos);
        if (take > 0) { buffer.set(buf.subarray(0, take), pos); pos += take; }
      }
      if (pos < total) buffer.fill(0, pos);
      return buffer;
    };

    G.Buffer = BufferW;

    // ------------------------------------------------------ module exports
    const isAscii = function isAscii(input) {
      let u8;
      if (isAnyArrayBuffer(input)) u8 = new Uint8Array(input);
      else if (ArrayBuffer.isView(input)) u8 = new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
      else throw errArgType("input", "ArrayBuffer, Buffer, or TypedArray", input);
      for (let i = 0; i < u8.length; i++) if (u8[i] > 127) return false;
      return true;
    };
    const isUtf8 = function isUtf8(input) {
      let u8;
      if (isAnyArrayBuffer(input)) u8 = new Uint8Array(input);
      else if (ArrayBuffer.isView(input)) u8 = new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
      else throw errArgType("input", "ArrayBuffer, Buffer, or TypedArray", input);
      try { new TextDecoder("utf-8", { fatal: true }).decode(u8); return true; } catch (_) { return false; }
    };
    const SlowBuffer = function SlowBuffer(size) { return OrigBuffer.allocUnsafe(size); };
    if (M) {
      const mod = M["buffer"] || M["node:buffer"];
      if (mod) {
        mod.Buffer = BufferW;
        mod.isAscii = isAscii;
        mod.isUtf8 = isUtf8;
        mod.SlowBuffer = SlowBuffer;
        mod.atob = G.atob;
        mod.btoa = G.btoa;
        // node:buffer re-exports the global Blob and resolveObjectURL
        // (ref: nodejs lib/buffer.js). resolveObjectURL is installed on the
        // global by the bootstrap URL registry.
        if (G.Blob) mod.Blob = G.Blob;
        if (typeof G.__bunResolveObjectURL === "function") mod.resolveObjectURL = G.__bunResolveObjectURL;
        if (mod.INSPECT_MAX_BYTES === undefined) {
          Object.defineProperty(mod, "INSPECT_MAX_BYTES", {
            get: () => INSPECT_MAX.value,
            set: (v) => { INSPECT_MAX.value = v; },
            enumerable: true,
            configurable: true,
          });
        }
        M["buffer"] = M["node:buffer"] = mod;
      }
    }
  } catch (e) {}
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
