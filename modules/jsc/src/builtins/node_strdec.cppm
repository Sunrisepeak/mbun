// node:string_decoder + node:querystring JS payload partition.
//
// Faithful ports of two node blueprints:
//   * string_decoder — node lib/string_decoder.js: the lastNeed/lastTotal/
//     lastChar state machine (utf8CheckByte/CheckIncomplete/CheckExtraBytes,
//     utf16 surrogate carry, base64/base64url 3-byte carry). StringDecoder is a
//     plain function (not a class) so the test's RealStringDecoder.apply(this)
//     + util.inherits keep working.
//   * querystring — bun-ref src/js/node/querystring.ts (itself node's port):
//     parse/stringify/escape/unescape/encode/decode with maxKeys, custom
//     sep/eq, and the encodeCheck fast-path. Primordial .$call calls are folded
//     to direct method calls (self-contained shard, no tamper surface here).
//
// NOTE: appended AFTER the master builtins IIFE has closed (see image_closure),
// so this is a self-contained IIFE that re-binds G = globalThis and overrides
// the weak string_decoder/querystring stubs registered earlier in bootstrap.
export module mbun.jsc.js_builtins:node_strdec;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kNodeStrDecJS = R"JS(
(function () {
  const G = globalThis;
  const M = G.__mbunNativeModules;
  if (!M) return;
  // Buffer is not yet on globalThis when this master script evaluates; touch it
  // lazily (all uses below run at user-call time, when G.Buffer exists).
  const reg = (nm, mod) => { M[nm] = mod; M["node:" + nm] = mod; };

  // ----------------------------------------------------------- string_decoder
  // 1:1 port of bun src/jsc/bindings/JSStringDecoder.cpp: the native decoder
  // resets lastNeed/lastTotal on end() (ResetScope) but keeps lastChar, flushes
  // an incomplete tail by *decoding the buffered bytes* (so invalid sequences
  // can yield multiple U+FFFD), and uses one unified write/text/fillLast switch.
  function isEncoding(encoding) {
    switch (String(encoding).toLowerCase()) {
      case "hex": case "utf8": case "utf-8": case "ascii": case "binary":
      case "base64": case "base64url": case "ucs2": case "ucs-2":
      case "utf16le": case "utf-16le": case "latin1":
        return true;
      default:
        return false;
    }
  }

  function _normalizeEncoding(enc) {
    if (!enc) return "utf8";
    let retried;
    for (;;) {
      switch (enc) {
        case "utf8": case "utf-8": return "utf8";
        case "ucs2": case "ucs-2": case "utf16le": case "utf-16le": return "utf16le";
        case "latin1": case "binary": return "latin1";
        case "base64": case "base64url": case "ascii": case "hex": return enc;
        default:
          if (retried) return undefined;
          enc = ("" + enc).toLowerCase();
          retried = true;
      }
    }
  }

  function normalizeEncoding(enc) {
    const nenc = _normalizeEncoding(enc);
    if (typeof nenc !== "string" && !isEncoding(enc)) {
      const e = new TypeError("Unknown encoding: " + enc);
      e.code = "ERR_UNKNOWN_ENCODING";
      throw e;
    }
    return nenc || enc;
  }

  function utf8CheckByte(byte) {
    if (byte <= 0x7f) return 0;
    else if (byte >> 5 === 0x06) return 2;
    else if (byte >> 4 === 0x0e) return 3;
    else if (byte >> 3 === 0x1e) return 4;
    return byte >> 6 === 0x02 ? -1 : -2;
  }

  function isContinuation(byte) { return (byte & 0xc0) === 0x80; }

  // Mutates self.lastNeed; `length` is an explicit bound (buf may be lastChar).
  function utf8CheckIncomplete(self, buf, length, i) {
    let j = length - 1;
    if (j < i) return 0;
    let nb = utf8CheckByte(buf[j]);
    if (nb >= 0) {
      if (nb > 0) self.lastNeed = nb - 1;
      return nb;
    }
    if (j === 0 || --j < i || nb === -2) return 0;
    nb = utf8CheckByte(buf[j]);
    if (nb >= 0) {
      if (nb > 0) self.lastNeed = nb - 2;
      return nb;
    }
    if (j === 0 || --j < i || nb === -2) return 0;
    nb = utf8CheckByte(buf[j]);
    if (nb >= 0) {
      if (nb > 0) {
        if (nb === 2) nb = 0;
        else self.lastNeed = nb - 3;
      }
      return nb;
    }
    return 0;
  }

  function fillLast(self, buf) {
    const enc = self.encoding, lc = self.lastChar, length = buf.length;
    if (enc === "utf8") {
      const max = Math.min(length, self.lastNeed);
      for (let i = 0; i < max; i++) {
        if (!isContinuation(buf[i])) {
          const chars = self.lastTotal - self.lastNeed + i;
          buf.copy(lc, self.lastTotal - self.lastNeed, 0, i);
          self.lastNeed = i;
          return lc.toString(enc, 0, chars);
        }
      }
    }
    if (self.lastNeed <= length) {
      buf.copy(lc, self.lastTotal - self.lastNeed, 0, self.lastNeed);
      return lc.toString(enc, 0, self.lastTotal);
    }
    buf.copy(lc, self.lastTotal - self.lastNeed, 0, length);
    if (enc === "utf8") {
      const lastLastNeed = self.lastNeed;
      const total = utf8CheckIncomplete(self, lc, self.lastTotal - lastLastNeed + length, 0);
      if (total === 0) {
        const len = self.lastTotal - self.lastNeed + length;
        self.lastNeed = length;
        return lc.toString(enc, 0, len);
      }
      self.lastNeed = lastLastNeed;
    }
    self.lastNeed -= length;
    return "";
  }

  function text(self, buf, length, offset) {
    const enc = self.encoding, lc = self.lastChar;
    if (enc === "utf16le") {
      if (length === offset) return "";
      if ((length - offset) % 2 === 0) {
        const c = (buf[length - 1] << 8) + buf[length - 2];
        if (c >= 0xd800 && c <= 0xdbff) {
          self.lastNeed = 2;
          self.lastTotal = 4;
          lc[0] = buf[length - 2];
          lc[1] = buf[length - 1];
          return buf.toString(enc, offset, length - 2);
        }
        return buf.toString(enc, offset, length);
      }
      self.lastNeed = 1;
      self.lastTotal = 2;
      lc[0] = buf[length - 1];
      return buf.toString(enc, offset, length - 1);
    }
    if (enc === "utf8") {
      const total = utf8CheckIncomplete(self, buf, length, offset);
      if (!self.lastNeed) return buf.toString(enc, offset, length);
      self.lastTotal = total;
      const end = length - (total - self.lastNeed);
      if (end < length) buf.copy(lc, 0, end, end + Math.min(4, length - end));
      return buf.toString(enc, offset, end);
    }
    // base64 / base64url
    const n = (length - offset) % 3;
    if (n === 0) return buf.toString(enc, offset, length);
    self.lastNeed = 3 - n;
    self.lastTotal = 3;
    if (n === 1) {
      lc[0] = buf[length - 1];
    } else {
      lc[0] = buf[length - 2];
      lc[1] = buf[length - 1];
    }
    return buf.toString(enc, offset, length - n);
  }

  function isMultiByte(enc) {
    return enc === "utf16le" || enc === "utf8" || enc === "base64" || enc === "base64url";
  }

  function decWrite(self, buf) {
    const length = buf.length;
    if (length === 0) return "";
    const enc = self.encoding;
    if (!isMultiByte(enc)) return buf.toString(enc);
    let offset = 0;
    if (self.lastNeed) {
      const firstHalf = fillLast(self, buf);
      if (firstHalf.length === 0) return firstHalf;
      offset = self.lastNeed;
      self.lastNeed = 0;
      self.lastTotal = 0;
      if (offset === length) return firstHalf;
      const secondHalf = text(self, buf, length, offset);
      if (secondHalf.length === 0) return firstHalf;
      return firstHalf + secondHalf;
    }
    return text(self, buf, length, 0);
  }

  function decEnd(self, buf) {
    const enc = self.encoding, lc = self.lastChar;
    const hasBuf = buf && buf.length > 0;
    try {
      if (enc === "utf16le" || enc === "utf8") {
        if (!hasBuf) return self.lastNeed ? lc.toString(enc, 0, self.lastTotal - self.lastNeed) : "";
        const firstHalf = decWrite(self, buf);
        if (self.lastNeed) return firstHalf + lc.toString(enc, 0, self.lastTotal - self.lastNeed);
        return firstHalf;
      }
      if (enc === "base64" || enc === "base64url") {
        if (!hasBuf) return self.lastNeed ? lc.toString(enc, 0, 3 - self.lastNeed) : "";
        const firstHalf = decWrite(self, buf);
        if (self.lastNeed) return firstHalf + lc.toString(enc, 0, 3 - self.lastNeed);
        return firstHalf;
      }
      return hasBuf ? decWrite(self, buf) : "";
    } finally {
      // ResetScope: clear MissingBytes/BufferedBytes, keep lastChar intact.
      self.lastTotal = 0;
      self.lastNeed = 0;
    }
  }

  function StringDecoder(encoding) {
    this.encoding = normalizeEncoding(encoding);
    this.lastNeed = 0;
    this.lastTotal = 0;
    // Native lastChar is a zero-initialized uint8_t[4] for every encoding.
    this.lastChar = G.Buffer.alloc(4);
  }

  // The native decoder reads the argument as raw bytes, so ANY TypedArray /
  // DataView must behave like a Buffer here. Without this the decode path fell
  // through to `Uint8Array.prototype.toString()`, which joins the bytes with
  // commas ("76,111,97,..."), and `buf.copy` (Buffer-only) was missing entirely
  // — see cli/hot/watch-many-dirs.test.ts, which decodes `proc.stdout` chunks
  // (plain Uint8Array, not Buffer).
  function asBuffer(buf) {
    if (G.Buffer.isBuffer(buf)) return buf;
    return G.Buffer.from(buf.buffer, buf.byteOffset, buf.byteLength);
  }

  StringDecoder.prototype.write = function (buf) {
    if (typeof buf === "string") return buf;
    if (buf == null || typeof buf.byteLength !== "number") {
      const e = new TypeError('The "buf" argument must be an instance of Buffer, TypedArray, or DataView.');
      e.code = "ERR_INVALID_ARG_TYPE";
      throw e;
    }
    return decWrite(this, asBuffer(buf));
  };

  StringDecoder.prototype.end = function (buf) {
    return decEnd(this, buf == null ? buf : asBuffer(buf));
  };

  StringDecoder.prototype.text = function (buf, offset) {
    offset = offset | 0;
    const byteLength = buf.byteLength;
    if (offset < 0 || offset > byteLength) return "";
    return decWrite(this, asBuffer(buf).subarray(offset));
  };

  reg("string_decoder", { StringDecoder });

  // --------------------------------------------------------------- querystring
  const ArrayIsArray = Array.isArray;
  const MathAbs = Math.abs;
  const NumberIsFinite = Number.isFinite;
  const ObjectKeys = Object.keys;
  function $ERR_INVALID_URI() {
    const e = new URIError("URI malformed");
    e.code = "ERR_INVALID_URI";
    return e;
  }

  function encodeStr(str, noEscapeTable, hexTable) {
    const len = str.length;
    if (len === 0) return "";
    let out = "";
    let lastPos = 0;
    let i = 0;
    outer: for (; i < len; i++) {
      let c = str.charCodeAt(i);
      while (c < 0x80) {
        if (noEscapeTable[c] !== 1) {
          if (lastPos < i) out += str.slice(lastPos, i);
          lastPos = i + 1;
          out += hexTable[c];
        }
        if (++i === len) break outer;
        c = str.charCodeAt(i);
      }
      if (lastPos < i) out += str.slice(lastPos, i);
      if (c < 0x800) {
        lastPos = i + 1;
        out += hexTable[0xc0 | (c >> 6)] + hexTable[0x80 | (c & 0x3f)];
        continue;
      }
      if (c < 0xd800 || c >= 0xe000) {
        lastPos = i + 1;
        out += hexTable[0xe0 | (c >> 12)] + hexTable[0x80 | ((c >> 6) & 0x3f)] + hexTable[0x80 | (c & 0x3f)];
        continue;
      }
      ++i;
      if (i >= len) throw $ERR_INVALID_URI();
      const c2 = str.charCodeAt(i) & 0x3ff;
      lastPos = i + 1;
      c = 0x10000 + (((c & 0x3ff) << 10) | c2);
      out +=
        hexTable[0xf0 | (c >> 18)] +
        hexTable[0x80 | ((c >> 12) & 0x3f)] +
        hexTable[0x80 | ((c >> 6) & 0x3f)] +
        hexTable[0x80 | (c & 0x3f)];
    }
    if (lastPos === 0) return str;
    if (lastPos < len) return out + str.slice(lastPos);
    return out;
  }

  const hexTable = new Array(256);
  for (let i = 0; i < 256; ++i) hexTable[i] = "%" + ((i < 16 ? "0" : "") + i.toString(16)).toUpperCase();
  const isHexTable = new Int8Array([
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0,
    0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  ]);

  const QueryString = {
    unescapeBuffer,
    unescape: qsUnescape,
    escape: qsEscape,
    stringify,
    encode: undefined,
    parse,
    decode: undefined,
  };

  const unhexTable = new Int8Array([
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    +0, +1, +2, +3, +4, +5, +6, +7, +8, +9, -1, -1, -1, -1, -1, -1,
    -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  ]);

  function unescapeBuffer(s, decodeSpaces) {
    const out = G.Buffer.allocUnsafe(s.length);
    let index = 0;
    let outIndex = 0;
    let currentChar;
    let nextChar;
    let hexHigh;
    let hexLow;
    const maxLength = s.length - 2;
    let hasHex = false;
    while (index < s.length) {
      currentChar = s.charCodeAt(index);
      if (currentChar === 43 && decodeSpaces) {
        out[outIndex++] = 32;
        index++;
        continue;
      }
      if (currentChar === 37 && index < maxLength) {
        currentChar = s.charCodeAt(++index);
        hexHigh = unhexTable[currentChar];
        if (!(hexHigh >= 0)) {
          out[outIndex++] = 37;
          continue;
        } else {
          nextChar = s.charCodeAt(++index);
          hexLow = unhexTable[nextChar];
          if (!(hexLow >= 0)) {
            out[outIndex++] = 37;
            index--;
          } else {
            hasHex = true;
            currentChar = hexHigh * 16 + hexLow;
          }
        }
      }
      out[outIndex++] = currentChar;
      index++;
    }
    return hasHex ? out.slice(0, outIndex) : out;
  }

  function qsUnescape(s, decodeSpaces) {
    try {
      return decodeURIComponent(s);
    } catch {
      return QueryString.unescapeBuffer(s, decodeSpaces).toString();
    }
  }

  const noEscape = new Int8Array([
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0,
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1,
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 0,
  ]);

  function qsEscape(str) {
    if (typeof str !== "string") {
      if (typeof str === "object") str = String(str);
      else str += "";
    }
    return encodeStr(str, noEscape, hexTable);
  }

  function stringifyPrimitive(v) {
    if (typeof v === "string") return v;
    if (typeof v === "number" && NumberIsFinite(v)) return "" + v;
    if (typeof v === "bigint") return "" + v;
    if (typeof v === "boolean") return v ? "true" : "false";
    return "";
  }

  function encodeStringified(v, encode) {
    if (typeof v === "string") return v.length ? encode(v) : "";
    if (typeof v === "number" && NumberIsFinite(v)) {
      return MathAbs(v) < 1e21 ? "" + v : encode("" + v);
    }
    if (typeof v === "bigint") return "" + v;
    if (typeof v === "boolean") return v ? "true" : "false";
    return "";
  }

  function encodeStringifiedCustom(v, encode) {
    return encode(stringifyPrimitive(v));
  }

  function stringify(obj, sep, eq, options) {
    sep ||= "&";
    eq ||= "=";
    let encode = QueryString.escape;
    if (options) {
      const encodeURIComponentOption = options.encodeURIComponent;
      if (typeof encodeURIComponentOption === "function") {
        encode = encodeURIComponentOption;
      }
    }
    const convert = encode === qsEscape ? encodeStringified : encodeStringifiedCustom;
    if (obj !== null && typeof obj === "object") {
      const keys = ObjectKeys(obj);
      const len = keys.length;
      let fields = "";
      for (let i = 0; i < len; ++i) {
        const k = keys[i];
        const v = obj[k];
        let ks = convert(k, encode);
        ks += eq;
        if (ArrayIsArray(v)) {
          const vlen = v.length;
          if (vlen === 0) continue;
          if (fields) fields += sep;
          for (let j = 0; j < vlen; ++j) {
            if (j) fields += sep;
            fields += ks;
            fields += convert(v[j], encode);
          }
        } else {
          if (fields) fields += sep;
          fields += ks;
          fields += convert(v, encode);
        }
      }
      return fields;
    }
    return "";
  }

  function charCodes(str) {
    if (str.length === 0) return [];
    if (str.length === 1) return [str.charCodeAt(0)];
    const ret = new Array(str.length);
    for (let i = 0; i < str.length; ++i) ret[i] = str.charCodeAt(i);
    return ret;
  }
  const defSepCodes = [38];
  const defEqCodes = [61];

  function addKeyVal(obj, key, value, keyEncoded, valEncoded, decode) {
    if (key.length > 0 && keyEncoded) key = decodeStr(key, decode);
    if (value.length > 0 && valEncoded) value = decodeStr(value, decode);
    if (obj[key] === undefined) {
      obj[key] = value;
    } else {
      const curValue = obj[key];
      if (curValue.pop) curValue[curValue.length] = value;
      else obj[key] = [curValue, value];
    }
  }

  function parse(qs, sep, eq, options) {
    const obj = Object.create(null);
    if (typeof qs !== "string" || qs.length === 0) {
      return obj;
    }
    const sepCodes = !sep ? defSepCodes : charCodes(String(sep));
    const eqCodes = !eq ? defEqCodes : charCodes(String(eq));
    const sepLen = sepCodes.length;
    const eqLen = eqCodes.length;
    let pairs = 1000;
    let decode = QueryString.unescape;
    if (options) {
      const maxKeys = options.maxKeys;
      const decodeURIComponentOption = options.decodeURIComponent;
      if (typeof maxKeys === "number") {
        pairs = maxKeys > 0 ? maxKeys : -1;
      }
      if (typeof decodeURIComponentOption === "function") {
        decode = decodeURIComponentOption;
      }
    }
    const customDecode = decode !== qsUnescape;
    let lastPos = 0;
    let sepIdx = 0;
    let eqIdx = 0;
    let key = "";
    let value = "";
    let keyEncoded = customDecode;
    let valEncoded = customDecode;
    const plusChar = customDecode ? "%20" : " ";
    let encodeCheck = 0;
    for (let i = 0; i < qs.length; ++i) {
      const code = qs.charCodeAt(i);
      if (code === sepCodes[sepIdx]) {
        if (++sepIdx === sepLen) {
          const end = i - sepIdx + 1;
          if (eqIdx < eqLen) {
            if (lastPos < end) {
              key += qs.slice(lastPos, end);
            } else if (key.length === 0) {
              if (--pairs === 0) return obj;
              lastPos = i + 1;
              sepIdx = eqIdx = 0;
              continue;
            }
          } else if (lastPos < end) {
            value += qs.slice(lastPos, end);
          }
          addKeyVal(obj, key, value, keyEncoded, valEncoded, decode);
          if (--pairs === 0) return obj;
          keyEncoded = valEncoded = customDecode;
          key = value = "";
          encodeCheck = 0;
          lastPos = i + 1;
          sepIdx = eqIdx = 0;
        }
      } else {
        sepIdx = 0;
        if (eqIdx < eqLen) {
          if (code === eqCodes[eqIdx]) {
            if (++eqIdx === eqLen) {
              const end = i - eqIdx + 1;
              if (lastPos < end) key += qs.slice(lastPos, end);
              encodeCheck = 0;
              lastPos = i + 1;
            }
            continue;
          } else {
            eqIdx = 0;
            if (!keyEncoded) {
              if (code === 37) {
                encodeCheck = 1;
                continue;
              } else if (encodeCheck > 0) {
                if (isHexTable[code] === 1) {
                  if (++encodeCheck === 3) keyEncoded = true;
                  continue;
                } else {
                  encodeCheck = 0;
                }
              }
            }
          }
          if (code === 43) {
            if (lastPos < i) key += qs.slice(lastPos, i);
            key += plusChar;
            lastPos = i + 1;
            continue;
          }
        }
        if (code === 43) {
          if (lastPos < i) value += qs.slice(lastPos, i);
          value += plusChar;
          lastPos = i + 1;
        } else if (!valEncoded) {
          if (code === 37) {
            encodeCheck = 1;
          } else if (encodeCheck > 0) {
            if (isHexTable[code] === 1) {
              if (++encodeCheck === 3) valEncoded = true;
            } else {
              encodeCheck = 0;
            }
          }
        }
      }
    }
    if (lastPos < qs.length) {
      if (eqIdx < eqLen) key += qs.slice(lastPos);
      else if (sepIdx < sepLen) value += qs.slice(lastPos);
    } else if (eqIdx === 0 && key.length === 0) {
      return obj;
    }
    addKeyVal(obj, key, value, keyEncoded, valEncoded, decode);
    return obj;
  }

  function decodeStr(s, decoder) {
    try {
      return decoder(s);
    } catch {
      return QueryString.unescape(s, true);
    }
  }

  QueryString.encode = stringify;
  QueryString.decode = parse;
  reg("querystring", QueryString);
})();

)JS";

}  // namespace mbun::jsc::builtins::detail
