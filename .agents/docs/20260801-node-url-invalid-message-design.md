# Node URL invalid-message design

Issue: #46
Owner: Node URL constructor error boundary
Acceptance corpus: `compat/node/test/parallel/test-whatwg-url-custom-parsing.js`

## Evidence

The fresh five-file candidate probe still has one isolated URL parsing failure
after the inspect owner was closed. The first failing assertion receives a
TypeError with the native message describing the input and base, while Node's
contract requires `ERR_INVALID_URL` with the message `Invalid URL`. The URL
setter file has a separate lone-surrogate Unicode owner and is not included.

## Triage result: parked

An uncommitted diagnostic wrapper normalized the first native error to
`Invalid URL`, but the focused file then exposed **9 invalid inputs that the
native parser accepts**. The failure is therefore a parser-algorithm/URL
compatibility cluster, not a message-only owner. The diagnostic wrapper was
reverted and no source commit was made.

- Keep the native parser unchanged until an implementation owner covers the
  invalid-host/code-point/IP cases without guessing.
- Keep valid URL construction, URL subclassing, prototype identity, and Bun
  dialect behavior unchanged.
- Keep URL setter Unicode handling as a separate owner.

## TDD and verification

1. Keep the upstream parsing file read-only as the red acceptance test.
2. Use a diagnostic-only experiment to distinguish error wording from parser
   acceptance; do not commit a partial message wrapper.
3. Record the focused failure and the nine additional no-throw cases before
   selecting a future parser owner.
4. Keep CI status separate from this local triage.

The focused file stayed red after the experiment; no green file or source
commit is claimed from this candidate.

No local absolute paths, usernames, hostnames, credentials, private emails,
tokens, environment values, or machine identifiers belong in source comments,
commits, PR text, or copied logs.
