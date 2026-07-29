# Wave33 HTTP/2 Tail Assessment

Baseline: `069117f`; branch: `codex/wave33-http2-tail`.

## Static expected greens

| Node corpus file | Source change | Expected result |
| --- | --- | --- |
| `test-http2-respond-file-fd-leak.js` | Close an owned file descriptor and route a rejected response header through the stream error path. | Green: invalid pseudo-header still reports `ERR_HTTP2_INVALID_PSEUDOHEADER`; no descriptor leak. |
| `test-http2-compat-socket.js` | Publish the session timeout handle under `internal/timers`' `kTimeout` symbol. | Green: compat proxy `setTimeout()` exposes the armed timeout. |
| `test-http2-socket-proxy.js` | Publish the same timeout handle for direct session socket proxies. | Green: the proxy's timeout inspection follows the session timer. |
| `test-http2-client-request-listeners-warning.js` | Batch pre-connect stream `ready` notifications behind one session `connect` listener. | Green: more than ten early requests do not create a warning. |
| `test-http2-connect-method-extended-cant-turn-off.js` | Retain initial `enableConnectProtocol` settings and reject a subsequent disable as a protocol error. | Green: the peer receives the required connection-error path. |
| `test-http2-create-client-secure-session.js` | Expose encrypted and origin-set state on secure server sessions. | Green: TLS server sessions report the same state surface as secure clients. |

No build or runtime test was run: the Wave33 instruction explicitly prohibits both. The entries above are static estimates, not executed corpus outcomes.

## Deferred backend work

| Node corpus file | Blocking capability |
| --- | --- |
| `test-http2-stream-client.js` | Requires a real end-to-end HTTP/2 stream/inspection run to distinguish transport timing from the already-present inspect surface. |
| `test-http2-options-max-reserved-streams.js` | Push-promise reservation, RST timing, and stream lifecycle need protocol-level validation. |
| `test-http2-altsvc.js` | ALTSVC frame delivery and origin serialization need end-to-end protocol validation. |
| `test-http2-propagate-session-destroy-code.js` | Session GOAWAY error-code propagation and stream teardown are coupled lifecycle work. |
| `test-http2-alpn.js` | TLS ALPNCallback negotiation belongs to the real TLS backend. |
| `test-http2-connect-method.js` | CONNECT bidirectional data forwarding requires full stream/socket data-plane verification. |

## Static checks

- `git diff --check` passed.
- The concatenated raw JavaScript from `js_http2.cppm` and
  `js_http2_part2.cppm` passed `node --check`.
- `clang-format --dry-run --Werror` passed for all changed line ranges.
- Whole-file clang-format on `js_http2.cppm` fails at an existing violation at
  line 2540, so it is not a clean-file signal for this change.

`compat/` stayed read-only. `changelog.md` was not changed because no runtime
evidence exists to support the repository's required measurement record.
