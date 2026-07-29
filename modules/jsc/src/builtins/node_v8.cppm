// node:v8 payload partition. Implements the V8 value-serialization wire format
// (kLatestVersion = 15) in pure JS so serialize/deserialize round-trip and stay
// byte-compatible with Node's serdes: the 0xFF+version header, zigzag/varint
// integers, one/two-byte strings, object-reference back-links (circular graphs),
// TypedArray/ArrayBuffer/DataView, Map/Set, Date, RegExp, BigInt, boxed
// primitives and Error objects. Serializer/Deserializer expose the Node public
// API (writeHeader/writeValue/writeUint32/writeUint64/writeDouble/writeRawBytes/
// releaseBuffer, readHeader/readValue/readUint32/...); DefaultSerializer routes
// ArrayBufferViews through the host-object channel exactly like Node's lib/v8.js.
//
// Blueprint: V8 src/objects/value-serializer.cc (format/tags) + Node lib/v8.js
// (DefaultSerializer/DefaultDeserializer host-object shape). Heap statistics are
// plausible stubs (JSC has no per-space accounting). DEFERRED: shared/resizable
// ArrayBuffers, WASM/SharedObject transfer, and references that point at a
// TypedArray's backing buffer specifically (a documented V8 asymmetry).
//
// NOTE: appended AFTER the master builtins IIFE (opened in bootstrap, closed by
// image_closure), so this is a self-contained IIFE that re-binds G = globalThis
// and overwrites the JSON-roundtrip v8 stub registered in bootstrap.
export module mbun.jsc.js_builtins:node_v8;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kNodeV8JS = R"JS(
(function () {
  const G = globalThis;
  const M = G.__mbunNativeModules;
  if (!M) return;

  const kLatestVersion = 15;

  // Serialization tags (V8 SerializationTag).
  const T = {
    version: 0xFF, padding: 0x00, undefined: 0x5F, null: 0x30, true: 0x54,
    false: 0x46, int32: 0x49, uint32: 0x55, double: 0x4E, bigint: 0x5A,
    utf8String: 0x53, oneByteString: 0x22, twoByteString: 0x63,
    objectReference: 0x5E, beginObject: 0x6F, endObject: 0x7B,
    beginSparseArray: 0x61, endSparseArray: 0x40, beginDenseArray: 0x41,
    endDenseArray: 0x24, theHole: 0x2D, date: 0x44, trueObject: 0x79,
    falseObject: 0x78, numberObject: 0x6E, bigintObject: 0x7A,
    stringObject: 0x73, regexp: 0x52, beginMap: 0x3B, endMap: 0x3A,
    beginSet: 0x27, endSet: 0x2C, arrayBuffer: 0x42, arrayBufferView: 0x56,
    sharedArrayBuffer: 0x75, hostObject: 0x5C, error: 0x72,
  };

  const hasF16 = typeof Float16Array !== "undefined";
  const hasBuf = typeof Buffer !== "undefined";
  // Host-object (DefaultSerializer) view table — index order matches Node's
  // lib/v8.js exactly so serialized buffers are cross-compatible. Slot 10 is
  // Buffer (a Uint8Array subclass), reconstructed as a Buffer on read.
  const kIndexByName = {
    Int8Array: 0, Uint8Array: 1, Uint8ClampedArray: 2, Int16Array: 3,
    Uint16Array: 4, Int32Array: 5, Uint32Array: 6, Float32Array: 7,
    Float64Array: 8, DataView: 9, Buffer: 10, BigInt64Array: 11,
    BigUint64Array: 12, Float16Array: 13,
  };
  const kViewByIndex = [Int8Array, Uint8Array, Uint8ClampedArray, Int16Array,
    Uint16Array, Int32Array, Uint32Array, Float32Array, Float64Array, DataView,
    hasBuf ? Buffer : Uint8Array, BigInt64Array, BigUint64Array,
    hasF16 ? Float16Array : Uint8Array];

  // Plain (non-host) ArrayBufferView tag chars (V8 ArrayBufferViewTag).
  const kViewTag = {
    Int8Array: 0x62, Uint8Array: 0x42, Uint8ClampedArray: 0x43, Int16Array: 0x77,
    Uint16Array: 0x57, Int32Array: 0x64, Uint32Array: 0x44, Float16Array: 0x68,
    Float32Array: 0x66, Float64Array: 0x46, BigInt64Array: 0x71,
    BigUint64Array: 0x51, DataView: 0x3F,
  };
  const kViewCtorByTag = {};
  kViewCtorByTag[0x62] = Int8Array; kViewCtorByTag[0x42] = Uint8Array;
  kViewCtorByTag[0x43] = Uint8ClampedArray; kViewCtorByTag[0x77] = Int16Array;
  kViewCtorByTag[0x57] = Uint16Array; kViewCtorByTag[0x64] = Int32Array;
  kViewCtorByTag[0x44] = Uint32Array; kViewCtorByTag[0x66] = Float32Array;
  kViewCtorByTag[0x46] = Float64Array; kViewCtorByTag[0x71] = BigInt64Array;
  kViewCtorByTag[0x51] = BigUint64Array; kViewCtorByTag[0x3F] = DataView;
  if (hasF16) kViewCtorByTag[0x68] = Float16Array;

  const utf8Decoder = typeof TextDecoder !== "undefined" ? new TextDecoder("utf-8") : null;

  const dataCloneError = (v) => {
    const e = new Error((typeof v === "object" ? Object.prototype.toString.call(v) : String(v)) +
      " could not be cloned.");
    e.name = "DataCloneError";
    return e;
  };

  // ------------------------------------------------------------- Serializer
  class Serializer {
    constructor() {
      this._buf = new Uint8Array(64);
      this._len = 0;
      this._idMap = new Map();
      this._nextId = 0;
      this._treatViewsAsHost = false;
    }
    _grow(extra) {
      if (this._len + extra <= this._buf.length) return;
      let cap = this._buf.length * 2;
      while (cap < this._len + extra) cap *= 2;
      const nb = new Uint8Array(cap);
      nb.set(this._buf.subarray(0, this._len));
      this._buf = nb;
    }
    _byte(b) { this._grow(1); this._buf[this._len++] = b & 0xFF; }
    _raw(bytes) {
      this._grow(bytes.length);
      this._buf.set(bytes, this._len);
      this._len += bytes.length;
    }
    _varint(n) {
      n = n < 0 ? 0 : n;
      for (;;) {
        const b = n % 128;
        n = Math.floor(n / 128);
        if (n !== 0) this._byte(b | 0x80);
        else { this._byte(b); break; }
      }
    }
    _zigzag(n) { this._varint((((n << 1) ^ (n >> 31)) >>> 0)); }
    _double(d) {
      const ab = new ArrayBuffer(8);
      new DataView(ab).setFloat64(0, d, true);
      this._raw(new Uint8Array(ab));
    }

    // ---- Node public API ----
    writeHeader() { this._byte(T.version); this._varint(kLatestVersion); }
    writeValue(value) { this._writeValue(value); return true; }
    releaseBuffer() { return Buffer.from(this._buf.subarray(0, this._len)); }
    transferArrayBuffer(_id, _ab) { /* DEFERRED: no transfer map wired */ }
    writeUint32(v) { this._varint(v >>> 0); }
    writeUint64(hi, lo) { this._varint((hi >>> 0) * 4294967296 + (lo >>> 0)); }
    writeDouble(d) { this._double(d); }
    writeRawBytes(source) {
      if (source instanceof ArrayBuffer) this._raw(new Uint8Array(source));
      else this._raw(new Uint8Array(source.buffer, source.byteOffset, source.byteLength));
    }
    _setTreatArrayBufferViewsAsHostObjects(flag) { this._treatViewsAsHost = !!flag; }
    _getDataCloneError(message) { const e = new Error(message); e.name = "DataCloneError"; return e; }
    _writeHostObject(_obj) { throw dataCloneError(_obj); }

    // ---- core walk ----
    _writeValue(v) {
      if (v === undefined) return this._byte(T.undefined);
      if (v === null) return this._byte(T.null);
      if (v === true) return this._byte(T.true);
      if (v === false) return this._byte(T.false);
      const t = typeof v;
      if (t === "number") return this._writeNumber(v);
      if (t === "bigint") { this._byte(T.bigint); return this._bigintContents(v); }
      if (t === "string") return this._writeString(v);
      if (t === "object" || t === "function") return this._writeReceiver(v);
      throw dataCloneError(v);
    }
    _writeNumber(n) {
      if (Number.isInteger(n) && n >= -2147483648 && n <= 2147483647 && !Object.is(n, -0)) {
        this._byte(T.int32); this._zigzag(n);
      } else { this._byte(T.double); this._double(n); }
    }
    _writeString(s) {
      let oneByte = true;
      for (let i = 0; i < s.length; i++) { if (s.charCodeAt(i) > 0xFF) { oneByte = false; break; } }
      if (oneByte) {
        this._byte(T.oneByteString);
        this._varint(s.length);
        const b = new Uint8Array(s.length);
        for (let i = 0; i < s.length; i++) b[i] = s.charCodeAt(i);
        this._raw(b);
      } else {
        this._byte(T.twoByteString);
        this._varint(s.length * 2);
        const b = new Uint8Array(s.length * 2);
        const dv = new DataView(b.buffer);
        for (let i = 0; i < s.length; i++) dv.setUint16(i * 2, s.charCodeAt(i), true);
        this._raw(b);
      }
    }
    _bigintContents(bi) {
      const negative = bi < 0n;
      let v = negative ? -bi : bi;
      const bytes = [];
      while (v > 0n) { bytes.push(Number(v & 0xFFn)); v >>= 8n; }
      while (bytes.length % 8 !== 0) bytes.push(0);
      const bitfield = (negative ? 1 : 0) | (bytes.length << 1);
      this._varint(bitfield >>> 0);
      for (let i = 0; i < bytes.length; i++) this._byte(bytes[i]);
    }
    _writeReceiver(v) {
      const existing = this._idMap.get(v);
      if (existing !== undefined) { this._byte(T.objectReference); this._varint(existing); return; }
      this._idMap.set(v, this._nextId++);

      if (Array.isArray(v)) return this._writeArray(v);
      if (v instanceof Date) { this._byte(T.date); return this._double(v.getTime()); }
      if (v instanceof RegExp) return this._writeRegExp(v);
      if (v instanceof Map) return this._writeMap(v);
      if (v instanceof Set) return this._writeSet(v);
      if (v instanceof ArrayBuffer) return this._writeArrayBuffer(v);
      if (ArrayBuffer.isView(v)) {
        if (this._treatViewsAsHost) { this._byte(T.hostObject); return this._writeHostObject(v); }
        return this._writeArrayBufferView(v);
      }
      if (v instanceof Number) { this._byte(T.numberObject); return this._double(v.valueOf()); }
      if (v instanceof String) { this._byte(T.stringObject); return this._writeString(v.valueOf()); }
      if (v instanceof Boolean) return this._byte(v.valueOf() ? T.trueObject : T.falseObject);
      if (typeof BigInt !== "undefined" && v instanceof BigInt) {
        this._byte(T.bigintObject); return this._bigintContents(v.valueOf());
      }
      if (v instanceof Error) return this._writeError(v);
      return this._writeObject(v);
    }
    _writeObject(o) {
      this._byte(T.beginObject);
      const keys = Object.keys(o);
      let count = 0;
      for (let i = 0; i < keys.length; i++) {
        const k = keys[i];
        this._writeValue(k);
        this._writeValue(o[k]);
        count++;
      }
      this._byte(T.endObject);
      this._varint(count);
    }
    _writeArray(a) {
      const length = a.length >>> 0;
      this._byte(T.beginDenseArray);
      this._varint(length);
      for (let i = 0; i < length; i++) {
        if (i in a) this._writeValue(a[i]);
        else this._byte(T.theHole);
      }
      // extra (non-index) own enumerable string properties
      const keys = Object.keys(a);
      let props = 0;
      for (let i = 0; i < keys.length; i++) {
        const k = keys[i];
        const n = +k;
        if (Number.isInteger(n) && n >= 0 && n < length && String(n) === k) continue;
        this._writeValue(k);
        this._writeValue(a[k]);
        props++;
      }
      this._byte(T.endDenseArray);
      this._varint(props);
      this._varint(length);
    }
    _writeMap(m) {
      this._byte(T.beginMap);
      let n = 0;
      m.forEach((val, key) => { this._writeValue(key); this._writeValue(val); n += 2; });
      this._byte(T.endMap);
      this._varint(n);
    }
    _writeSet(s) {
      this._byte(T.beginSet);
      let n = 0;
      s.forEach((val) => { this._writeValue(val); n++; });
      this._byte(T.endSet);
      this._varint(n);
    }
    _writeRegExp(re) {
      this._byte(T.regexp);
      this._writeString(re.source);
      let flags = 0;
      const f = re.flags;
      if (f.indexOf("g") >= 0) flags |= 1;
      if (f.indexOf("i") >= 0) flags |= 2;
      if (f.indexOf("m") >= 0) flags |= 4;
      if (f.indexOf("y") >= 0) flags |= 8;
      if (f.indexOf("u") >= 0) flags |= 16;
      if (f.indexOf("s") >= 0) flags |= 32;
      if (f.indexOf("d") >= 0) flags |= 128;
      if (f.indexOf("v") >= 0) flags |= 256;
      this._varint(flags);
    }
    _writeArrayBuffer(ab) {
      this._byte(T.arrayBuffer);
      this._varint(ab.byteLength);
      this._raw(new Uint8Array(ab));
    }
    _writeArrayBufferView(view) {
      this._writeValue(view.buffer);
      this._byte(T.arrayBufferView);
      const name = view instanceof DataView ? "DataView" : view.constructor.name;
      this._varint(kViewTag[name] || kViewTag.Uint8Array);
      this._varint(view.byteOffset);
      this._varint(view.byteLength);
      this._varint(0); // flags: not length-tracking / not RAB-backed
    }
    _writeError(err) {
      this._byte(T.error);
      const protoTag = { EvalError: 0x45, RangeError: 0x52, ReferenceError: 0x46,
        SyntaxError: 0x53, TypeError: 0x54, URIError: 0x55 }[err.name];
      if (protoTag) this._byte(protoTag);
      if (typeof err.message === "string") { this._byte(0x6D); this._writeString(err.message); }
      if (typeof err.stack === "string") { this._byte(0x73); this._writeString(err.stack); }
      if (Object.prototype.hasOwnProperty.call(err, "cause")) { this._byte(0x63); this._writeValue(err.cause); }
      this._byte(0x2E);
    }
  }

  // ----------------------------------------------------------- Deserializer
  class Deserializer {
    constructor(buffer) {
      if (buffer instanceof ArrayBuffer) this._bytes = new Uint8Array(buffer);
      else this._bytes = new Uint8Array(buffer.buffer, buffer.byteOffset, buffer.byteLength);
      this._pos = 0;
      this._version = kLatestVersion;
      this._objects = [];
    }
    _byte() {
      if (this._pos >= this._bytes.length) throw new Error("v8 deserialize: unexpected end of buffer");
      return this._bytes[this._pos++];
    }
    _peek() { return this._pos < this._bytes.length ? this._bytes[this._pos] : -1; }
    _varint() {
      let value = 0, shift = 0, b;
      do { b = this._byte(); value += (b & 0x7F) * Math.pow(2, shift); shift += 7; } while (b & 0x80);
      return value;
    }
    _zigzag() { const zz = this._varint(); return (Math.floor(zz / 2)) ^ -(zz & 1); }
    _double() {
      const ab = new ArrayBuffer(8);
      const out = new Uint8Array(ab);
      for (let i = 0; i < 8; i++) out[i] = this._byte();
      return new DataView(ab).getFloat64(0, true);
    }
    _rawCopy(len) {
      const out = new Uint8Array(len);
      for (let i = 0; i < len; i++) out[i] = this._byte();
      return out;
    }
    _add(obj) { this._objects.push(obj); return obj; }

    // ---- Node public API ----
    readHeader() {
      if (this._peek() === T.version) { this._byte(); this._version = this._varint(); }
      return this._version;
    }
    readValue() { return this._readValue(); }
    getWireFormatVersion() { return this._version; }
    transferArrayBuffer(_id, _ab) { /* DEFERRED */ }
    readUint32() { return this._varint() >>> 0; }
    readUint64() { const v = this._varint(); return [Math.floor(v / 4294967296) >>> 0, (v >>> 0)]; }
    readDouble() { return this._double(); }
    readRawBytes(length) { return this._rawCopy(length); }
    _readHostObject() { throw new Error("v8 deserialize: host objects are not supported by the base Deserializer"); }

    _readString() {
      const tag = this._byte();
      return this._readStringTag(tag);
    }
    _readStringTag(tag) {
      if (tag === T.oneByteString) {
        const len = this._varint();
        let s = "";
        for (let i = 0; i < len; i++) s += String.fromCharCode(this._byte());
        return s;
      }
      if (tag === T.twoByteString) {
        const len = this._varint();
        const b = this._rawCopy(len);
        const dv = new DataView(b.buffer);
        let s = "";
        for (let i = 0; i + 1 < len; i += 2) s += String.fromCharCode(dv.getUint16(i, true));
        return s;
      }
      if (tag === T.utf8String) {
        const len = this._varint();
        const b = this._rawCopy(len);
        return utf8Decoder ? utf8Decoder.decode(b) : Buffer.from(b).toString("utf8");
      }
      throw new Error("v8 deserialize: expected a string, got tag 0x" + tag.toString(16));
    }
    _bigint() {
      const bitfield = this._varint();
      const negative = (bitfield & 1) === 1;
      const byteLength = Math.floor(bitfield / 2);
      let v = 0n;
      for (let i = 0; i < byteLength; i++) v |= BigInt(this._byte()) << BigInt(8 * i);
      return negative ? -v : v;
    }
    _readValue() {
      const tag = this._byte();
      switch (tag) {
        case T.undefined: return undefined;
        case T.null: return null;
        case T.true: return true;
        case T.false: return false;
        case T.int32: return this._zigzag();
        case T.uint32: return this._varint() >>> 0;
        case T.double: return this._double();
        case T.bigint: return this._bigint();
        case T.oneByteString:
        case T.twoByteString:
        case T.utf8String: return this._readStringTag(tag);
        case T.objectReference: return this._objects[this._varint()];
        case T.beginObject: return this._readObject();
        case T.beginDenseArray: return this._readDenseArray();
        case T.beginSparseArray: return this._readSparseArray();
        case T.date: return this._add(new Date(this._double()));
        case T.trueObject: return this._add(new Boolean(true));
        case T.falseObject: return this._add(new Boolean(false));
        case T.numberObject: return this._add(new Number(this._double()));
        case T.bigintObject: return this._add(Object(this._bigint()));
        case T.stringObject: return this._add(new String(this._readString()));
        case T.regexp: return this._readRegExp();
        case T.beginMap: return this._readMap();
        case T.beginSet: return this._readSet();
        case T.arrayBuffer: return this._readArrayBuffer();
        case T.sharedArrayBuffer: return this._readArrayBuffer();
        case T.hostObject: { const o = this._readHostObject(); return this._add(o); }
        case T.error: return this._readError();
        default: throw new Error("v8 deserialize: unknown tag 0x" + tag.toString(16));
      }
    }
    _readObject() {
      const o = this._add({});
      for (;;) {
        if (this._peek() === T.endObject) { this._byte(); break; }
        const key = this._readValue();
        o[key] = this._readValue();
      }
      this._varint(); // property count
      return o;
    }
    _readDenseArray() {
      const length = this._varint();
      const a = this._add(new Array(length));
      for (let i = 0; i < length; i++) {
        if (this._peek() === T.theHole) { this._byte(); continue; }
        a[i] = this._readValue();
      }
      // Extra (non-index) props are written inline before the end tag.
      for (;;) {
        if (this._peek() === T.endDenseArray) { this._byte(); break; }
        const k = this._readValue();
        a[k] = this._readValue();
      }
      this._varint(); // property count
      this._varint(); // length echo
      return a;
    }
    _readSparseArray() {
      const length = this._varint();
      const a = this._add(new Array(length));
      for (;;) {
        if (this._peek() === T.endSparseArray) { this._byte(); break; }
        const k = this._readValue();
        a[k] = this._readValue();
      }
      this._varint(); // property count
      this._varint(); // length echo
      return a;
    }
    _readMap() {
      const m = this._add(new Map());
      for (;;) {
        if (this._peek() === T.endMap) { this._byte(); break; }
        const k = this._readValue();
        const v = this._readValue();
        m.set(k, v);
      }
      this._varint(); // element count
      return m;
    }
    _readSet() {
      const s = this._add(new Set());
      for (;;) {
        if (this._peek() === T.endSet) { this._byte(); break; }
        s.add(this._readValue());
      }
      this._varint(); // element count
      return s;
    }
    _readRegExp() {
      const source = this._readString();
      const bits = this._varint();
      let f = "";
      if (bits & 1) f += "g";
      if (bits & 2) f += "i";
      if (bits & 4) f += "m";
      if (bits & 8) f += "y";
      if (bits & 16) f += "u";
      if (bits & 32) f += "s";
      if (bits & 128) f += "d";
      if (bits & 256) f += "v";
      // 'y' and 'd' ordering in the string does not matter to the RegExp ctor.
      return this._add(new RegExp(source, f.replace("yd", "dy")));
    }
    _readArrayBuffer() {
      const len = this._varint();
      const bytes = this._rawCopy(len);
      const ab = bytes.buffer;
      this._add(ab);
      if (this._peek() === T.arrayBufferView) { this._byte(); return this._readArrayBufferView(ab); }
      return ab;
    }
    _readArrayBufferView(ab) {
      const tag = this._varint();
      const byteOffset = this._varint();
      const byteLength = this._varint();
      if (this._version >= 14) this._varint(); // flags
      const ctor = kViewCtorByTag[tag] || Uint8Array;
      let view;
      if (ctor === DataView) view = new DataView(ab, byteOffset, byteLength);
      else view = new ctor(ab, byteOffset, byteLength / (ctor.BYTES_PER_ELEMENT || 1));
      return this._add(view);
    }
    _readError() {
      let ctor = Error, message = "", stack, created = null;
      const ensure = () => {
        if (!created) { created = new ctor(message); this._add(created); }
        return created;
      };
      for (;;) {
        const tag = this._byte();
        if (tag === 0x2E) break; // '.'
        switch (tag) {
          case 0x45: ctor = EvalError; break;
          case 0x52: ctor = RangeError; break;
          case 0x46: ctor = ReferenceError; break;
          case 0x53: ctor = SyntaxError; break;
          case 0x54: ctor = TypeError; break;
          case 0x55: ctor = URIError; break;
          case 0x6D: message = this._readString(); break;
          case 0x73: stack = this._readString(); break;
          case 0x63: { const e = ensure(); e.cause = this._readValue(); break; }
          default: break;
        }
      }
      const e = ensure();
      if (stack !== undefined) { try { e.stack = stack; } catch (_) {} }
      return e;
    }
  }

  // ------------------------------------------------ Default (host-object) pair
  class DefaultSerializer extends Serializer {
    constructor() { super(); this._setTreatArrayBufferViewsAsHostObjects(true); }
    _writeHostObject(view) {
      if (!ArrayBuffer.isView(view)) throw dataCloneError(view);
      let idx;
      if (hasBuf && Buffer.isBuffer(view)) idx = 10;
      else {
        const name = view instanceof DataView ? "DataView" : view.constructor.name;
        idx = kIndexByName[name];
        if (idx === undefined) idx = 1; // fall back to Uint8Array
      }
      const bytes = new Uint8Array(view.buffer, view.byteOffset, view.byteLength);
      this.writeUint32(idx);
      this.writeUint32(bytes.byteLength);
      this.writeRawBytes(bytes);
    }
  }
  class DefaultDeserializer extends Deserializer {
    _readHostObject() {
      const idx = this.readUint32();
      const ctor = kViewByIndex[idx] || Uint8Array;
      const byteLength = this.readUint32();
      const bytes = this.readRawBytes(byteLength);
      if (idx === 10 && hasBuf) return Buffer.from(bytes.buffer, bytes.byteOffset, byteLength);
      if (ctor === DataView) return new DataView(bytes.buffer, bytes.byteOffset, byteLength);
      const per = ctor.BYTES_PER_ELEMENT || 1;
      return new ctor(bytes.buffer, bytes.byteOffset, byteLength / per);
    }
  }

  function serialize(value) {
    const s = new DefaultSerializer();
    s.writeHeader();
    s.writeValue(value);
    return s.releaseBuffer();
  }
  function deserialize(buffer) {
    const d = new DefaultDeserializer(buffer);
    d.readHeader();
    return d.readValue();
  }

  // ----------------------------------------------------- heap / misc (stubs)
  function getHeapStatistics() {
    return {
      total_heap_size: 4194304, total_heap_size_executable: 262144,
      total_physical_size: 4194304, total_available_size: 1073741824,
      used_heap_size: 2097152, heap_size_limit: 2147483648, malloced_memory: 8192,
      peak_malloced_memory: 1048576, does_zap_garbage: 0,
      number_of_native_contexts: 1, number_of_detached_contexts: 0,
      total_global_handles_size: 8192, used_global_handles_size: 2208,
      external_memory: 2264,
    };
  }
  function getHeapSpaceStatistics() {
    const names = ["read_only_space", "new_space", "old_space", "code_space",
      "map_space", "large_object_space", "code_large_object_space", "new_large_object_space"];
    return names.map((space_name) => ({
      space_name, space_size: 1048576, space_used_size: 524288,
      space_available_size: 524288, physical_space_size: 1048576,
    }));
  }
  function getHeapCodeStatistics() {
    return { code_and_metadata_size: 0, bytecode_and_metadata_size: 0,
      external_script_source_size: 0, cpu_profiler_metadata_size: 0 };
  }
  let cachedTag = 0;
  function cachedDataVersionTag() { return (cachedTag ||= 0x8f3d2c1b >>> 0); }
  function setFlagsFromString() { /* no-op: JSC does not honor V8 flags */ }
  function takeCoverage() { /* no-op */ }
  function stopCoverage() { /* no-op */ }
  function writeHeapSnapshot() { return ""; }
  function setHeapSnapshotNearHeapLimit() {}
  function getHeapSnapshot() {
    const Readable = (M["stream"] || M["node:stream"] || {}).Readable;
    if (Readable) {
      const r = new Readable({ read() {} });
      r.push("{}"); r.push(null);
      return r;
    }
    return { [Symbol.asyncIterator]() { let done = false; return { next() {
      if (done) return Promise.resolve({ done: true, value: undefined });
      done = true; return Promise.resolve({ done: false, value: Buffer.from("{}") });
    } }; } };
  }
  const promiseHooks = { createHook: () => () => {}, onInit: () => () => {},
    onSettled: () => () => {}, onBefore: () => () => {}, onAfter: () => () => {} };
  const startupSnapshot = { isBuildingSnapshot: () => false,
    addSerializeCallback: () => {}, addDeserializeCallback: () => {},
    setDeserializeMainFunction: () => {} };
  class GCProfiler { start() {} stop() { return { statistics: [] }; } }

  const v8 = {
    serialize, deserialize, Serializer, Deserializer, DefaultSerializer,
    DefaultDeserializer, getHeapStatistics, getHeapSpaceStatistics,
    getHeapCodeStatistics, getHeapSnapshot, cachedDataVersionTag,
    setFlagsFromString, takeCoverage, stopCoverage, writeHeapSnapshot,
    setHeapSnapshotNearHeapLimit, promiseHooks, startupSnapshot, GCProfiler,
  };
  M["v8"] = v8;
  M["node:v8"] = v8;
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
