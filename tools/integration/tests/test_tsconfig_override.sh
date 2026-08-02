#!/usr/bin/env bash
# Direct CLI/runtime contract for --tsconfig-override. This intentionally runs
# the built executable: unit parser coverage cannot prove diagnostics are
# non-fatal, nor that Worker/fork keep the parse-time cwd after process.chdir().
set -euo pipefail

if [ "$#" -ne 1 ]; then
  echo "usage: $0 <mbun-bin>" >&2
  exit 2
fi

bin=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

mkdir -p "$tmp/project/config" "$tmp/project/src" "$tmp/project/elsewhere"

cat >"$tmp/project/config/tsconfig.json" <<'EOF'
{
  "compilerOptions": {
    "baseUrl": "..",
    "paths": { "#/*": ["src/*"] }
  }
}
EOF
cat >"$tmp/project/config/malformed.json" <<'EOF'
{ invalid
EOF
cat >"$tmp/project/src/value.ts" <<'EOF'
export const value = 42;
EOF
cat >"$tmp/project/dep.ts" <<'EOF'
console.log("DEP_EFFECT");
EOF
cat >"$tmp/project/entry.ts" <<'EOF'
import "./dep";
console.log("SIDE_EFFECT");
process.exitCode = 7;
EOF
cat >"$tmp/project/behavior.test.ts" <<'EOF'
import { test, expect } from "bun:test";
console.log("TEST_SIDE_EFFECT");
test("still runs", () => expect(42).toBe(42));
EOF
ln -s "$bin" "$tmp/node"

run_runtime_error_case() {
  local name=$1 config=$2 diagnostic=$3
  set +e
  (cd "$tmp/project" && "$bin" --tsconfig-override "$config" entry.ts) \
    >"$tmp/$name.out" 2>"$tmp/$name.err"
  local rc=$?
  set -e
  [ "$rc" -eq 7 ] || {
    echo "$name: expected script exit 7 after diagnostic, got $rc" >&2
    cat "$tmp/$name.out" "$tmp/$name.err" >&2
    exit 1
  }
  grep -Fxq 'DEP_EFFECT' "$tmp/$name.out"
  grep -Fxq 'SIDE_EFFECT' "$tmp/$name.out"
  grep -Fq "$diagnostic" "$tmp/$name.err"
}

run_runtime_error_case missing config/missing.json 'Cannot find tsconfig file'
run_runtime_error_case malformed config/malformed.json 'Cannot parse tsconfig file'

for config in config/missing.json config/malformed.json; do
  name=${config##*/}
  set +e
  (cd "$tmp/project" && "$bin" --tsconfig-override "$config" -e \
    'console.log("EVAL_EFFECT"); process.exitCode = 7') \
    >"$tmp/eval-$name.out" 2>"$tmp/eval-$name.err"
  rc=$?
  set -e
  [ "$rc" -eq 7 ] || {
    echo "eval $name: expected exit 7, got $rc" >&2
    cat "$tmp/eval-$name.out" "$tmp/eval-$name.err" >&2
    exit 1
  }
  grep -Fxq 'EVAL_EFFECT' "$tmp/eval-$name.out"
  grep -Eq 'Cannot (find|parse) tsconfig file' "$tmp/eval-$name.err"

  set +e
  (cd "$tmp/project" && "$bin" build --tsconfig-override "$config" entry.ts \
    --outfile "$tmp/bundle-$name.js") >"$tmp/build-$name.out" 2>"$tmp/build-$name.err"
  rc=$?
  set -e
  [ "$rc" -eq 1 ] || {
    echo "build $name: expected exit 1, got $rc" >&2
    cat "$tmp/build-$name.out" "$tmp/build-$name.err" >&2
    exit 1
  }
  grep -Eq 'Cannot (find|parse) tsconfig file' "$tmp/build-$name.err"

  set +e
  (cd "$tmp/project" && "$tmp/node" --tsconfig-override "$config" entry.ts) \
    >"$tmp/node-$name.out" 2>"$tmp/node-$name.err"
  rc=$?
  set -e
  [ "$rc" -eq 7 ] || {
    echo "node $name: expected script exit 7, got $rc" >&2
    cat "$tmp/node-$name.out" "$tmp/node-$name.err" >&2
    exit 1
  }
  grep -Fxq 'SIDE_EFFECT' "$tmp/node-$name.out"
  grep -Eq 'Cannot (find|parse) tsconfig file' "$tmp/node-$name.err"

  (cd "$tmp/project" && "$bin" test --tsconfig-override "$config" behavior.test.ts) \
    >"$tmp/test-$name.out" 2>"$tmp/test-$name.err"
  grep -Fq 'TEST_SIDE_EFFECT' "$tmp/test-$name.out"
  grep -Eq 'Cannot (find|parse) tsconfig file' "$tmp/test-$name.err"

  printf '.exit\n' | (cd "$tmp/project" && "$bin" --tsconfig-override "$config" -i) \
    >"$tmp/repl-$name.out" 2>"$tmp/repl-$name.err"
  grep -Fq 'Welcome to Node.js' "$tmp/repl-$name.out"
  grep -Eq 'Cannot (find|parse) tsconfig file' "$tmp/repl-$name.err"
done

cat >"$tmp/project/worker.ts" <<'EOF'
import { parentPort } from "worker_threads";
import { value } from "#/value";
parentPort.postMessage({
  value,
  execArgv: process.execArgv,
  leaked: Object.prototype.hasOwnProperty.call(process.env, "MBUN_INTERNAL_TSCONFIG_OVERRIDE"),
});
EOF
cat >"$tmp/project/fork-child.ts" <<'EOF'
import { value } from "#/value";
process.send({
  value,
  execArgv: process.execArgv,
  leaked: Object.prototype.hasOwnProperty.call(process.env, "MBUN_INTERNAL_TSCONFIG_OVERRIDE"),
});
EOF
cat >"$tmp/project/plain-child.ts" <<'EOF'
console.log(Object.prototype.hasOwnProperty.call(process.env, "MBUN_INTERNAL_TSCONFIG_OVERRIDE") ? "LEAK" : "NO_LEAK");
EOF
cat >"$tmp/project/propagate.ts" <<'EOF'
import path from "path";
import { Worker } from "worker_threads";
import { fork, spawnSync } from "child_process";

const root = process.cwd();
const expected = ["--tsconfig-override", "config/tsconfig.json"];
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);
if (!same(process.execArgv, expected)) throw new Error(`raw parent execArgv changed: ${JSON.stringify(process.execArgv)}`);
if (Object.prototype.hasOwnProperty.call(process.env, "MBUN_INTERNAL_TSCONFIG_OVERRIDE")) throw new Error("internal override leaked to parent JS");

process.chdir(path.join(root, "elsewhere"));

const workerResult = await new Promise((resolve, reject) => {
  const worker = new Worker(path.join(root, "worker.ts"));
  worker.once("message", resolve);
  worker.once("error", reject);
});
if (workerResult.value !== 42 || workerResult.leaked || !same(workerResult.execArgv, expected)) {
  throw new Error(`worker propagation mismatch: ${JSON.stringify(workerResult)}`);
}

const forkResult = await new Promise((resolve, reject) => {
  const child = fork(path.join(root, "fork-child.ts"), [], { silent: true });
  child.once("message", resolve);
  child.once("error", reject);
});
if (forkResult.value !== 42 || forkResult.leaked || !same(forkResult.execArgv, expected)) {
  throw new Error(`fork propagation mismatch: ${JSON.stringify(forkResult)}`);
}

const plain = spawnSync(process.execPath, [path.join(root, "plain-child.ts")], {
  cwd: process.cwd(), encoding: "utf8",
});
if (plain.status !== 0 || plain.stdout.trim() !== "NO_LEAK") {
  throw new Error(`ordinary spawn inherited internal state: ${JSON.stringify(plain)}`);
}
console.log("PROPAGATION_OK");
EOF

(cd "$tmp/project" && "$bin" --tsconfig-override config/tsconfig.json propagate.ts) \
  >"$tmp/propagate.out" 2>"$tmp/propagate.err"
grep -Fxq 'PROPAGATION_OK' "$tmp/propagate.out"
[ ! -s "$tmp/propagate.err" ] || {
  echo "propagation emitted stderr" >&2
  cat "$tmp/propagate.err" >&2
  exit 1
}

echo "test_tsconfig_override: ok"
