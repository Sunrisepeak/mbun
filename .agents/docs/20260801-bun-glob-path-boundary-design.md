# Bun.Glob path-boundary compatibility design

Issue: #42  
Owner: `modules/glob/src/glob.cppm`  
Acceptance corpus: `compat/bun/test/js/bun/glob/path-length.test.ts`

## Evidence

The W56 bounded Linux lane measured five real Bun files with the default
runner profile (`MemoryMax=4G`, `TasksMax=512`, three concurrent jobs): 4/5
files green, 60/65 tests passed, 5 failed, and 3287 expects. The only red file
was `glob/path-length.test.ts`, with 1/6 tests passed. Its failures are all
path-boundary cases; the self-referential symlink case passes.

The current `mbun::glob::detail::Walker` constructs dynamic
`std::filesystem::path` and `std::string` values. It does not expose a
path-too-long result when a directory path or pattern exceeds Bun's platform
path policy. The JS callback consequently returns an array where the native
Bun contract raises an `ENAMETOOLONG` error.

## Contract and scope

- Reuse the existing platform path policy, including Linux's 4096-byte limit
  and the smaller limits used by other supported POSIX targets.
- Check the input pattern before component normalization. This preserves the
  `./` and `../` overflow contract instead of dropping those components first.
- Check a directory path before opening/iterating it. A directory path that
  exceeds the policy reports `ENAMETOOLONG`; a matched file whose emitted
  logical path exceeds the join-buffer boundary remains returnable, matching
  the native test's explicit exception.
- Propagate one `std::error_code` from the glob scan API to the JS callback;
  map `ENAMETOOLONG` through the existing Node-shaped filesystem error helper.
- Preserve matching, result ordering, symlink-cycle detection, permissions,
  and the existing default file-only behavior.

## Verification gate

1. Add a focused unit regression for the scan error output before changing the
   implementation; confirm it is red against the current API/behavior.
2. Implement the smallest path-boundary/error propagation change.
3. Run the glob unit target and the focused native Bun path-length file under
   the bounded runner.
4. Re-run the four W56 green guards (`semver`, `glob/match`, `glob/proto`, and
   `toUTF16Alloc`) plus the existing W54 guard set. Do not run the full corpus
   or a workspace-wide build for this owner.

No local absolute paths, usernames, hostnames, credentials, tokens, private
URLs, environment values, or machine identifiers belong in source comments,
commits, PR text, or copied logs.
