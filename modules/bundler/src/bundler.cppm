// bundler.cppm — mbun.bundler: bun's bundler, pure-logic port.
//
// Aggregator that re-exports the bundler subsystem's modules. This is a
// mechanical-translation-底 port (移植三段法 ①②): the option/graph/chunk/output
// data model and the BundleV2 driver shape are ported 1:1 from
// .mbun/bun-ref/src/bundler, while every phase body that needs the real
// parser/resolver/thread-pool/linker/JSC bindings is DEFERRED(S-bundler).
// The heavy internals (LinkerContext, ParseTask, transpiler, ThreadPool, the
// linker_context/* codegen submodules) are the next porting waves.
export module mbun.bundler;

export import mbun.bundler.index;
export import mbun.bundler.source;
export import mbun.bundler.options;
export import mbun.bundler.graph;
export import mbun.bundler.entry_points;
export import mbun.bundler.chunk;
export import mbun.bundler.output_file;
export import mbun.bundler.bundle;
export import mbun.bundler.standalone_graph;
export import mbun.bundler.standalone_exe;
export import mbun.bundler.bundler_jsc;
export import mbun.bundler.ast_jsc;
export import mbun.bundler.ascii_only;
export import mbun.bundler.defines;
export import mbun.bundler.vertical_slice;
