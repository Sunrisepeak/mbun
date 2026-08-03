// URLSearchParams A-level candidate: every runtime must execute this exact JS.
// mbun results are intentionally unavailable until T3.6 JSC binding lands.
const iterations = Number(process.argv[2] ?? 200_000);
const input =
  "name=John+Doe&tag=runtime&tag=performance&emoji=%F0%9F%98%80&" +
  "redirect=https%3A%2F%2Fexample.com%2Fa%3Fx%3D1%26y%3D2";

let checksum = 0;
for (let warmup = 0; warmup < 3; warmup++) {
  for (let i = 0; i < 10_000; i++) checksum += new URLSearchParams(input).toString().length;
}

let start = Bun.nanoseconds();
for (let i = 0; i < iterations; i++) {
  const params = new URLSearchParams(input);
  params.sort();
  checksum += params.size + params.toString().length;
}
const parseSerializeNs = Bun.nanoseconds() - start;

start = Bun.nanoseconds();
for (let i = 0; i < iterations; i++) {
  const params = new URLSearchParams("a=1&a=2&b=3");
  params.set("a", "updated");
  params.append("tag", "performance");
  params.delete("b");
  checksum += params.has("tag", "performance") ? params.get("a").length : 0;
}
const mutateQueryNs = Bun.nanoseconds() - start;

console.log(
  JSON.stringify({
    parse_serialize_ops_per_s: Math.round(iterations / (parseSerializeNs / 1e9)),
    mutate_query_ops_per_s: Math.round(iterations / (mutateQueryNs / 1e9)),
    checksum,
  }),
);
