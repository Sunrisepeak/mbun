# `mbun.valkey`

This is the first pure-data-model translation of Bun's Valkey/Redis client.
The boundaries follow Bun's Rust `src/valkey/valkey_protocol.rs` and
`src/runtime/valkey_jsc/ValkeyCommand.rs`, with the RESP tags and URI aliases
checked against the Zig implementation in `src/valkey/valkey_protocol.zig` and
`src/runtime/valkey_jsc/valkey.zig`.

The member currently provides:

- RESP2/RESP3 tags, values, a bounded recursive reader, typed accessors
  (`as_string`/`as_integer`/`as_double`/`as_boolean`/`error_message` +
  `apply_return_as_bool`), and display helpers;
- `ReplyScanner` — incremental, cross-segment reply framing (bun's
  `ReplyScanner::scan`) — plus a single-shot `parse_reply` that returns a
  status + consumed-byte count in the style of `mbun.http`'s `parse_request`;
- RESP command serialization, command metadata bits, and `cmd::*` builders for
  the common commands (GET/SET/DEL/INCR/EXISTS/EXPIRE/TTL/HSET/HGET/SISMEMBER/
  PING);
- protocol aliases, TCP/Unix address data, connection options, status, flags,
  and per-context state.

Codec behavior is covered by `tests/test_protocol.cpp` (encode/decode
round-trips for every RESP2/RESP3 type, byte-at-a-time incremental parsing,
pipelined and split-segment framing, malformed-input rejection, and typed
return conversion).

The socket, TLS, event-loop, JSC Promise, and reconnect backends are
`DEFERRED(S-net)`. No package dependency is needed by this pure layer, so its
`mcpp.toml` intentionally has no `[indices]` section; add the local `mbun`
index when a backend package is introduced.

## Initial build checkpoint

Command: `cd modules/valkey && mcpp build`
Toolchain: GCC 16.1.0
Result: passed (`Finished release [optimized]`)
First error: none; the initial build completed without a compiler error.
