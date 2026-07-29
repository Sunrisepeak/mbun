# Wave 35 net/dgram tail static assessment

Baseline: `ec1bf69dbc26c6620355fd257d7f702653db834f`.

Scope source commit: `77f7fe1 fix(net): restore end and dgram error contracts`.
The compatibility corpus was read only. Per the requested time box, no build
or runtime test was started; every result below is a static expectation, not a
claimed green test.

## Changes

- `net.Socket` installs its default non-half-open enforcer as one real `end`
  listener. EOF from the reactor, drained parked reads, and `push(null)` now
  share the same next-microtask write-side shutdown path. This matches the
  observable listener shape and preserves the turn in which an `end` handler
  may write.
- `dgram` buffer-size failures now produce the Node-shaped `SystemError`:
  `ERR_SOCKET_BUFFER_SIZE`, detailed `info`, and enumerable `errno`/`syscall`
  accessors. The existing UDP wrapper already supplies EBADF context for an
  unbound handle.

## Static worklist coverage

| Compatibility file | Static disposition | Basis |
| --- | --- | --- |
| `test-net-socket-no-halfopen-enforcer.js` | expected candidate (new) | Default socket has exactly one internal `end` listener. |
| `test-net-allow-half-open.js` | expected candidate (new) | Default EOF closure is deferred through that listener; `allowHalfOpen: true` does not install it. |
| `test-dgram-socket-buffer-size.js` | expected candidate (new) | Unbound EBADF context is converted to the checked SystemError shape. |
| `test-net-local-address-port.js` | baseline candidate | `ec1bf69` already exposes `localFamily` with the local address state. |
| `test-net-remote-address-port.js` | baseline candidate | `ec1bf69` keeps remote fields undefined until peer adoption/connect. |
| `test-net-remote-address.js` | baseline candidate | Same pre-connect remote-address contract already exists in the baseline. |
| `test-net-large-string.js` | baseline candidate | Baseline's queued pre-connect writes avoid dropping a large initial payload. |

This provides 3 new and 4 baseline static candidates, exceeding the requested
three-candidate time-box target without representing unrun cases as green.

## Deferred backend-dependent cases

- `test-net-connect-options-fd.js`: Pipe descriptor ownership/identity needs
  the real transport handle path.
- `test-net-end-close.js`: relies on the stream-base injected-handle seam.
- `test-net-persistent-nodelay.js`: observes the native TCP wrapper call.
- `test-net-connect-reset-until-connected.js` and `test-net-connect-reset.js`:
  require real TCP reset timing and error delivery.

`changelog.md` is intentionally unchanged: this wave has static evidence only,
not runtime verification.

## Static verification performed

```text
git diff --check
clang-format --dry-run --Werror --style=file --lines=... modules/jsc/src/js_{net,dgram}.cppm
{ net payload part 1; net payload part 2; } | node --check
dgram payload | node --check
```

All commands above exited successfully. No build or compatibility command was
run by instruction.
