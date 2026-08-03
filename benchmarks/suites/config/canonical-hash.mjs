// Deep, order-sensitive FNV-1a hash shared by Bun benchmark/oracle generation.
// Strings are encoded as WTF-8 so lone UTF-16 surrogates match mbun's Value storage.
const OFFSET = 0xcbf29ce484222325n;
const PRIME = 0x100000001b3n;
const MASK = 0xffffffffffffffffn;

function byte(state, value) {
  return ((state ^ BigInt(value & 0xff)) * PRIME) & MASK;
}

function u64(state, value) {
  let n = BigInt(value);
  for (let i = 0; i < 8; i++) {
    state = byte(state, Number(n & 0xffn));
    n >>= 8n;
  }
  return state;
}

function wtf8(text) {
  const out = [];
  for (let i = 0; i < text.length; i++) {
    let cp = text.charCodeAt(i);
    if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < text.length) {
      const low = text.charCodeAt(i + 1);
      if (low >= 0xdc00 && low <= 0xdfff) {
        cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
        i++;
      }
    }
    if (cp <= 0x7f) out.push(cp);
    else if (cp <= 0x7ff) out.push(0xc0 | (cp >> 6), 0x80 | (cp & 0x3f));
    else if (cp <= 0xffff)
      out.push(0xe0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f));
    else
      out.push(
        0xf0 | (cp >> 18),
        0x80 | ((cp >> 12) & 0x3f),
        0x80 | ((cp >> 6) & 0x3f),
        0x80 | (cp & 0x3f),
      );
  }
  return out;
}

function string(state, value) {
  state = byte(state, 0x53);
  const bytes = wtf8(value);
  state = u64(state, bytes.length);
  for (const b of bytes) state = byte(state, b);
  return state;
}

function visit(state, value) {
  if (value === null) return byte(state, 0x4e);
  if (typeof value === "boolean") return byte(state, value ? 0x54 : 0x46);
  if (typeof value === "number") {
    state = byte(state, 0x44);
    const data = new DataView(new ArrayBuffer(8));
    data.setFloat64(0, value, true);
    for (let i = 0; i < 8; i++) state = byte(state, data.getUint8(i));
    return state;
  }
  if (typeof value === "string") return string(state, value);
  if (Array.isArray(value)) {
    state = byte(state, 0x41);
    state = u64(state, value.length);
    for (const child of value) state = visit(state, child);
    return state;
  }
  state = byte(state, 0x4f);
  const keys = Object.keys(value);
  state = u64(state, keys.length);
  for (const key of keys) {
    state = string(state, key);
    state = visit(state, Object.getOwnPropertyDescriptor(value, key).value);
  }
  return state;
}

export function canonicalHash(value) {
  return visit(OFFSET, value);
}

export function canonicalHashHex(value) {
  return canonicalHash(value).toString(16).padStart(16, "0");
}
