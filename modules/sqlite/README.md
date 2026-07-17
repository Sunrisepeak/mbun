# mbun.sqlite

This member is the first pure-logic translation layer for Bun's SQLite APIs.
It follows the Rust rewrite in `src/js/internal/sql/sqlite.ts`,
`src/sql/shared`, and `src/sql_jsc`, with the Zig `src/sql` and
`src/sql_jsc` trees used as the algorithm and boundary cross-check.

The initial API surface contains:

- SQL command classification, result metadata, and SQLite value kinds;
- column identifiers and row/value containers;
- statement metadata, result modes, lifecycle, and changes shape;
- SQLite transaction command generation and option validation.

The sqlite3 database, statement prepare/bind/step/finalize calls, JSC value
conversion, extension loading, serialization, and filesystem-backed opening
are `DEFERRED(S1)`. The deferred result is explicit in the statement API so
the pure logic layer can be compiled and tested without a native dependency.
