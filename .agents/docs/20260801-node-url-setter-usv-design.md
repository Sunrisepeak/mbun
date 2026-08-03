# Node URL setter USVString design

Issue: #47
Owner: Node URL setter string-conversion boundary
Acceptance corpus: `compat/node/test/parallel/test-whatwg-url-custom-setters.js`

## Evidence

The fresh W61/W62 URL candidate probe leaves this file as an independent
failure. The first case assigns an unpaired surrogate to `URL.username`; the
native setter raises `URIError: String contained an illegal UTF-16 sequence`.
Node URL setters apply WebIDL USVString conversion and continue with U+FFFD.
The adjacent URL parsing cluster is broader and remains separately parked.

## Contract and scope

- In Node dialect, apply the WebIDL USVString boundary to `href`, `protocol`,
  `username`, `password`, `host`, `hostname`, `port`, `pathname`, `search`,
  and `hash` before the native URL setter runs.
- Perform JavaScript `ToString` first, then normalize lone surrogates through
  the existing `util.toUSVString` boundary; reject Symbols with Node's
  `TypeError` contract.
- Keep Bun setter behavior, URL parsing, URL inspection, and generic Unicode
  APIs unchanged.

## TDD and verification

1. Keep the upstream setter file read-only as the red acceptance test.
2. Wrap only the Node URL setter string boundary.
3. The focused setter file is **1/1 pass** after the wrapper covers lone
   surrogates, object `ToString`, Symbols, and all URL setters in the corpus.
4. A five-file bounded regression with **5 jobs** is **4/5 files pass**:
   setters, transcode, URL inspect, and Buffer.fill pass; URL custom parsing
   remains the previously parked parser/message owner.
5. The final root release build passed in **60.86 seconds**. No full corpus or
   workspace-wide build was run; CI status remains a separate signal.

No local absolute paths, usernames, hostnames, credentials, private emails,
tokens, environment values, or machine identifiers belong in source comments,
commits, PR text, or copied logs.
