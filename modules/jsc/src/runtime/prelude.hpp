#pragma once

#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic ignored "-Wexpose-global-module-tu-local"
#endif

// Match the prebuilt JavaScriptCore's RELEASE configuration before including any
// WTF/JSC header: NDEBUG ⇒ ASSERT_ENABLED 0 (wtf/PlatformEnable.h). Without it,
// the C++ internals' inline code paths (GCOwnedDataScope ctor/dtor, JSString::
// view) reference debug-only symbols (setTopGCOwnedDataScopeIfNeeded /
// DoesGCCheck::verifyCanGC) that the release static lib does not export, so the
// link fails. This is the "与产物同 cmakeconfig.h 配置" requirement from
// docs/design/20260710-jsc-integration.md §4.
#ifndef NDEBUG
#  define NDEBUG 1
#endif

// cmakeconfig.h — the exact ENABLE()/USE() config the prebuilt JavaScriptCore was
// built with. WebKit's own build injects this on the command line (-include
// cmakeconfig.h); nothing under include/ pulls it in, so a TU that omits it silently
// falls back to PlatformEnable.h defaults and disagrees with the shipped library about
// struct layout. Concretely: without it ENABLE(WEBASSEMBLY) is off here but on in the
// library, so JSGlobalObject's WASM member block (JSGlobalObject.h:468) disappears and
// sizeof(JSC::JSGlobalObject) comes out 3456 instead of the library's 3720 -- every
// member past that point is then read at the wrong offset (globalObjectMethodTable()
// returned nullptr before this include was added). Must precede all WTF/JSC headers.
#include <cmakeconfig.h>

// macOS only, and a defect in the artifact's own config rather than a choice:
// cmakeconfig.h:83 sets ENABLE_MEDIA_SOURCE 0 for this JSC-only build but says
// nothing about COCOA_WEBM_PLAYER, so PlatformEnableCocoa.h:80 then turns that
// one ON by Cocoa default -- and PlatformEnable.h:1052 asserts the combination
// is impossible:
//
//     #if ENABLE(COCOA_WEBM_PLAYER) && !ENABLE(MEDIA_SOURCE)
//     #error "ENABLE(COCOA_WEBM_PLAYER) requires ENABLE(MEDIA_SOURCE)"
//
// Both of PlatformEnableCocoa.h's defines are `#if !defined(...)` guarded, so
// settling it here wins. Off, not on: this is a media-player feature, mbun uses
// none of it, and unlike the WEBASSEMBLY case above it gates no JSC struct
// member -- turning MEDIA_SOURCE on instead would diverge from the config the
// shipped library was compiled with, which is the exact hazard cmakeconfig.h is
// included to avoid.
#if defined(__APPLE__) && !defined(ENABLE_COCOA_WEBM_PLAYER)
#define ENABLE_COCOA_WEBM_PLAYER 0
#endif

// Global module fragment: the JSC C API header + JSC::initialize declaration
// (external C++ symbol from the prebuilt product), mirroring jsc.cppm so the
// header symbols stay in the global module with external linkage.
//
// Plus the JSC C++ internals used for zero-copy string-arg extraction:
//   APICast.h    — toJS(JSContextRef)→JSGlobalObject*, toJS(global,JSValueRef)→JSValue
//   JSCInlines.h — inline JSValue::toString / JSString::view definitions
//   JSString.h   — JSString::view(global) → GCOwnedDataScope<StringView>
//   StringView.h — is8Bit / span8 / span16 zero-copy access
// Header set verified to compile under both gcc16 and llvm22.1.8 at C++26
// (bun-webkit clang21+libstdc++ / mbun gcc16+libstdc++ same ABI family).
#include <JavaScriptCore/JavaScript.h>
#include <JavaScriptCore/APICast.h>
#include <JavaScriptCore/JSCInlines.h>
#include <JavaScriptCore/JSString.h>
#include <JavaScriptCore/StringObject.h>
#include <JavaScriptCore/JSArrayBuffer.h>
#include <JavaScriptCore/JSTypedArray.h>
#include <JavaScriptCore/JSFunction.h>
#include <JavaScriptCore/JSBigInt.h>
#include <JavaScriptCore/CallFrame.h>
#include <JavaScriptCore/ImplementationVisibility.h>
#include <JavaScriptCore/Error.h>
// JSNativeStdFunction.h — locked-entry host functions for set_fn (C-callback
// signature without JSC's DropAllLocks wrapper; see common.inc set_fn).
// JSLock.h — JSLockHolder re-lock guard for C API class-constructor callbacks,
// which JSC still invokes inside a JSLock::DropAllLocks window.
#include <JavaScriptCore/JSNativeStdFunction.h>
#include <JavaScriptCore/JSLock.h>
// InitializeThreading.h — initialize(callback): JSC freezes its option table at
// the end of initialize(), so options are only settable from inside that
// callback (bun does the same in ZigGlobalObject.cpp).
// VM.h — VM::drainMicrotasks(), needed to pump microtasks from inside a locked
// host call, where JSLock::didReleaseLock never fires.
#include <JavaScriptCore/InitializeThreading.h>
#include <JavaScriptCore/Options.h>
// GlobalObjectMethodTable.h — JSGlobalObject.h only forward-declares the table, but
// the runtime copies it by value to fill the deriveShadowRealmGlobalObject slot the C
// API leaves null (see engine.inc create_context_).
#include <JavaScriptCore/GlobalObjectMethodTable.h>
#include <JavaScriptCore/VM.h>
// MicrotaskQueueInlines.h — MicrotaskQueue::performMicrotaskCheckpoint, i.e.
// node's runMicrotasks(): exhaust the microtask queue WITHOUT the
// unhandled-rejection checkpoint VM::drainMicrotasks() appends to it
// (bindings_install.inc __mbunRunMicrotasksNative).
#include <JavaScriptCore/MicrotaskQueueInlines.h>
// JSDateMath.h (JSC::DateCache) + wtf/DateMath.h (WTF::setTimeZoneOverride):
// assigning process.env.TZ has to invalidate the per-VM timezone cache, which
// only these expose (runtime/process_base.inc proc_set_timezone_cb).
#include <JavaScriptCore/JSDateMath.h>
#include <wtf/DateMath.h>
#include <wtf/text/WTFString.h>
#include <wtf/text/StringView.h>

// CAP-NAPI (runtime/napi_core.inc + napi_objects.inc): the Node-API layer is
// implemented directly on JSC internals (blueprint: bun src/jsc/bindings/
// napi.cpp), so the cells/scopes/refs it ports need these headers. The
// vendored node ABI headers + the env/cell definitions ride along via
// napi/mbun_napi.h so `struct napi_env__` lives in the global module fragment.
#include <JavaScriptCore/ArgList.h>
#include <JavaScriptCore/MarkedVector.h>
#include <JavaScriptCore/ArrayConstructor.h>
#include <JavaScriptCore/CallData.h>
#include <JavaScriptCore/TopExceptionScope.h>
#include <JavaScriptCore/Completion.h>
#include <JavaScriptCore/ConstructData.h>
#include <JavaScriptCore/DateInstance.h>
#include <JavaScriptCore/ErrorInstance.h>
#include <JavaScriptCore/Exception.h>
#include <JavaScriptCore/GetterSetter.h>
#include <JavaScriptCore/InternalFunction.h>
#include <JavaScriptCore/JSDestructibleObject.h>
#include <JavaScriptCore/JSGlobalObjectInlines.h>
#include <JavaScriptCore/JSPromise.h>
#include <JavaScriptCore/ObjectConstructor.h>
#include <JavaScriptCore/PrivateName.h>
#include <JavaScriptCore/PropertyDescriptor.h>
#include <JavaScriptCore/SourceCode.h>
#include <JavaScriptCore/StrongInlines.h>
#include <JavaScriptCore/Symbol.h>
#include <JavaScriptCore/WeakHandleOwner.h>
#include <JavaScriptCore/WeakInlines.h>
// PORT-SOURCE: compat/bun/src/jsc/bindings/JSCTaskScheduler.cpp,
// compat/bun/src/jsc/bindings/BunClientData.cpp:110-118.
// JSC parks post-GC work -- JSFinalizationRegistry cleanup callbacks,
// Atomics.waitAsync resumptions, async WebAssembly compile completions -- on
// VM::deferredWorkTimer, a JSRunLoopTimer. mbun never runs WTF's RunLoop on
// the JS thread, so that timer NEVER fires and the parked work is dropped on
// the floor forever. bun does not run the RunLoop either; it replaces the
// timer's dispatch with onAddPendingWork/onScheduleWorkSoon hooks that queue
// into its own event loop. mbun's equivalent is to drain the timer's task
// queue from its own pump (see mbun_drain_deferred_work).
#include <JavaScriptCore/DeferredWorkTimer.h>
#if !defined(_WIN32)
#  include <dlfcn.h>  // dlopen/dlsym for .node addon loading
#endif
#include "napi/mbun_napi.h"
// The napi implementation compiles here in the GLOBAL MODULE FRAGMENT, not in
// runtime.cppm's purview, for two load-bearing reasons: (1) the `extern "C"
// napi_*` ABI must have plain external linkage so dlopen'd .node addons
// resolve it from the executable; (2) JSC's pointer-tagging templates
// (WTF::PtrTagTraits et al.) reference TU-local internals, which is a hard
// error when instantiated from module-purview code but only the (suppressed)
// -Wexpose-global-module-tu-local diagnostic here. prelude.hpp is included
// solely by runtime.cppm, so these definitions exist exactly once.
#include "napi_core.inc"
#include "napi_objects.inc"
#include "napi_binary.inc"

// ICU per-item zstd decompression hook. Strong extern "C" definition of
// bun_icu_maybe_decompress binds the weak-undef reference the repacked ICU's
// patched udata.cpp emits; must live in the GLOBAL MODULE FRAGMENT for plain
// external linkage (same reason as napi_*). Without it, compressed display-name
// data reaches ICU raw and Intl.DateTimeFormat/NumberFormat throw.
#include "icu_decompress.inc"

// POSIX subprocess primitives for node:child_process.spawnSync (real fork/exec)
// and the async pipe ops behind __mbunProcNative. <cerrno>/<csignal> provide the
// errno/SIG* macros (not exported by `import std`).
#include <cerrno>
#include <csignal>
// <cstdlib> for ::realpath / ::free (fs.realpathSync.native's strict resolver in
// io_bindings.inc). `import std` does not export the POSIX realpath overload.
#include <cstdlib>
#if !defined(_WIN32)
#  include <sys/wait.h>
#  include <sys/stat.h>  // stat() for Bun.which is_executable_file_path probe
#  include <sys/syscall.h>  // SYS_close_range on Linux/musl
#  include <sys/statvfs.h>  // statvfs() backing fs.statfs / fs.statfsSync
#  include <unistd.h>
// POSIX TCP sockets for __mbunNetNative (Bun.serve / real fetch / node:net).
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/un.h>  // sockaddr_un for Bun.listen({unix}) (runtime/net.inc listenUnix)
#  include <arpa/inet.h>
#  include <netdb.h>   // getaddrinfo/getnameinfo for __mbunDnsNative (node:dns/Bun.dns)
#  include <fcntl.h>
#  include <poll.h>
#  include <grp.h>   // setgroups for child_process uid/gid
#endif

// OpenSSL 3.1.5 (vendored mbun.openssl local-index static lib; NOT host /usr) for
// node:crypto's asymmetric + cipher backend (runtime/crypto_asym.inc). Included in
// the global module fragment so the C API symbols keep external linkage.
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/decoder.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/encoder.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/param_build.h>
#include <openssl/params.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/ssl.h>  // node:tls getCiphers (SSL_CTX default cipher list, runtime/node_tls.inc)

// POSIX OS-info system headers for node:os / node:tty (runtime/node_os.inc).
// All are OS syscalls / libc, not third-party host libraries.
#if !defined(_WIN32)
#  include <sys/utsname.h>   // uname() → sysname/nodename/release/version/machine
#  include <sys/resource.h>  // getpriority/setpriority (PRIO_PROCESS)
#  include <sys/ioctl.h>     // TIOCGWINSZ terminal window size
#  include <pwd.h>           // getpwuid() for os.userInfo/homedir
#  include <ifaddrs.h>       // getifaddrs() for os.networkInterfaces
#  include <net/if.h>        // IFF_LOOPBACK
#  include <termios.h>       // tcgetattr/tcsetattr for tty setRawMode
#  if defined(__linux__)
#    include <sys/sysinfo.h>       // sysinfo() totalram/freeram/uptime/loads
#    include <netpacket/packet.h>  // sockaddr_ll interface MAC address
#  elif defined(__APPLE__)
#    include <mach/mach.h>         // host_statistics64() vm page counts for os.freemem
#  endif
#endif
