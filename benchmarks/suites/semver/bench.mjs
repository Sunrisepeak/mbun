// bun 侧 semver 基准 driver（bun-zig 与 bun-rust 用同一脚本）。
// 用法: <bun二进制> bench.mjs [iters]   输出 JSON 一行。
// 口径: 进程内热循环（排除启动），checksum 防止 DCE。
const { order, satisfies } = Bun.semver;
const dir = new URL(".", import.meta.url);
const versions = (await Bun.file(new URL("versions.txt", dir)).text()).trim().split("\n");
const ranges = (await Bun.file(new URL("ranges.txt", dir)).text()).trim().split("\n");
const iters = Number(process.argv[2] ?? 50);

// 预热
let checksum = 0;
for (let k = 0; k < 3; k++) {
  for (let i = 0; i < versions.length - 1; i++) checksum += order(versions[i], versions[i + 1]);
}

let t0 = Bun.nanoseconds();
for (let k = 0; k < iters; k++) {
  for (let i = 0; i < versions.length - 1; i++) checksum += order(versions[i], versions[i + 1]);
}
const orderNs = Bun.nanoseconds() - t0;
const orderOps = (versions.length - 1) * iters;

t0 = Bun.nanoseconds();
for (let k = 0; k < iters; k++) {
  for (let i = 0; i < versions.length; i++) {
    checksum += satisfies(versions[i], ranges[i % ranges.length]) ? 1 : 0;
  }
}
const satNs = Bun.nanoseconds() - t0;
const satOps = versions.length * iters;

console.log(
  JSON.stringify({
    impl: `bun ${Bun.version}${Bun.revision ? " (" + Bun.revision.slice(0, 9) + ")" : ""}`,
    order_ops_per_s: Math.round(orderOps / (orderNs / 1e9)),
    satisfies_ops_per_s: Math.round(satOps / (satNs / 1e9)),
    checksum,
  }),
);
