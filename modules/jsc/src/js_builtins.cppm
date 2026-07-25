// modules/jsc/src/js_builtins.cppm — module mbun.jsc.js_builtins
//
// Public aggregator for the one JS IIFE installed once by mbun.jsc.runtime.
// Payload partitions are joined byte-for-byte in source order; they are never
// evaluated independently and no separator is inserted between them.
export module mbun.jsc.js_builtins;

import std;
export import :bootstrap;
export import :process_web;
export import :async_hooks;
export import :yaml_flow;
export import :yaml_block_markdown;
export import :markdown_web;
export import :webcrypto;
export import :shell;
export import :ffi;
export import :image_closure;
export import :sql;
export import :bunsh;
export import :node_stream_core;
export import :node_stream_readable;
export import :node_stream_writable;
export import :node_stream_pipeline;
export import :node_stream_webadapters;
export import :zlib_stream;
export import :crypto_asym;
export import :node_os;
export import :node_vm;
export import :node_vm_modules;
export import :node_tls;
export import :node_worker;
export import :node_readline;
export import :node_v8;
export import :node_perf;
export import :node_strdec;
export import :node_module;
export import :node_repl;
export import :node_http;
export import :node_diag;
export import :node_net;
export import :node_fs_watch;
export import :node_fs_streams;
export import :bun_password;
export import :node_process_extra;
export import :node_process_lifecycle;
export import :node_util_extra;
export import :node_test_runner;
export import :node_cluster;
export import :node_legacy_ctors;
export import :node_domain;
export import :node_timers;
export import :node_buffer_extra;
export import :node_assert_deepequal;
export import :web_headers;
export import :web_urlpattern;
export import :web_navigator;
export import :web_events;
export import :valkey_client;
export import :s3;
export import :html_rewriter;
export import :node_internal_binding;
export import :fn_tostring_printer;

namespace mbun::jsc::builtins {

export inline const std::string kNodeBuiltinsJS =
    std::string {detail::kBootstrapJS}
        .append(detail::kProcessWebJS)
        .append(detail::kAsyncHooksJS)
        .append(detail::kYamlFlowJS)
        .append(detail::kYamlBlockMarkdownJS)
        .append(detail::kMarkdownWebJS)
        .append(detail::kWebCryptoJS)
        .append(detail::kShellJS)
        .append(detail::kFFIJS)
        .append(detail::kImageClosureJS)
        .append(detail::kSqlJS)
        .append(detail::kBunShJS)
        // node:util/types and node:string_decoder must precede the node:stream
        // partitions: internal/streams/legacy destructures isArrayBufferView /
        // isUint8Array out of node:util/types at module top level (bun-ref
        // src/js/internal/streams/legacy.ts:2) and internal/streams/readable
        // requires node:string_decoder at top level (readable.ts:25). Both only
        // read M["util"] / write their own entry at load, so hoisting is safe.
        .append(detail::kNodeUtilExtraJS)
        .append(detail::kNodeStrDecJS)
        // node:stream (the 1:1 port) must precede crypto_asym/zlib_stream:
        // both capture `M["stream"].Transform` at load time and subclass it,
        // so they have to see the real Transform, not the bootstrap stub.
        .append(detail::kNodeStreamCoreJS)
        .append(detail::kNodeStreamReadableJS)
        .append(detail::kNodeStreamWritableJS)
        .append(detail::kNodeStreamPipelineJS)
        .append(detail::kNodeStreamWebAdaptersJS)
        .append(detail::kCryptoAsymJS)
        .append(detail::kZlibStreamJS)
        .append(detail::kNodeOsJS)
        .append(detail::kNodeVmJS)
        // vm.Module & friends — needs the vm namespace above already registered.
        .append(detail::kNodeVmModulesJS)
        .append(detail::kNodeTlsJS)
        .append(detail::kNodeWorkerJS)
        .append(detail::kNodeReadlineJS)
        .append(detail::kNodeV8JS)
        .append(detail::kNodePerfJS)
        .append(detail::kNodeModuleJS)
        .append(detail::kNodeHttpJS)
        .append(detail::kNodeDiagJS)
        .append(detail::kNodeNetJS)
        .append(detail::kNodeFsWatchJS)
        // fs.ReadStream/WriteStream: needs node:stream (Readable/Writable) and
        // the fs module both already registered.
        .append(detail::kNodeFsStreamsJS)
        .append(detail::kBunPasswordJS)
        .append(detail::kNodeProcessExtraJS)
        // after node_process_extra: the uncaught-exception path consults the
        // capture-callback registry installed there.
        .append(detail::kNodeProcessLifecycleJS)
        // node:test standalone runner (used when no bun:test harness is present);
        // after bootstrap registered the delegating M["test"] it wraps.
        // node:cluster — after node_process_extra (which wires the child-side
        // IPC channel a worker's _setupWorker() sends 'online' over) and after
        // node:child_process/node:net are registered.
        .append(detail::kNodeClusterJS)
        .append(detail::kNodeTestRunnerJS)
        .append(detail::kNodeTimersJS)
        .append(detail::kNodeBufferExtraJS)
        .append(detail::kNodeAssertDeepEqualJS)
        .append(detail::kWebHeadersJS)
        .append(detail::kWebURLPatternJS)
        .append(detail::kWebNavigatorJS)
        // after node_process_extra: MessageEvent/CloseEvent/ErrorEvent
        // subclass the Event installed there.
        .append(detail::kWebEventsJS)
        .append(detail::kValkeyClientJS)
        // CAP-S3: Bun.S3Client / Bun.s3 / S3File over __mbunS3Native.sign + node:http.
        .append(detail::kS3JS)
        // CAP-HTMLREWRITER: new HTMLRewriter().on(...).transform(...) over the
        // vendored lol-html engine (__mbunHTMLRewriterNative).
        .append(detail::kHTMLRewriterJS)
        // node:domain — needs node:events (EventEmitter) already registered.
        .append(detail::kNodeDomainJS)
        // node:repl — last of the node modules: REPLServer extends readline's
        // Interface and evaluates through node:vm, so both must be installed.
        .append(detail::kNodeReplJS)
        // internalBinding(): the accessor node's lib/internal/** modules are
        // evaluated with. Handed to those modules as a wrapper parameter by the
        // loader, never installed as an ambient global.
        .append(detail::kNodeInternalBindingJS)
        // last: printer-normalizing Function.prototype.toString override —
        // every earlier partition must capture the native toString.
        .append(detail::kFnToStringPrinterJS);

}  // namespace mbun::jsc::builtins
