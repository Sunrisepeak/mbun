// The parser is private in Bun and only exposed by test builds. Release Bun
// binaries may reject this script; that absence is recorded, never replaced by
// a different operation masquerading as an INI parser benchmark.
import { canonicalHash } from "./canonical-hash.mjs";

const path = process.argv[2];
const iters = Number(process.argv[3] ?? 20000);
const source = await Bun.file(path).text();
const parse = require("bun:internal-for-testing").iniInternals.parse;

let last;
for (let i = 0; i < 3; i++) last = parse(source);
const begin = Bun.nanoseconds();
for (let i = 0; i < iters; i++) last = parse(source);
const elapsed = Bun.nanoseconds() - begin;
const checksum = canonicalHash(last);
console.log(JSON.stringify({
  impl: `bun ${Bun.version}`,
  parse_ops_per_s: Math.round(iters * 1e9 / elapsed),
  checksum: checksum.toString(16).padStart(16, "0"),
}));
