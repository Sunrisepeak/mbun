# Node URL inspect design

Issue: #45
Owner: Node URL custom-inspect compatibility boundary
Acceptance corpus: `compat/node/test/parallel/test-whatwg-url-custom-inspect.js`

## Evidence

The W61 fresh Linux probe measured this file as one of three independent URL
failures. The current custom inspector emits JSON-style double-quoted strings,
includes `toJSON` and `toString` in the visible shape, ignores `showHidden`,
and hard-codes the header as `URL`, so the Node file fails before its hidden
context and subclass assertions.

## Contract and scope

- In Node dialect, render URL fields with Node's single-quoted inspect form and
  Node's visible field order.
- Omit `toJSON`/`toString` from the normal visible shape.
- Render the URL context fields only for `showHidden: true`.
- Respect the dynamic URL constructor name and return `<Name> {}` at depth zero.
- Keep Bun dialect output and generic URL parsing/setter/error behavior unchanged.
- Leave the adjacent custom-parsing and custom-setters failures as separate
  owners.

## TDD and verification

1. Keep the upstream URL file read-only as the red acceptance test.
2. Change only the Node URL custom-inspect boundary.
3. Run the focused file, then the two remaining URL files plus Buffer/Node
   guards with three to five bounded jobs.
4. Record before/after file counts and keep CI status separate from local runs.

## Verified result

The focused acceptance file passed **1/1**. The five-file candidate set passed
**3/5** after the change: URL inspect, buffer.transcode, and Buffer.fill are
green; custom parsing and custom setters remain separate failures. The Bun
dialect smoke retained its original inspect shape. The root release build
passed in **60.24 seconds**. No full-corpus run or workspace-wide build was
performed.

No local absolute paths, usernames, hostnames, credentials, private emails,
tokens, environment values, or machine identifiers belong in source comments,
commits, PR text, or copied logs.
