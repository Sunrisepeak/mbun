# Corpus coverage W42 handoff

Date: 2026-08-02
Base: `rewrite_bun_in_mcpp` at `357c6a0`
PR: #79

## Objective and policy

Linux-first, bounded progress toward the Node and Bun native corpora. The
parallel screen used four independent processes with one worker each. Parallel
results are triage data only; a file counts as green only after a serial
re-run on the candidate binary. No full corpus run was started.

Local paths, usernames, credentials, tokens, host identifiers, and private
environment values are intentionally omitted from this handoff.

## Baseline screen

| Lane | Dispatched | Pass | Fail | Skipped | Timeout | Decision |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| test-runner | 40 | 18 | 15 | 3 | 4 | measured, defer deeper runner owners |
| util | 30 | 18 | 9 | 2 | 1 | take custom promisify name owner |
| webcrypto | 50 | 31 | 19 | 0 | 0 | take branded CryptoKey HMAC owner; defer modern/asymmetric owners |
| process | 96 | 85 | 6 | 4 | 1 | take internal binding allowlist owner |

The frozen whole-corpus reference remains Node 3,134/4,433 (70.7%), Node
excluding self-skips 3,134/3,919 (80.0%), Bun 1,015/1,902 (53.4%), combined
4,149/6,335 (65.5%). These figures were not recomputed in W42.

## Source and test changes

- `2d9ff3a` adds the hidden-slot CryptoKey-to-secret-material bridge for
  legacy `node:crypto` HMAC and a focused JSC regression assertion.
- `6527ee9` preserves intentional custom promisify names, fills anonymous or
  `value` placeholders from the original callback API, and adds explicit
  shape-only inspector and zlib internal-binding namespaces. It also adds the
  focused Node compatibility seam test.

The inspector and zlib namespaces are capability boundaries, not claims of a
working inspector protocol or native zlib backend. Unsupported calls remain
explicitly unsupported.

## Verification evidence

- GCC 16.1.0 focused JSC target: `test_webcrypto` passed.
- GCC 16.1.0 focused JSC target: `test_node_compat_bridges` passed.
- Candidate serial corpus gate: 2/3 passed, 1/3 failed, 0 timed out.
- The two green files are `test-util-promisify-custom-names.mjs` and
  `test-process-binding-internalbinding-allowlist.js`.
- The remaining WebCrypto file passed its HMAC CryptoKey bridge assertion but
  then hit the pre-existing unsupported EC `node:crypto` signing backend. It is
  retained as a deferred owner and excluded from the green count.

## Next route

1. Treat GCC and LLVM Linux CI as the integration gate for PR #79.
2. If Linux CI is green, keep the PR scoped and ready for maintainer merge.
3. Continue WebCrypto asymmetric signing only as a separate owner requiring a
   real OpenSSL/KeyObject bridge; do not broaden W42 or rerun the full corpus
   for this checkpoint.

The repository rule for filtering local sensitive information is already
centralized in `hagent/agents.md`, and this PR follows it for source comments,
commit messages, README text, PR title/body/comments, and handoff records.
