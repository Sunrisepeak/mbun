// bun-zig / bun-rust driver. The virtual file graph is byte-for-byte identical
// to bench.cpp; output is validated separately by validate.sh.
const iterations = Number(process.argv[2] ?? 5000);
const files = {
  "/entry.js":
    "import { a } from './components/a.js'; import { b } from './components/b.js'; function callLocal(require) { return require('./not-a-module.js'); } globalThis.__bundlerChecksum = a + b + callLocal(() => 0);",
  "/components/a.js": "import { value } from '../shared/util.ts'; export const a = value + 1;",
  "/components/b.js": "import { value } from '../shared/util.ts'; export const b = value + 2;",
  "/shared/util.ts": "export const value: number = 40;",
};
const options = { entrypoints: ["/entry.js"], files, write: false, target: "bun" };

for (let i = 0; i < 3; i++) {
  const warm = await Bun.build(options);
  if (!warm.success) throw new Error("warmup build failed");
}

let bytes = 0;
const begin = Bun.nanoseconds();
for (let i = 0; i < iterations; i++) {
  const result = await Bun.build(options);
  if (!result.success) throw new Error("build failed");
  bytes += result.outputs[0].size;
}
const elapsed = Bun.nanoseconds() - begin;

if (process.argv[3] === "--emit") {
  const result = await Bun.build(options);
  await Bun.write(process.argv[4], result.outputs[0]);
}
console.log(JSON.stringify({ ns_per_build: Math.trunc(elapsed / iterations), checksum: 83, bytes }));
