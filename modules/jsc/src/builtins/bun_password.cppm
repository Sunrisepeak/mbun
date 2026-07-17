// Bun.password JS layer. Self-contained IIFE (appended after the master
// builtins IIFE) that installs Bun.password = { hash, hashSync, verify,
// verifySync } over the native __mbunPasswordNative (runtime/bun_password.inc,
// backed by pure-C++ mbun.crypto bcrypt / argon2). All argument validation,
// option parsing, and error-message shaping match bun's PasswordObject.rs.
//
// Async hash/verify compute synchronously and wrap the result in a resolved/
// rejected Promise (no threadpool); invalid-encoding / unsupported-algorithm
// errors surface synchronously, WeakParameters rejects on the async path.
export module mbun.jsc.js_builtins:bun_password;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kBunPasswordJS = R"JS(
(function () {
  const G = globalThis;
  const NV = G.__mbunPasswordNative;
  if (!NV || !G.Bun) return;

  const UNKNOWN = 'unknown algorithm, expected one of: "bcrypt", "argon2id", "argon2d", "argon2i" (default is "argon2id")';
  const ARGONS = ["argon2id", "argon2d", "argon2i"];
  const isObj = (v) => v !== null && (typeof v === "object" || typeof v === "function");
  const toInt32 = (n) => n | 0;

  const parseAlgorithm = (a) => {
    if (a === undefined || a === null) return { name: "argon2id", memoryCost: 65536, timeCost: 2 };
    if (typeof a === "string") {
      if (a === "bcrypt") return { name: "bcrypt", cost: 10 };
      if (ARGONS.includes(a)) return { name: a, memoryCost: 65536, timeCost: 2 };
      throw new TypeError(UNKNOWN);
    }
    if (isObj(a)) {
      const algo = a.algorithm;
      if (typeof algo !== "string") throw new TypeError('The "algorithm" argument must be of type string.');
      if (algo === "bcrypt") {
        let cost = 10;
        const c = a.cost;
        if (c !== undefined && c !== null && c !== 0) {
          if (typeof c !== "number") throw new TypeError('The "cost" argument must be of type number.');
          const rounds = toInt32(c);
          if (rounds < 4 || rounds > 31) throw new Error("Rounds must be between 4 and 31");
          cost = rounds & 0x3f;
        }
        return { name: "bcrypt", cost };
      }
      if (ARGONS.includes(algo)) {
        let memoryCost = 65536;
        let timeCost = 2;
        const t = a.timeCost;
        if (t !== undefined && t !== null && t !== 0) {
          if (typeof t !== "number") throw new TypeError('The "timeCost" argument must be of type number.');
          const tc = toInt32(t);
          if (tc < 1) throw new Error("Time cost must be greater than 0");
          timeCost = tc;
        }
        const m = a.memoryCost;
        if (m !== undefined && m !== null && m !== 0) {
          if (typeof m !== "number") throw new TypeError('The "memoryCost" argument must be of type number.');
          const mc = toInt32(m);
          if (mc < 8) throw new Error("Memory cost must be at least 8");
          memoryCost = mc;
        }
        return { name: algo, memoryCost, timeCost };
      }
      throw new TypeError(UNKNOWN);
    }
    throw new TypeError('The "algorithm" argument must be of type string.');
  };

  // Coerce a hash-input value (password or stored hash). Buffers pass through;
  // everything else stringifies (Symbol / throwing toString propagate).
  const coerce = (v) => {
    // String(Symbol()) does NOT throw (returns "Symbol()"); reject explicitly so
    // symbols surface as a type error like bun's StringOrBuffer decode.
    if (typeof v === "symbol") {
      throw new TypeError("password must be a string, TypedArray, or Buffer");
    }
    if (typeof v === "string") return { value: v, empty: v.length === 0 };
    if (ArrayBuffer.isView(v)) return { value: v, empty: v.byteLength === 0 };
    if (v instanceof ArrayBuffer) return { value: v, empty: v.byteLength === 0 };
    const s = String(v);
    return { value: s, empty: s.length === 0 };
  };

  const requirePassword = (v, argc) => {
    if (argc < 1 || v === undefined) throw new TypeError("password is required");
    const p = coerce(v);
    if (p.empty) throw new Error("password must not be empty");
    return p.value;
  };

  const doHash = (pwValue, opts) => {
    if (opts.name === "bcrypt") return NV.hash(pwValue, "bcrypt", opts.cost, 0, 0);
    return NV.hash(pwValue, opts.name, 0, opts.memoryCost, opts.timeCost);
  };

  const validateVerifyAlgorithm = (a) => {
    if (a === undefined || a === null) return undefined;
    if (typeof a !== "string") throw new TypeError('The "algorithm" argument must be of type string.');
    if (a !== "bcrypt" && !ARGONS.includes(a)) throw new TypeError(UNKNOWN);
    return a;
  };

  const runVerify = (pwValue, hashValue, algo) => {
    const code = NV.verify(pwValue, hashValue, algo);
    if (code === 1) return true;
    if (code === 0) return false;
    if (code === 2) {
      const e = new Error('Password verification failed with error "WeakParameters"');
      e.code = "PASSWORD_WEAK_PARAMETERS";
      throw e;
    }
    if (code === 3) throw new Error('Password verification failed with error "InvalidEncoding"');
    throw new Error(UNKNOWN);
  };

  const password = {
    hashSync(value, algorithm) {
      const pw = requirePassword(value, arguments.length);
      return doHash(pw, parseAlgorithm(algorithm));
    },
    hash(value, algorithm) {
      let pw, opts;
      try {
        pw = requirePassword(value, arguments.length);
        opts = parseAlgorithm(algorithm);
      } catch (e) {
        throw e;
      }
      return Promise.resolve(doHash(pw, opts));
    },
    verifySync(value, hash, algorithm) {
      if (arguments.length < 2) throw new TypeError("hash is required");
      const algo = validateVerifyAlgorithm(algorithm);
      const P = coerce(value);
      const H = coerce(hash);
      if (P.empty || H.empty) return false;
      return runVerify(P.value, H.value, algo);
    },
    verify(value, hash, algorithm) {
      if (arguments.length < 2) throw new TypeError("hash is required");
      const algo = validateVerifyAlgorithm(algorithm);
      const P = coerce(value);
      const H = coerce(hash);
      if (P.empty || H.empty) return Promise.resolve(false);
      try {
        return Promise.resolve(runVerify(P.value, H.value, algo));
      } catch (e) {
        if (e && e.code === "PASSWORD_WEAK_PARAMETERS") return Promise.reject(e);
        throw e;
      }
    },
  };

  Object.defineProperty(G.Bun, "password", {
    configurable: true, enumerable: true, writable: true, value: password,
  });
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
