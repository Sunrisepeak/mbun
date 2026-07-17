# Bundler vertical-slice benchmark

This suite compares the same four-file in-memory static ESM graph and validates
the emitted chunks by evaluating each one and requiring semantic checksum `83`.
The entry also executes a function-local parameter named `require`; its missing
specifier must not become a graph edge or be rewritten by the bundler.

The Bun driver runs `Bun.build` from JS. The mbun driver invokes the native
`mbun.bundler` library because `Bun.build` is not yet exposed by the single
`mbun` executable. Therefore all current numbers are **level B, non-equivalent
boundary**, and cannot support a claim that mbun is faster than Bun. The graph,
parse→link→emit operation and semantic checksum are equal; only the JS boundary
is not.
