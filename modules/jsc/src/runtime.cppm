// modules/jsc/src/runtime.cppm — module mbun.jsc.runtime
//
// mcpp 0.0.56 does not place non-exported module partitions in the provider
// graph, while exporting partitions that include JSC's TU-local header graph
// is rejected by GCC without non-portable permissive flags. Keep one compiled
// module unit and split its implementation into ordered, responsibility-based
// include fragments. Every physical source remains below the 2000-line limit.
module;
#include "runtime/prelude.hpp"

export module mbun.jsc.runtime;

import std;
import mbun.semver;
import mbun.toml;
import mbun.ini;
import mbun.glob;
import mbun.which;
import mbun.shell;
import mbun.dotenv;
import mbun.core.compress;
import mbun.compress;
import mbun.crypto;
import mbun.image;
import mbun.image.jpeg;
import mbun.core.io;
import mbun.core.strings;
import mbun.js_parser;
// T-CAP-BUILD: in-memory bundler engine backing Bun.build (runtime/bun_build.inc).
import mbun.bundler.vertical_slice;
import mbun.bundler.defines;
// Target / target_is_bun + the JSC-facing `target` string decode: `target: "bun"`
// drives the ASCII-only output pass.
import mbun.bundler.options;
// Bun.build({ compile }) — the single-file executable container (bun_build.inc).
import mbun.bundler.standalone_exe;
import mbun.bundler.bundler_jsc;
import mbun.resolver;
import mbun.jsc.module_loader;
import mbun.jsc.js_streams;
import mbun.jsc.js_builtins;
import mbun.jsc.js_net;
import mbun.jsc.js_bun_socket;
import mbun.jsc.js_tls_live;
import mbun.jsc.js_http2;
import mbun.jsc.js_websocket;
import mbun.jsc.js_dns;
import mbun.dns;
import mbun.ffi;
import mbun.sqlite;
import mbun.watcher;
import mbun.postgres;
// Bun.redis RESP codec (runtime/valkey_client.inc __mbunValkeyNative).
import mbun.valkey;
import mbun.sourcemap_jsc.internal_source_map;
// T-LOOP native epoll event loop for Bun.serve (runtime/serve_native.inc).
import mbun.event_loop;
import mbun.runtime_socket;
import mbun.runtime_server;
import mbun.http;
// issue #16: chained fault-signal handler (installed after JSC init below).
import mbun.crash_handler;
// CAP-S3: AWS SigV4 signing for Bun.S3Client (runtime/s3_native.inc __mbunS3Native).
import mbun.s3_signing;
import mbun.s3_signing.backend;
// T-TLS.3: memory-BIO TLS channels over reactor fds (runtime/net.inc tls*).
import mbun.tls;
// CAP-HTMLREWRITER: dependency-free HTMLRewriter engine (runtime/html_rewriter.inc).
import mbun.html_rewriter;

// CAP-NAPI: the Node-API layer (runtime/napi_core.inc + napi_objects.inc) is
// included from prelude.hpp in the GLOBAL MODULE FRAGMENT — its `extern "C"
// napi_*` ABI must have plain external linkage for dlopen'd .node addons, and
// JSC's pointer-tagging templates only instantiate cleanly there (see the
// note in prelude.hpp). The mbun_napi_* runtime hooks engine.inc calls are
// declared in runtime/napi/mbun_napi.h.

namespace {

#include "runtime/common.inc"
#include "runtime/jsc_internal.hpp"
#include "runtime/core_bindings.inc"
#include "runtime/webcrypto.inc"
// node:crypto native backend (createHash/createHmac/pbkdf2/random* → mbun.crypto).
#include "runtime/node_crypto.inc"
#include "runtime/bun_password.inc"
// node:crypto asymmetric + cipher backend over vendored OpenSSL (mbun.openssl).
#include "runtime/crypto_asym.inc"
// node:tls native backend (getCiphers) over vendored OpenSSL/libssl (mbun.openssl).
#include "runtime/node_tls.inc"
#include "runtime/sourcemap.inc"
#include "runtime/io_bindings.inc"
// node:zlib streaming Transform handles (mbun.compress.stream): incremental
// deflate/inflate/brotli/zstd state machines behind __mbunZlibNative.stream*.
#include "runtime/zlib_stream.inc"
// Shell bridge owns marker compilation and bounded template-array flattening.
#include "runtime/shell.inc"
#include "runtime/process_base.inc"
#include "runtime/process_extended.inc"
// Bun.$ execution bridge: runs compiled shell scripts through mbun's own
// interpreter (modules/shell) with output capture; needs b64_encode (process_base)
// and the shell_value_to_string helper (shell.inc).
#include "runtime/bunsh.inc"
#include "runtime/net.inc"
// T-LOOP: Bun.serve over the native epoll stack (__mbunServeNative bridge).
#include "runtime/serve_native.inc"
#include "runtime/node_net.inc"
#include "runtime/dns.inc"
// sqlite3-backed bun:sqlite native bridge (Database/Statement live in JS layer).
#include "runtime/sqlite.inc"
// CAP-WATCH: node:fs.watch inotify bridge (FSWatcher lives in the JS layer).
#include "runtime/watch.inc"
// bun:ffi native backend (Bun.FFI): dlopen + self-implemented SysV call path.
#include "runtime/ffi.inc"
// Bun.sql postgres wire codec bridge (frontend encoders + backend decoder).
#include "runtime/sql.inc"
// node:vm sandbox contexts (__mbunNodeVMNative): real JSC child global contexts.
#include "runtime/node_vm.inc"
// node:os / node:tty system-info bridge (uname/sysinfo/getpwuid/getifaddrs/…).
#include "runtime/node_os.inc"
#include "runtime/node_util.inc"
// Bun.build in-memory bundler bridge → mbun.bundler.build_bundle (vertical slice).
#include "runtime/bun_build.inc"
// CAP-S3: AWS SigV4 request signing bridge (__mbunS3Native.sign → mbun.s3_signing).
#include "runtime/s3_native.inc"
// CAP-HTMLREWRITER: Bun's HTMLRewriter over mbun.html_rewriter
// (__mbunHTMLRewriterNative.transform).
#include "runtime/html_rewriter.inc"
// Bun.redis RESP wire codec bridge (__mbunValkeyNative; offline codec only).
#include "runtime/valkey_client.inc"
// CAP-WORKER: real cross-thread Worker (second JSC VM per OS thread). Defines
// the __mbunWorkerNative seam engine.inc install_bindings_ registers; the parent
// event-loop pump drains it via globalThis.__mbunWorkerDrain.
#include "runtime/worker.inc"
// Engine owns binding installation and the child/microtask-aware event-loop pump.
// The CommonJS require/module-environment JS prelude lives in its own slice
// (engine.inc's 2000-line budget, enforced by test_runtime_structure).
#include "runtime/engine_require_js.inc"
#include "runtime/engine.inc"

}  // namespace

#include "runtime/api_impl.inc"
