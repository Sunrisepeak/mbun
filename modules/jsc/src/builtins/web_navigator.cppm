// Web `navigator` global partition.
//
// bun exposes a lazily-built `navigator` object carrying three attributes that
// runtime-detection code keys off of. hono's getRuntimeKey()
// (src/helper/adapter/index.ts) reads navigator.userAgent and matches the
// "Bun" prefix; with navigator absent it falls through to
// process.release.name === "node" and misreports the runtime as node.
//
// Observable contract, pinned against real bun 1.3.14 (not the C++ shape, the
// *JS-visible* shape):
//   - globalThis.navigator: data property { writable, enumerable, configurable }
//   - navigator.userAgent           = "Bun/" + version   (data prop, w/e/c)
//   - navigator.platform            = compile-time OS string
//   - navigator.hardwareConcurrency = online CPU count
//   - navigator[Symbol.toStringTag] = "Navigator" (non-writable, non-enumerable)
//   - Object.keys(navigator) === ["userAgent", "platform", "hardwareConcurrency"]
//   - there is NO global `Navigator` constructor
// Note the attributes read back as plain data properties even though bun
// installs them with putDirectNativeIntrinsicGetter, so plain data properties
// are the faithful rendering here.
//
// hardwareConcurrency uses __mbunOsNative.nprocs() (a single
// sysconf(_SC_NPROCESSORS_ONLN), runtime/node_os.inc:98) rather than parsing
// /proc/cpuinfo, so installing this partition costs one syscall at startup.
// bun wires the same two together in the other direction: os.availableParallelism()
// returns navigator.hardwareConcurrency (src/js/node/os.ts:93).
//
// NOTE: appended AFTER the master builtins IIFE; self-contained IIFE that
// re-binds G = globalThis. Top level must never throw.
//
// Blueprint: bun src/jsc/bindings/ZigGlobalObject.cpp:2267 (m_navigatorObject
// initLater), :1657 functionNavigatorGetUserAgent, :1663
// functionNavigatorGetPlatform, :1685 functionNavigatorGetHardwareConcurrency,
// and src/bun_core/Global.rs:774 (user_agent = concatcp!("Bun/", version)).
export module mbun.jsc.js_builtins:web_navigator;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kWebNavigatorJS = R"JS(
(function () {
  const G = globalThis;
  try {
    if (typeof G.navigator !== "undefined") return;
    const ON = G.__mbunOsNative;

    // bun_core/Global.rs:774 — user_agent = concatcp!("Bun/", package_json_version)
    const version = (G.Bun && G.Bun.version) || "1.3.14";

    // ZigGlobalObject.cpp:1663 — a fixed string per compile-time OS.
    // https://developer.mozilla.org/en-US/docs/Web/API/Navigator/platform
    // (bun returns "Linux x86_64" for every linux build, arch included.)
    const platformOf = function (osName, arch) {
      switch (osName) {
        case "darwin": return "MacIntel";
        case "win32": return "Win32";
        case "linux": return "Linux x86_64";
        case "freebsd": return arch === "arm64" ? "FreeBSD arm64" : "FreeBSD amd64";
        default: return "";
      }
    };

    const proc = G.process || {};
    const nav = {};
    // Plain assignment yields { writable: true, enumerable: true,
    // configurable: true } in bun's own property order.
    // navigator.* are getter-only accessors (get:function, no setter,
    // enumerable+configurable) — a strict-mode assignment must throw. ref bun
    // Navigator bindings; plain data properties render the descriptor wrong.
    const __ua = "Bun/" + version;
    const __plat = platformOf(proc.platform || "linux", proc.arch);
    const __hc = ON && typeof ON.nprocs === "function" ? ON.nprocs() : 1;
    Object.defineProperty(nav, "userAgent", { get: function () { return __ua; }, enumerable: true, configurable: true });
    Object.defineProperty(nav, "platform", { get: function () { return __plat; }, enumerable: true, configurable: true });
    Object.defineProperty(nav, "hardwareConcurrency", { get: function () { return __hc; }, enumerable: true, configurable: true });

    Object.defineProperty(nav, Symbol.toStringTag, {
      value: "Navigator", writable: false, enumerable: false, configurable: true,
    });

    Object.defineProperty(G, "navigator", {
      value: nav, writable: true, enumerable: true, configurable: true,
    });
  } catch (e) {}
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
