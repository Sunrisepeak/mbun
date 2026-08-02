# Node buffer.transcode design

Issue: #44
Owner: `modules/jsc/src/builtins/node_buffer_extra.cppm`
Acceptance corpus: `compat/node/test/parallel/test-icu-transcode.js`

## Evidence

The fresh Linux bounded probe used the current coordinator binary, five jobs,
four candidate files, and one already-green Buffer guard. The result was
**1/5 files pass**: `test-buffer-fill.js` passed, while
`test-icu-transcode.js` failed at the first call because `buffer.transcode` was
not exposed. The three URL files failed on independent formatting/error
contracts and are not part of this owner.

## Contract and scope

- Expose `buffer.transcode` only in the Node process dialect; preserve Bun's
  existing `undefined`/not-implemented contract.
- Accept Buffer and Uint8Array sources and return a Buffer.
- Cover the Node acceptance encodings: utf8, latin1, ascii, and utf16le/ucs2
  aliases, including replacement of unrepresentable characters with `?` for
  latin1/ascii output.
- Reject invalid source/target encodings with Node's ICU-shaped error and keep
  the existing Buffer and module export identities intact.
- Keep the vendored upstream corpus unchanged and do not modify generic text or
  URL error formatting.

## TDD and verification

1. Keep `test-icu-transcode.js` as the read-only red acceptance test.
2. Add the smallest Node-dialect module export and conversion path.
3. Run the focused file, then a bounded Buffer/Node regression guard set with
   three to five jobs.
4. Re-run the five-file probe and record per-file results before updating the
   issue, coverage log, and PR comment.

## Verified result

The focused acceptance file passed **1/1**. The target plus ten Buffer/Node
guards passed **11/11 files** with three bounded jobs. The original five-file
probe moved from **1/5** to **2/5 files pass**; the three URL files retained
their independent failures. A Bun-dialect smoke preserved both transcode
exports as `undefined`. The root release build passed in **60.12 seconds**.
No full-corpus run or workspace-wide build was performed.

No local absolute paths, usernames, hostnames, credentials, private emails,
tokens, environment values, or machine identifiers belong in source comments,
commits, PR text, or copied logs.
