// bun 侧 glob 基准 driver（bun-zig / bun-rust 同一脚本）——与 mbun 侧同口径。
// 10 组 pattern/path 取自 compat/bun/bench/glob/match.mjs（上游原生用例）。
// 用法: <bun二进制> bench.mjs [iters]   输出 JSON 一行。进程内热循环，checksum 防 DCE。
const CASES = [
  ["1{2,3{4,5{6,7{8,9{a,b{c,d{e,f{g,h{i,j{k,l}}}}}}}}}}m", "13579bdfhjlm"],
  ["😎/¢£.{ts,tsx,js,jsx}", "😎/¢£.jsx"],
  ["フォルダ/**/*", "フォルダ/aaa.js"],
  ["1{2,3{4,5{6,7{8,😎{a,b{c,d{e,f{g,h{i,j{k,l}}}}}}}}}}m", "1357😎bdfhjlm"],
  ["test/{foo/**,bar}/baz", "test/bar/baz"],
  ["a/**/c/*.md", "a/bb.bb/aa/b.b/aa/c/xyz.md"],
  ["a/b/**/c{d,e}/**/xyz.md", "a/b/cd/xyz.md"],
  ["foo/bar/**/one/**/*.*", "foo/bar/baz/one/two/three/image.png"],
  ["some/**/needle.{js,tsx,mdx,ts,jsx,txt}", "some/a/bigger/path/to/the/crazy/needle.txt"],
  ["f[^eiu][^eiu][^eiu][^eiu][^eiu]r", "foo-bar"],
];
const iters = Number(process.argv[2] ?? 200);
const globs = CASES.map(([g]) => new Bun.Glob(g)); // construct once; measure match()

let checksum = 0;
for (let k = 0; k < 3; k++) for (let i = 0; i < CASES.length; i++) checksum += globs[i].match(CASES[i][1]) ? 1 : 0;

const t0 = Bun.nanoseconds();
for (let k = 0; k < iters; k++) {
  for (let i = 0; i < CASES.length; i++) checksum += globs[i].match(CASES[i][1]) ? 1 : 0;
}
const ns = Bun.nanoseconds() - t0;
const ops = CASES.length * iters;
console.log(JSON.stringify({
  impl: `bun ${Bun.version}${Bun.revision ? " (" + Bun.revision.slice(0, 9) + ")" : ""}`,
  match_ops_per_s: Math.round(ops / (ns / 1e9)),
  checksum,
}));
