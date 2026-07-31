// Promise throughput probe: 200k `.then` chain, then 200k Promise.resolve().then
// fanout, printing both in ms.
//
// WHY THIS EXISTS: modules/jsc/src/builtins/async_hooks.cppm instruments
// Promise.prototype.then, so a change there is paid by every promise in the
// process. A port of node's async_hooks that wrapped every reaction measured
// 4-5x on the chain here while latency_probe.py showed nothing -- its probes are
// dominated by startup and none of them exercise promise throughput. Run this
// (at least twice; the numbers move a few ms run to run) against the previous
// binary before landing anything that touches the promise path.
//
// Reference, this box, release build: chain200k ~12-18ms, fanout200k ~20-36ms.
const N = 200000;
function noop(v) { return v; }

async function chain() {
  const t0 = performance.now();
  let p = Promise.resolve(0);
  for (let i = 0; i < N; i++) p = p.then(noop);
  await p;
  return performance.now() - t0;
}

async function fanout() {
  const t0 = performance.now();
  const all = new Array(N);
  for (let i = 0; i < N; i++) all[i] = Promise.resolve(i).then(noop);
  for (let i = 0; i < N; i++) await all[i];
  return performance.now() - t0;
}

(async () => {
  const c = await chain();
  const f = await fanout();
  console.log(`chain${N / 1000}k ${c.toFixed(1)}ms  fanout${N / 1000}k ${f.toFixed(1)}ms`);
})();
