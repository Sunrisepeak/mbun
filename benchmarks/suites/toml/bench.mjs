// bun 侧 TOML 基准 driver（bun-zig / bun-rust 同一脚本）——与 mbun 侧同口径。
// 输入 = bun 原生 fixture（compat/bun/test/js/bun/resolve/toml/toml-fixture.toml）。
// 用法: <bun二进制> bench.mjs <fixture路径> [iters]   输出 JSON 一行。热循环，checksum 防 DCE。
const path = process.argv[2];
const iters = Number(process.argv[3] ?? 20000);
const src = require("fs").readFileSync(path, "utf8");

let checksum = 0;
for (let k = 0; k < 3; k++) checksum += Object.keys(Bun.TOML.parse(src)).length; // 预热

const t0 = Bun.nanoseconds();
for (let k = 0; k < iters; k++) checksum += Object.keys(Bun.TOML.parse(src)).length;
const ns = Bun.nanoseconds() - t0;

console.log(JSON.stringify({
  impl: `bun ${Bun.version}${Bun.revision ? " (" + Bun.revision.slice(0, 9) + ")" : ""}`,
  parse_ops_per_s: Math.round(iters / (ns / 1e9)),
  checksum,  // = iters*top-level-key-count (+ warmup)
}));
