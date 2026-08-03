// 生成确定性的 semver 基准数据（固定种子 LCG，任何 JS 运行时可复现）。
// 用法: bun gen.mjs  → 写出 versions.txt / ranges.txt（已入库，无需重复生成）
let seed = 0x6d62756e; // "mbun"
function rnd(n) {
  seed = (seed * 1103515245 + 12345) & 0x7fffffff;
  return seed % n;
}

const PRE = ["alpha", "beta", "rc", "canary", "0", "1", "12", "next"];
function version() {
  let v = `${rnd(20)}.${rnd(50)}.${rnd(100)}`;
  const r = rnd(10);
  if (r < 3) v += `-${PRE[rnd(PRE.length)]}.${rnd(30)}`;
  if (r === 3) v += `-${PRE[rnd(PRE.length)]}`;
  if (rnd(10) < 2) v += `+build.${rnd(1000)}`;
  return v;
}

function range() {
  const base = () => `${rnd(20)}.${rnd(50)}.${rnd(100)}`;
  switch (rnd(8)) {
    case 0: return `^${base()}`;
    case 1: return `~${base()}`;
    case 2: return `>=${base()} <${rnd(20) + 1}.0.0`;
    case 3: return `${rnd(20)}.x`;
    case 4: return `${base()} - ${base()}`;
    case 5: return `^${base()} || ^${base()}`;
    case 6: return `>${base()}`;
    default: return base();
  }
}

const versions = Array.from({ length: 2000 }, version);
const ranges = Array.from({ length: 500 }, range);
await Bun.write(new URL("versions.txt", import.meta.url), versions.join("\n") + "\n");
await Bun.write(new URL("ranges.txt", import.meta.url), ranges.join("\n") + "\n");
console.log(`generated ${versions.length} versions, ${ranges.length} ranges`);
