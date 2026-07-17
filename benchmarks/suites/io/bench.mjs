// bun-zig / bun-rust positioned descriptor I/O benchmark driver.
// Usage: <bun> bench.mjs [minimum-round-ms] [rounds]. Output is one JSON line.
import { closeSync, mkdtempSync, openSync, readSync, rmSync, writeFileSync, writeSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { ptr } from "bun:ffi";

const minimumRoundNs = Math.max(250, Number(process.argv[2] ?? 250)) * 1e6;
const rounds = Math.max(10, Number(process.argv[3] ?? 10));
const directory = mkdtempSync(join(tmpdir(), "mbun-io-bench-"));
const sizes = [4 * 1024, 1024 * 1024];
const names = ["read_4k", "read_1m", "write_4k", "write_1m"];

function makeState(size) {
  const payload = Buffer.allocUnsafe(size);
  for (let index = 0; index < payload.length; index++) payload[index] = (index * 131 + 17) & 0xff;
  const input = join(directory, `input-${size}.bin`);
  const output = join(directory, `output-${size}.bin`);
  writeFileSync(input, payload);
  return {
    payload,
    buffer: Buffer.allocUnsafe(size),
    reader: openSync(input, "r"),
    writer: openSync(output, "w+"),
  };
}

function runCaseOnce(caseIndex, state, iterations) {
  const write = caseIndex >= 2;
  let checksum = 0;
  for (let iteration = 0; iteration < iterations; iteration++) {
    if (write) {
      const amount = writeSync(state.writer, state.payload, 0, state.payload.length, 0);
      if (amount !== state.payload.length) throw new Error(`short pwrite: ${amount}`);
      checksum += amount;
    } else {
      const amount = readSync(state.reader, state.buffer, 0, state.buffer.length, 0);
      if (amount !== state.buffer.length) throw new Error(`short pread: ${amount}`);
      checksum += amount + state.buffer[0] + state.buffer[state.buffer.length - 1];
    }
  }
  return checksum;
}

function measure(caseIndex, state, iterations, result) {
  const start = Bun.nanoseconds();
  result.checksum += runCaseOnce(caseIndex, state, iterations);
  return Bun.nanoseconds() - start;
}

function calibrate(caseIndex, state, result) {
  let iterations = caseIndex % 2 === 0 ? 4096 : 32;
  for (;;) {
    const elapsed = measure(caseIndex, state, iterations, result);
    if (elapsed >= minimumRoundNs) return iterations;
    const scale = Math.max(1.25, Math.min(8, minimumRoundNs / Math.max(elapsed, 1)));
    iterations = Math.max(iterations + 1, Math.floor(iterations * scale));
  }
}

// Deterministic xorshift shuffle: every implementation sees the same order,
// while read/write and 4 KiB/1 MiB cases alternate across rounds.
let randomState = 0x6d62756e;
function random() {
  randomState ^= randomState << 13;
  randomState ^= randomState >>> 17;
  randomState ^= randomState << 5;
  return (randomState >>> 0) / 0x100000000;
}

try {
  const states = sizes.map(makeState);
  const results = names.map((name, caseIndex) => ({
    name,
    iterations: 0,
    round_ns: [],
    checksum: 0,
    caseIndex,
  }));
  for (const result of results) {
    result.iterations = calibrate(result.caseIndex, states[result.caseIndex % 2], result);
  }
  const order = results.map((_, index) => index);
  for (let round = 0; round < rounds; round++) {
    for (let index = order.length - 1; index > 0; index--) {
      const swap = Math.floor(random() * (index + 1));
      [order[index], order[swap]] = [order[swap], order[index]];
    }
    for (const caseIndex of order) {
      const result = results[caseIndex];
      result.round_ns.push(measure(caseIndex, states[caseIndex % 2], result.iterations, result));
    }
  }
  for (const state of states) {
    closeSync(state.reader);
    closeSync(state.writer);
  }
  console.log(JSON.stringify({
    impl: `bun ${Bun.version}${Bun.revision ? ` (${Bun.revision.slice(0, 9)})` : ""}`,
    profile: "release-upstream",
    minimum_round_ns: minimumRoundNs,
    rounds,
    alignment: {
      payload_4k_mod_64: Number(ptr(states[0].payload)) % 64,
      payload_4k_mod_4k: Number(ptr(states[0].payload)) % 4096,
      payload_4k_mod_2m: Number(ptr(states[0].payload)) % (2 * 1024 * 1024),
      buffer_4k_mod_64: Number(ptr(states[0].buffer)) % 64,
      buffer_4k_mod_4k: Number(ptr(states[0].buffer)) % 4096,
      buffer_4k_mod_2m: Number(ptr(states[0].buffer)) % (2 * 1024 * 1024),
      payload_1m_mod_64: Number(ptr(states[1].payload)) % 64,
      payload_1m_mod_4k: Number(ptr(states[1].payload)) % 4096,
      payload_1m_mod_2m: Number(ptr(states[1].payload)) % (2 * 1024 * 1024),
      buffer_1m_mod_64: Number(ptr(states[1].buffer)) % 64,
      buffer_1m_mod_4k: Number(ptr(states[1].buffer)) % 4096,
      buffer_1m_mod_2m: Number(ptr(states[1].buffer)) % (2 * 1024 * 1024),
    },
    cases: Object.fromEntries(results.map(({ name, iterations, round_ns, checksum }) =>
      [name, { iterations, round_ns, checksum }])),
  }));
} finally {
  rmSync(directory, { force: true, recursive: true });
}
