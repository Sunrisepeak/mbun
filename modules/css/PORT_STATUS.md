# CSS tokenizer and property-table port status

This checkpoint selectively preserves the tokenizer data model and a small
property metadata table from `agent/t4-7-css-tables` on the current baseline.

Source mapping:

- Rust: `src/css/lib.rs` (`Token`) and `src/css/css_parser.rs` (`Tokenizer`).
- Rust: `src/css/properties/properties_generated.rs` and
  `properties_impl.rs` (`PropertyId`, generated name lookup).
- Zig: `src/css/css_parser.zig` and
  `src/css/properties/properties_generated.zig` / `properties_impl.zig`.

The property subset uses allocation-free `string_view` metadata and matches
Bun's ASCII-case-insensitive standard-property lookup. CSS custom property
spelling remains untouched. The public token model carries zero-copy source
spans and mirrors the token categories used by the current generic engine.

The CSS Syntax L3 scanner now lives in `src/tokenizer.cppm` as the single public
producer `mbun::css::tokenize(std::string_view) -> std::vector<CssToken>`; the
transform engine (`src/css.cppm`) consumes it directly instead of carrying a
duplicate scanner. `tests/test_css_tokenizer.cpp` covers token classification,
zero-copy spans, escapes, and parse/serialize round-trip + minify idempotency.

DEFERRED:

- generation and migration of Bun's complete `Property`, `PropertyId`, and
  `PropertyIdTag` tables;
- typed property values, vendor-prefix payloads, and unknown-property payloads;
- numeric token payloads, `IdHash` versus `UnrestrictedHash`, and source
  locations (the tokenizer state machine itself is now shared and public).

Verification on 2026-07-13:

- `mcpp build` in `modules/css`: GCC 16.1.0, release build passed.
- `mcpp test` in `modules/css`: 2 test targets passed; 223 checks total,
  0 failures.
- Temporary LLVM 22.1.8 selection: the same build and test commands passed;
  the temporary member override was then removed.

`modules/css` has no local-index package dependency, so no `[indices]` section
is required in its member manifest.
