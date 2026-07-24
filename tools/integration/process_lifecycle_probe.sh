#!/usr/bin/env bash
# process_lifecycle_probe.sh — node-differential probe for the process shutdown
# sequence: 'exit' / 'beforeExit' emission, exit-code propagation, the uncaught
# exception path, and handle ref/unref.
#
# Why a probe and not corpus files: every one of these behaviours is a *whole
# process* property (what status did it leave with, did it leave at all), so it
# can only be observed from outside. The corpus can tell you 580 files hang; it
# cannot tell you that `process.on('exit')` never fired — the very hole that let
# ~62% of the node corpus run its `common.mustCall` verifier never once.
#
# Each case states the expected stdout and exit status, taken from real node
# (v24). Run it against a candidate binary:
#
#   tools/integration/process_lifecycle_probe.sh ./target/<triple>/<hash>/bin/mbun
#
# Every case executes through bounded_run.py's sandbox (via safe-test.sh), so a
# case that hangs is killed and reported rather than wedging the machine.
set -uo pipefail

BIN="${1:?usage: process_lifecycle_probe.sh <mbun-binary> [node-binary]}"
REF="${2:-}"                       # optional: cross-check the cases against node
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

PASS=0
FAIL=0
TIMEOUT_SEC=15

# case <name> <expected-status> <expected-stdout>  (script arrives on stdin).
# An expected stdout of "@any" checks the exit status only.
probe() {
  local name="$1" wantStatus="$2" wantOut="$3"
  local file="$WORK/$name.js"
  cat > "$file"

  local out status
  out="$("$HERE/safe-test.sh" "$TIMEOUT_SEC" "$BIN" "$file" 2>/dev/null)"
  status=$?
  [ "$wantOut" = "@any" ] && wantOut="$out"
  # safe-test.sh reports a killed scope as 124; keep that distinct from a real
  # non-zero exit so a hang never reads as "wrong status".
  if [ "$status" = 124 ]; then
    printf 'FAIL %-28s hung (killed after %ss)\n' "$name" "$TIMEOUT_SEC"
    FAIL=$((FAIL + 1))
    return
  fi
  if [ "$out" = "$wantOut" ] && [ "$status" = "$wantStatus" ]; then
    printf 'ok   %-28s status=%s\n' "$name" "$status"
    PASS=$((PASS + 1))
  else
    printf 'FAIL %-28s status=%s (want %s)\n' "$name" "$status" "$wantStatus"
    printf '       stdout: %q\n         want: %q\n' "$out" "$wantOut"
    FAIL=$((FAIL + 1))
  fi
  if [ -n "$REF" ]; then
    local refOut refStatus
    refOut="$("$REF" "$file" 2>/dev/null)"
    refStatus=$?
    if [ "$refOut" != "$wantOut" ] || [ "$refStatus" != "$wantStatus" ]; then
      printf '       NOTE: reference %s disagrees with the expectation: status=%s stdout=%q\n' \
        "$REF" "$refStatus" "$refOut"
    fi
  fi
}

# ---- 'exit' / 'beforeExit' ------------------------------------------------

probe natural-drain 0 'main
beforeExit 0
exit 0' <<'JS'
process.on('exit', (c) => console.log('exit', c));
process.on('beforeExit', (c) => console.log('beforeExit', c));
console.log('main');
JS

# node: an explicit exit() skips 'beforeExit' entirely and carries its code.
probe explicit-exit 3 'exit 3' <<'JS'
process.on('exit', (c) => console.log('exit', c));
process.on('beforeExit', () => console.log('beforeExit SHOULD NOT RUN'));
process.exit(3);
JS

# node: process.exitCode set at top level is the process's status, and
# 'beforeExit'/'exit' see it.
probe exit-code-slot 5 'exit 5' <<'JS'
process.on('exit', (c) => console.log('exit', c));
process.exitCode = 5;
JS

# node: a listener may still change the status; later listeners keep the
# original argument (per_thread.js re-reads the slot only after the emit).
probe exit-code-mutation 42 'exit1 0
exit2 0' <<'JS'
process.on('exit', (c) => { console.log('exit1', c); process.exitCode = 42; });
process.on('exit', (c) => { console.log('exit2', c); });
JS

# node: a throw inside an 'exit' listener is fatal — status 1, and the
# remaining listeners never run.
probe throw-in-exit-listener 1 'exit' <<'JS'
process.on('exit', () => { console.log('exit'); throw new Error('boom'); });
process.on('exit', () => { console.log('SHOULD NOT RUN'); });
JS

# node: a 'beforeExit' listener that schedules work re-arms the loop, so
# beforeExit fires again; 'exit' comes only after it stops.
probe before-exit-rearm 0 'beforeExit 0
beforeExit 1
beforeExit 2
exit 0' <<'JS'
let n = 0;
process.on('beforeExit', () => { console.log('beforeExit', n); if (++n < 3) setTimeout(() => {}, 1); });
process.on('exit', (c) => console.log('exit', c));
JS

# node: 'exit' listeners run synchronously and the loop is over — a timer armed
# from one never fires. (node does still drain its microtask queue once more on
# the way out; mbun leaves immediately, which is stricter, so this case asserts
# only the timer invariant both agree on.)
probe exit-listener-is-sync 0 'exit' <<'JS'
process.on('exit', () => {
  console.log('exit');
  setTimeout(() => console.log('TIMER SHOULD NOT RUN'), 0);
});
JS

# node: a fatal uncaught exception still emits 'exit', with status 1, and never
# emits 'beforeExit'.
probe fatal-still-emits-exit 1 'exit 1' <<'JS'
process.on('exit', (c) => console.log('exit', c));
process.on('beforeExit', () => console.log('beforeExit SHOULD NOT RUN'));
throw new Error('fatal');
JS

# ---- the uncaught exception path -----------------------------------------

# node: a throw escaping a timer callback is an uncaught exception (status 1),
# not something the runtime may swallow.
probe throw-in-timer 1 '' <<'JS'
setTimeout(() => { throw new Error('boom-timer'); }, 10);
JS

# node: a throw escaping a socket/server callback is likewise fatal — and,
# critically, the process must still LEAVE (the swallowed version left the
# listening handle registered and hung forever).
probe throw-in-listen-callback 1 '' <<'JS'
const net = require('net');
const s = net.createServer(() => {});
s.listen(0, () => { throw new Error('boom-listen'); });
JS

# node: an installed 'uncaughtException' listener claims it and the loop
# continues.
probe uncaught-handler-claims 0 'caught boom
after' <<'JS'
process.on('uncaughtException', (e) => console.log('caught', e.message));
setTimeout(() => { throw new Error('boom'); }, 1);
setTimeout(() => { console.log('after'); }, 20);
JS

# node: a throw from process.nextTick is an uncaught exception, not an
# unhandled rejection.
probe throw-in-nexttick 0 'caught tick' <<'JS'
process.on('uncaughtException', (e) => console.log('caught', e.message));
process.nextTick(() => { throw new Error('tick'); });
JS

# ---- handle ref / unref ---------------------------------------------------

# node: an unref'd server does not hold the loop open, even when unref() was
# called before listen() — the sticky-intent case that used to hang.
probe server-unref-before-listen 0 'listening' <<'JS'
const net = require('net');
const server = net.createServer((s) => s.resume()).unref();
server.listen(() => console.log('listening'));
JS

probe server-unref-after-listen 0 'listening' <<'JS'
const net = require('net');
const server = net.createServer((s) => s.resume());
server.listen(() => { console.log('listening'); server.unref(); });
JS

# node: ref() puts the hold back, so this one must be kept alive by the server
# and leave only when it closes.
probe server-ref-restores 0 'listening
closed' <<'JS'
const net = require('net');
const server = net.createServer((s) => s.resume()).unref();
server.listen(() => {
  console.log('listening');
  server.ref();
  setTimeout(() => server.close(() => console.log('closed')), 20);
});
JS

# hasRef() on the public Server/Socket is additive: node keeps it on the
# internal handle (server._handle.hasRef()), so the reference cross-check below
# will report a disagreement here — expected, not a defect.
probe server-has-ref 0 'true false true' <<'JS'
const net = require('net');
const server = net.createServer();
const a = server.hasRef();
server.unref();
const b = server.hasRef();
server.ref();
console.log(a, b, server.hasRef());
JS

probe dgram-unref 0 'bound' <<'JS'
const dgram = require('dgram');
const socket = dgram.createSocket('udp4');
socket.on('listening', () => console.log('bound'));
socket.bind();
socket.unref();
JS

# ---- what all of this is for: mustCall must actually be enforced ----------
# node fails this file (the callback is registered for 2 calls and made once);
# with no 'exit' event the verifier never ran and mbun scored it as a pass.
probe mustcall-undercall-fails 1 '@any' <<JS
const common = require('$(cd "$HERE/../.." && pwd)/compat/node/test/common');
const f = common.mustCall(() => {}, 2);
f();
JS

probe mustcall-satisfied-passes 0 '@any' <<JS
const common = require('$(cd "$HERE/../.." && pwd)/compat/node/test/common');
const f = common.mustCall(() => {}, 2);
f(); f();
JS

echo
echo "process lifecycle probe: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
