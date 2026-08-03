# mbun.postgres

This member is the pure-logic starting point for Bun's PostgreSQL SQL client.
It follows the Rust rewrite in `src/sql/postgres`, `src/sql/shared`, and
`src/sql_jsc/postgres`, with the Zig `src/sql/postgres` tree used as the
protocol and boundary cross-check.

The initial API surface contains PostgreSQL wire constants and query framing,
parameter values and array escaping, field/row metadata, PostgreSQL type and
error names, result modes, and query/statement lifecycle shapes.

Socket transport, TLS, authentication, pooling, JSC conversion, and server
execution are `DEFERRED(S-net)`. The query API exposes an explicit deferred
backend seam so these pure data structures can be compiled and tested without
pretending that a database connection exists.
