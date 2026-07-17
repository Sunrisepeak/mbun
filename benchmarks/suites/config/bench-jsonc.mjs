// B-level comparison input for Bun.JSONC.parse. mbun currently runs the same
// bytes/checksum in its native driver; JS/JSC binding is a separate integration.
import { canonicalHash } from "./canonical-hash.mjs";

const path = process.argv[2];
const iters = Number(process.argv[3] ?? 20000);
const source = await Bun.file(path).text();

let last;
for (let i = 0; i < 3; i++) last = Bun.JSONC.parse(source);
const begin = Bun.nanoseconds();
for (let i = 0; i < iters; i++) last = Bun.JSONC.parse(source);
const elapsed = Bun.nanoseconds() - begin;
const checksum = canonicalHash(last);

console.log(JSON.stringify({
  impl: `bun ${Bun.version}`,
  parse_ops_per_s: Math.round(iters * 1e9 / elapsed),
  checksum: checksum.toString(16).padStart(16, "0"),
}));
