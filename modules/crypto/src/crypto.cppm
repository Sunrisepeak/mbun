// crypto.cppm — mbun.crypto: hashing / message-digest subsystem.
//
// Aggregator that re-exports the crypto subsystem's modules. Pure algorithm
// logic only — the JSC binding layer (Bun.CryptoHasher / Bun.hash host_fns,
// argument decoding, ArrayBuffer plumbing) is DEFERRED(S1) to the JSC
// subsystem. Now covered here: HMAC (RFC 2104), SHA-3/SHAKE (FIPS 202),
// BLAKE2b/2s (RFC 7693), PBKDF2 (RFC 8018), RIPEMD-160 (Dobbertin/Bosselaers/
// Preneel, FSE 1996). Still DEFERRED(S2): md4, XxHash3, and Bun.password
// (bcrypt/argon2).
// ref: bun src/wyhash, src/hash, src/sha_hmac/sha.rs, src/runtime/crypto,
// src/runtime/api/HashObject.rs.
export module mbun.crypto;

export import mbun.crypto.wyhash;
export import mbun.crypto.noncrypto_hash;
export import mbun.crypto.md4;
export import mbun.crypto.md5;
export import mbun.crypto.ripemd160;
export import mbun.crypto.sha;
export import mbun.crypto.hasher;
export import mbun.crypto.hmac;
export import mbun.crypto.sha3;
export import mbun.crypto.blake2;
export import mbun.crypto.pbkdf2;
export import mbun.crypto.bcrypt;
export import mbun.crypto.argon2;
