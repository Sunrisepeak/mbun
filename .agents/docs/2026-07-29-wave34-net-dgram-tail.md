# Wave34 Net and Dgram Tail Assessment

Baseline: `928c5d7`; branch: `codex/wave34-net-dgram`.

## Static expected greens

| Node corpus file | Source change | Expected result |
| --- | --- | --- |
| `test-net-normalize-args.js` | Export and recognize the shared `internal/net` normalized-arguments marker. | Green: normalized arrays carry the exact internal symbol and reconnect through `Socket.connect()`. |
| `test-dgram-send-queue-info.js` | Track pending UDP send bytes/count while Node's explicit no-try-send test mode is active. | Green: two immediate sends expose the expected queue counters. |
| `test-net-buffersize.js` | Hold pre-connect writes until the public connect edge. | Green: `bufferSize` reflects queued pre-connect bytes and drains after connect. |
| `test-net-local-address-port.js` | Initialize the observable local address family. | Green: accepted IPv4 socket reports the listener's address, port, and family. |
| `test-net-remote-address-port.js` | Leave client peer fields undefined before connect and populate them after dialing. | Green: pre-connect and connected remote-address contracts agree. |
| `test-net-remote-address.js` | Share the same remote-address lifecycle correction. | Green: a connecting socket has no remote address until connect. |

No build or runtime test was run: Wave34 explicitly prohibits both. These are static estimates, not executed corpus results.

## Deferred backend work

| Files | Blocking capability |
| --- | --- |
| `test-net-connect-options-ipv6.js`, `test-net-dns-custom-lookup.js`, `test-dgram-udp6-link-local-address.js` | Native IPv6 address parsing, scoped link-local binding, and IPv6 reactor dialing. |
| `test-net-autoselectfamily-ipv4first.js`, `test-net-autoselectfamily-commandline-option.js`, `test-net-autoselectfamily-default.js`, `test-net-autoselectfamily.js` | Real Happy-Eyeballs parallel attempts, native errors, and address ordering. |
| `test-net-bytes-stats.js`, `test-net-connect-memleak.js`, `test-net-connect-options-fd.js`, `test-net-end-close.js`, `test-net-large-string.js`, `test-net-persistent-nodelay.js`, `test-net-socket-no-halfopen-enforcer.js`, `test-net-allow-half-open.js` | Stream/handle ownership, GC, pipe FD adoption, and close-half lifecycle validation. |
| `test-dgram-socket-buffer-size.js` | Native UDP socket-buffer errno/value behavior. |
| `test-net-connect-reset-until-connected.js`, `test-net-connect-reset.js` | TCP RST generation and peer error delivery. |

## Static checks

- `git diff --check` passed.
- Extracted net (both payload parts) and dgram JavaScript passed `node --check`.
- `clang-format --dry-run --Werror` passed for every changed line range.
- Whole-file clang-format on `js_net.cppm` fails at an existing violation at
  line 3153, so it is not a clean-file signal for this change.

`compat/` stayed read-only. `changelog.md` was not changed because no runtime
measurement exists for the required evidence record.
