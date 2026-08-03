// bun 侧 stringWidth 基准 driver（bun-zig / bun-rust 同一脚本）——与 mbun 侧同口径。
// 15 组覆盖宽度逻辑的字符串（ASCII/CJK/emoji/ANSI/ZWJ/组合符）。checksum = Σ 宽度。
// 用法: <bun二进制> bench.mjs [iters]   输出 JSON 一行。
const S = [
  "hello world",
  "the quick brown fox jumps over the lazy dog 0123456789",
  "你好世界",                 // 你好世界 (CJK wide)
  "こんにちは",           // こんにちは
  "😎中😀",           // 😎中😀
  "\x1b[31mred\x1b[0m \x1b[1;32mgreen\x1b[0m",  // ANSI colored
  "café naïve résumé",     // Latin-1
  "👩‍💻",            // 👩‍💻 ZWJ family
  "áéó",                     // combining accents
  "🇺🇸🇨🇳",  // 🇺🇸🇨🇳 flags
  "ＡＢＣ",                        // ＡＢＣ fullwidth
  "1️⃣ 2️⃣",               // 1️⃣ 2️⃣ keycaps
  "mixed 中文 and English 🚀",
  "\t\ttabs and    spaces",
  "no-width​​zero",                  // zero-width spaces
];
const iters = Number(process.argv[2] ?? 30000);

let checksum = 0;
for (let k = 0; k < 3; k++) for (const s of S) checksum += Bun.stringWidth(s);

const t0 = Bun.nanoseconds();
for (let k = 0; k < iters; k++) for (const s of S) checksum += Bun.stringWidth(s);
const ns = Bun.nanoseconds() - t0;
const ops = S.length * iters;

console.log(JSON.stringify({
  impl: `bun ${Bun.version}${Bun.revision ? " (" + Bun.revision.slice(0, 9) + ")" : ""}`,
  width_ops_per_s: Math.round(ops / (ns / 1e9)),
  checksum,
}));
