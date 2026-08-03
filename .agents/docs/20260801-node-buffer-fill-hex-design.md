# Node Buffer.fill contract validation design

Issue: #43  
Owner: `modules/jsc/src/builtins/node_buffer_extra.cppm`  
Acceptance corpus: `compat/node/test/parallel/test-buffer-fill.js`

## Evidence

The W59 bounded Node buffer probes used the default resource profile with three
jobs. The first 10-file slice had nine passes; the expanded 15-file slice had
13 passes and two failures. `test-buffer-fill.js` first failed at the upstream
assertion around the invalid hex patterns `"yKJh"` and a non-ASCII odd-length
pattern. Later assertions in the same file also exposed non-string encoding
arguments and a spoofed `length` property. These are runtime assertion failures,
not timeouts, skips, or harness errors.

The current `Buffer.prototype.fill` normalizes `hex`, allocates a temporary
buffer, and delegates to the lenient hex writer. It also coerced non-string
encoding arguments and trusted the observable `buf.length` property even when
it no longer matched the TypedArray backing length.

## Contract and scope

- For string fill values with `encoding === "hex"`, reject odd-length or
  non-hex input with Node's `TypeError` carrying `ERR_INVALID_ARG_VALUE`.
- Reject non-string encoding arguments with Node's `ERR_INVALID_ARG_TYPE`.
- Compare the visible length with the TypedArray length before and after value
  coercion, reporting `ERR_BUFFER_OUT_OF_BOUNDS` for a spoofed length.
- Preserve valid hex fill patterns, offsets, empty/inverted ranges, and all
  non-hex encodings.
- Keep `test-buffer-constants.js` separate; its failure is a generic JSC
  String capacity boundary and is not part of this owner.
- Do not change the read-only upstream corpus or global String behavior.

## TDD and verification

1. The upstream `test-buffer-fill.js` is the red acceptance test and remains
   unchanged.
2. Add only the smallest validation needed at the `Buffer.fill` contract
   boundary; leave the generic String capacity issue separate.
3. Run the focused Node file through the bounded runner.
4. Re-run the nine W59 passing Buffer leaves plus the focused file and inspect
   the diff-derived Buffer runtime guards before reporting green.

## Verified result

The focused file and nine guards passed **10/10 files** with three bounded jobs.
The complete W59 sample passed **14/15 files**; the sole remaining failure is
`test-buffer-constants.js`, outside this owner. The root release build passed
in **60.70 seconds**. The upstream corpus remained read-only and no full-corpus
run was performed.

No local absolute paths, usernames, hostnames, credentials, private emails,
tokens, environment values, or machine identifiers belong in source comments,
commits, PR text, or copied logs.
