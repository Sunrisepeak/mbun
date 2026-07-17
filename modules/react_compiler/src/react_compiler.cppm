// react_compiler.cppm — public seam aggregator.
// ref: Bun's react_compiler Rust/Zig sources are expected under
// .mbun/bun-ref/src/ and .mbun/bun-zig-src/src/; this checkout has neither.
// The complete compiler and JSC/React runtime integration are DEFERRED.
export module mbun.react_compiler;

export import mbun.react_compiler.parser;
export import mbun.react_compiler.hir;
export import mbun.react_compiler.lowering;
export import mbun.react_compiler.validation;
