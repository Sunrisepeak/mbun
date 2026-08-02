// process.permission and node's initializePermission bootstrap.
//
// Translated from:
//   compat/node/lib/internal/process/permission.js       has/drop/availableFlags
//   compat/node/lib/internal/process/pre_execution.js:655 initializePermission
//   compat/node/lib/fs.js                                the fd-API refusals and
//                                                        the symlink message
//
// SCOPE OF THIS FILE — read before adding anything to it.
//
// This partition is the *reporting* surface, not the enforcement surface.
// Enforcement lives in C++ (runtime/io_bindings.inc, the spawn bridges, the
// worker bridge, the module loader's read hook), because those are the only
// places the filesystem and fork() are actually reached, and a check that a
// script can delete or monkey-patch is not a sandbox. Everything here either
//
//   (a) asks the C++ model a question (`has`, `drop`), or
//   (b) mirrors a refusal node ITSELF performs in lib/fs.js — the fd-metadata
//       APIs that are switched off wholesale under the model, and the symlink
//       "requires full fs.read and fs.write" case.
//
// (b) is parity, not defence: each of those calls ALSO passes through the C++
// gate, so removing the JS half changes the message, never the outcome.
export module mbun.jsc.js_builtins:node_permission;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kNodePermissionJS = R"JS(
(function () {
  const G = globalThis;
  const PN = G.__mbunPermissionNative;
  if (!PN) return;

  const proc = G.process;
  if (!proc) return;

  // node internal/validators validateString / validateBuffer, reproduced so the
  // ERR_INVALID_ARG_TYPE messages match byte-for-byte (test-permission-has and
  // test-permission-drop-errors assert the text, not just the code).
  const typeName = (v) => {
    if (v === null) return "null";
    if (Array.isArray(v)) return "an instance of Array";
    if (typeof v === "object") {
      const n = v.constructor && v.constructor.name;
      return n ? "an instance of " + n : "an instance of Object";
    }
    if (typeof v === "string") return "type string";
    return "type " + typeof v;
  };
  const argTypeErr = (name, expected, actual) => {
    let received;
    if (actual === null) received = "null";
    else if (typeof actual === "object") received = typeName(actual);
    else if (typeof actual === "string") received = "type string";
    else received = "type " + typeof actual;
    const e = new TypeError(
      'The "' + name + '" argument must be of ' + expected + ". Received " + received);
    e.code = "ERR_INVALID_ARG_TYPE";
    return e;
  };
  const validateString = (v, name) => {
    if (typeof v !== "string") throw argTypeErr(name, "type string", v);
  };
  const isBuffer = (v) => {
    try { return G.Buffer && G.Buffer.isBuffer(v); } catch (e) { return false; }
  };
  const validateReference = (reference) => {
    if (reference === undefined || reference === null) return;
    // node: a Buffer is accepted (validateBuffer); anything else must be a string.
    if (isBuffer(reference)) return;
    if (typeof reference !== "string") throw argTypeErr("reference", "type string", reference);
  };

  // node src/permission/permission.cc publishes to a diagnostics channel on
  // every DENIED answer, and on every drop. The channel names are node's.
  //
  // KNOWN GAP, stated plainly: only the denials that pass through
  // process.permission.has()/drop() publish. A denial raised inside the C++ fs
  // gate does not, because that gate must not call back into JS mid-syscall.
  // The gate still denies — this affects observability, not enforcement.
  const CHANNEL = {
    FileSystem: "node:permission-model:fs",
    FileSystemRead: "node:permission-model:fs",
    FileSystemWrite: "node:permission-model:fs",
    ChildProcess: "node:permission-model:child",
    WorkerThreads: "node:permission-model:worker",
    Net: "node:permission-model:net",
    Inspector: "node:permission-model:inspector",
    WASI: "node:permission-model:wasi",
    Addon: "node:permission-model:addon",
    FFI: "node:permission-model:ffi",
  };
  const SCOPE_NAME = {
    fs: "FileSystem", "fs.read": "FileSystemRead", "fs.write": "FileSystemWrite",
    child: "ChildProcess", worker: "WorkerThreads", wasi: "WASI",
    inspector: "Inspector", net: "Net", addon: "Addon", ffi: "FFI",
  };
  let publishing = false;
  const publish = (scope, resource, drop) => {
    const name = SCOPE_NAME[scope];
    const channelName = name && CHANNEL[name];
    if (!channelName || publishing) return;
    let dc;
    try { dc = G.require ? G.require("diagnostics_channel") : null; } catch (e) { return; }
    if (!dc || typeof dc.channel !== "function") return;
    let ch;
    try { ch = dc.channel(channelName); } catch (e) { return; }
    if (!ch || !ch.hasSubscribers) return;
    publishing = true;
    try {
      const msg = { __proto__: null, permission: name, resource: resource === undefined ? "" : String(resource) };
      if (drop) msg.drop = true;
      ch.publish(msg);
    } catch (e) {
    } finally { publishing = false; }
  };

  const has = function has(scope, reference) {
    validateString(scope, "scope");
    validateReference(reference);
    const granted = PN.has(scope, reference === undefined || reference === null
      ? undefined : String(reference));
    if (!granted) publish(scope, reference, false);
    return granted;
  };

  const drop = function drop(scope, reference) {
    validateString(scope, "scope");
    validateReference(reference);
    PN.drop(scope, reference === undefined || reference === null ? undefined : String(reference));
    publish(scope, reference, true);
  };

  // node lib/internal/process/permission.js availableFlags(). --allow-ffi only
  // appears with --experimental-ffi, as node has it.
  const availableFlags = () => {
    const flags = ["--allow-fs-read", "--allow-fs-write", "--allow-addons",
                   "--allow-child-process", "--allow-net", "--allow-inspector",
                   "--allow-wasi", "--allow-worker"];
    return flags;
  };

  // ── node pre_execution.js initializePermission ────────────────────────────
  // The model being OFF is the overwhelmingly common case and must cost nothing:
  // no wrappers installed, no process.permission, no behaviour change at all.
  //
  // node's "--allow-* without --permission throws ERR_MISSING_OPTION" check is
  // NOT done here but in C++ (engine.inc, right after the options are applied),
  // because a throw escaping this IIFE would abort the rest of the builtins image
  // and take the runtime down with it.
  if (!PN.enabled) return;

  // process.binding() is a documented escape hatch straight into the C++
  // bindings, so the model closes it outright.
  proc.binding = function binding(_module) {
    // node: `new ERR_ACCESS_DENIED('process.binding')` — the message IS the
    // API name, and `permission`/`resource` stay empty.
    throw PN.denyError("", "", "process.binding");
  };

  // "Guarantee path module isn't monkey-patched to bypass permission model."
  // (Every fs path is resolved through the same normaliser the model uses, so a
  // patched path.resolve would otherwise be able to shift what a grant covers.)
  try {
    const path = G.require && G.require("path");
    if (path) Object.freeze(path);
  } catch (e) {}

  // The startup warnings. They are already on stderr (printed from C++ where the
  // flags are parsed), because emitWarning at THIS point is lost: this IIFE runs
  // inside the builtins image, whose nextTick queue drains before the entry
  // script exists, so no listener can have been attached yet.
  //
  // They are still delivered to a listener, because node does and the corpus
  // checks it (common.expectWarning): held here and replayed the moment someone
  // subscribes to 'warning'. The interception removes itself on the first flush.
  const pending = [];
  const warnFlags = [
    ["--allow-addons", PN.allowAddons],
    ["--allow-child-process", PN.allowChildProcess],
    ["--allow-inspector", PN.allowInspector],
    ["--allow-wasi", PN.allowWasi],
    ["--allow-worker", PN.allowWorker],
  ];
  for (const [flag, on] of warnFlags) {
    if (on) {
      pending.push(["SecurityWarning",
        "The flag " + flag + " must be used with extreme caution. " +
        "It could invalidate the permission model."]);
    }
  }
  if (PN.allowFfi) {
    pending.push(["SecurityWarning",
      "The flag --allow-ffi must be used with extreme caution. " +
      "It could invalidate the permission model."]);
  }

  // The comma-separated form stopped being a path list in node 20; a single
  // value containing a comma is almost always someone still using it.
  for (const [flag, values] of [["--allow-fs-read", PN.allowFsRead],
                                ["--allow-fs-write", PN.allowFsWrite]]) {
    if (values && values.length === 1 && String(values[0]).includes(",")) {
      pending.push(["Warning",
        "The " + flag + " CLI flag has changed. " +
        "Passing a comma-separated list of paths is no longer valid. " +
        "Documentation can be found at " +
        "https://nodejs.org/api/permissions.html#file-system-permissions"]);
    }
  }

  if (PN.allowNet) {
    pending.push(["ExperimentalWarning", "The flag --allow-net is under experimental phase."]);
  }

  if (pending.length) {
    const originals = {};
    const names = ["on", "addListener", "once", "prependListener", "prependOnceListener"];
    for (const n of names) originals[n] = proc[n];
    const restore = () => { for (const n of names) if (typeof originals[n] === "function") proc[n] = originals[n]; };
    const flush = () => {
      if (!pending.length) return;
      const list = pending.splice(0, pending.length);
      restore();
      for (const [type, message] of list) {
        try { proc.emitWarning(message, type); } catch (e) {}
      }
    };
    for (const n of names) {
      const orig = originals[n];
      if (typeof orig !== "function") continue;
      proc[n] = function (event, listener) {
        const r = orig.call(this, event, listener);
        if (String(event) === "warning") flush();
        return r;
      };
    }
  }

  Object.defineProperty(proc, "permission", {
    __proto__: null,
    enumerable: true,
    configurable: false,
    value: { has, drop },
  });

  // ── node lib/fs.js's own JS-level refusals ────────────────────────────────
  // These are the fd-addressed metadata syscalls: the kernel lets them change an
  // inode through a READ-ONLY descriptor, so node switches them off wholesale
  // while the model is on rather than trying to map an fd back to a path. The
  // list and the message are node's (lib/fs.js).
  const fsMod = (G.__mbunNativeModules || {})["fs"] || (G.__mbunNativeModules || {})["node:fs"];
  // node's fs wrappers convert a valid path-like value before it reaches
  // node_file.cc's permission check. Keep this adapter deliberately narrow:
  // invalid values still go to the original function and its full validator.
  const permissionPath = (value) => {
    if (typeof value === "string") return value;
    if (isBuffer(value) || ArrayBuffer.isView(value)) {
      try { return G.Buffer.from(value.buffer, value.byteOffset, value.byteLength).toString("utf8"); }
      catch (e) { return undefined; }
    }
    if (value && typeof value === "object" && value.protocol === "file:") {
      try {
        const url = G.require && G.require("url");
        return url && typeof url.fileURLToPath === "function" ? url.fileURLToPath(value) : undefined;
      } catch (e) { return undefined; }
    }
    return undefined;
  };
  const deniedFsError = (scope, value, suffix = "") => {
    const path = permissionPath(value);
    if (path === undefined) return undefined;
    const resource = path + suffix;
    return PN.has(scope, resource) ? undefined : PN.denyError(scope, resource);
  };
  // EXACTLY node's list (lib/fs.js + internal/fs/promises.js): fsync, fdatasync,
  // fchmod, fchown, futimes. Notably NOT read/write/close — those take an fd that
  // could only have come from a gated open(), so gating them again would break
  // every legitimate caller (node's own test harness reads its file this way)
  // without adding any protection.
  const disabledUnderModel = ["fsync", "fdatasync", "fchmod", "fchown", "futimes"];
  if (fsMod) {
    // node node_file.cc keeps these checks outside the filesystem operation's
    // async branch. mbun's load-bearing C++ gates already deny access, but the
    // JS adapters used to mask access's denial as ENOENT and defer utimes /
    // lutimes until a microtask. These thin wrappers restore node's public
    // timing/error contract; direct calls to __mbunFsNative remain gated too.
    const origAccessSync = fsMod.accessSync;
    if (typeof origAccessSync === "function") {
      fsMod.accessSync = function (path, ...rest) {
        const err = deniedFsError("fs.read", path);
        if (err) throw err;
        return origAccessSync.call(this, path, ...rest);
      };
    }
    const origAccess = fsMod.access;
    if (typeof origAccess === "function") {
      fsMod.access = function (path, mode, callback) {
        const cb = typeof mode === "function" ? mode : callback;
        const err = typeof cb === "function" ? deniedFsError("fs.read", path) : undefined;
        if (err) { G.queueMicrotask(() => cb(err)); return; }
        return origAccess.apply(this, arguments);
      };
    }
    for (const name of ["utimes", "lutimes", "mkdir", "chmod"]) {
      const original = fsMod[name];
      if (typeof original === "function") {
        fsMod[name] = function (path, ...rest) {
          const err = deniedFsError("fs.write", path);
          if (err) throw err;
          return original.call(this, path, ...rest);
        };
      }
    }
    // chown/lchown are no-op compatibility stubs on this runtime, so there is
    // no syscall capability to protect. They still report node's sync/callback
    // refusal contracts while the model is enabled.
    for (const name of ["chownSync", "lchownSync"]) {
      const original = fsMod[name];
      if (typeof original === "function") {
        fsMod[name] = function (path, ...rest) {
          const err = deniedFsError("fs.write", path);
          if (err) throw err;
          return original.call(this, path, ...rest);
        };
      }
    }
    for (const name of ["chown", "lchown"]) {
      const original = fsMod[name];
      if (typeof original === "function") {
        fsMod[name] = function (path, uid, gid, callback) {
          const err = typeof callback === "function" ? deniedFsError("fs.write", path) : undefined;
          if (err) { G.queueMicrotask(() => callback(err)); return; }
          return original.apply(this, arguments);
        };
      }
    }
    const FileHandle = fsMod.promises && fsMod.promises.FileHandle;
    if (FileHandle && FileHandle.prototype && typeof FileHandle.prototype.chown === "function") {
      const original = FileHandle.prototype.chown;
      FileHandle.prototype.chown = function (...args) {
        // Run the original first for its fd/uid/gid validation. It has no
        // fchown syscall behind it, so success is replaced with node's model-on
        // refusal without exposing an operation between validation and denial.
        return Promise.resolve(original.apply(this, args)).then(() => {
          throw PN.denyError("", "", "fchown API is disabled when Permission Model is enabled.");
        });
      };
    }

    for (const base of disabledUnderModel) {
      for (const name of [base, base + "Sync"]) {
        const original = fsMod[name];
        if (typeof original !== "function") continue;
        const message = base + " API is disabled when Permission Model is enabled.";
        if (name.endsWith("Sync")) {
          fsMod[name] = function () { throw PN.denyError("", "", message); };
        } else {
          fsMod[name] = function () {
            const cb = arguments[arguments.length - 1];
            const err = PN.denyError("", "", message);
            if (typeof cb === "function") { cb(err); return; }
            throw err;
          };
        }
      }
    }

    // fs.symlink: the target is interpreted relative to the LINK's directory,
    // which the model cannot resolve without following it — so node requires
    // full read AND write and refuses otherwise. Message is node's.
    const symlinkMessage = "fs.symlink API requires full fs.read and fs.write permissions.";
    const symlinkAllowed = () => PN.has("fs");
    const origSymlinkSync = fsMod.symlinkSync;
    if (typeof origSymlinkSync === "function") {
      fsMod.symlinkSync = function (...a) {
        if (!symlinkAllowed()) throw PN.denyError("", "", symlinkMessage);
        return origSymlinkSync.apply(this, a);
      };
    }
    const origSymlink = fsMod.symlink;
    if (typeof origSymlink === "function") {
      fsMod.symlink = function (...a) {
        if (!symlinkAllowed()) {
          const cb = a[a.length - 1];
          const err = PN.denyError("", "", symlinkMessage);
          if (typeof cb === "function") { cb(err); return; }
          throw err;
        }
        return origSymlink.apply(this, a);
      };
    }
    if (fsMod.promises && typeof fsMod.promises.symlink === "function") {
      const origAsync = fsMod.promises.symlink;
      fsMod.promises.symlink = function (...a) {
        if (!symlinkAllowed()) return Promise.reject(PN.denyError("", "", symlinkMessage));
        return origAsync.apply(this, a);
      };
    }

    // fs.watch / fs.watchFile take a path and hand back its contents over time,
    // so node gates them on fs.read like any other read.
    for (const name of ["watch", "watchFile"]) {
      const original = fsMod[name];
      if (typeof original !== "function") continue;
      fsMod[name] = function (p, ...rest) {
        const ref = typeof p === "string" ? p
          : (p && typeof p === "object" && p.href ? p : undefined);
        const asPath = typeof ref === "string" ? ref
          : (ref ? (G.require ? G.require("url").fileURLToPath(ref) : String(ref)) : String(p));
        if (!PN.has("fs.read", asPath)) {
          throw PN.denyError("fs.read", asPath);
        }
        return original.call(this, p, ...rest);
      };
    }
  }

  // process.report.writeReport and v8.writeHeapSnapshot both write a file whose
  // name defaults to the cwd; node gates them on fs.write with that resolved
  // name as the resource.
  try {
    if (proc.report && typeof proc.report.writeReport === "function") {
      const orig = proc.report.writeReport;
      proc.report.writeReport = function (filename, err) {
        const resource = filename === undefined ? proc.cwd() : filename;
        if (!PN.has("fs.write", resource)) throw PN.denyError("fs.write", resource);
        return orig.call(this, filename, err);
      };
    }
  } catch (e) {}
  try {
    const v8mod = (G.__mbunNativeModules || {})["v8"] || (G.__mbunNativeModules || {})["node:v8"];
    if (v8mod && typeof v8mod.writeHeapSnapshot === "function") {
      const orig = v8mod.writeHeapSnapshot;
      v8mod.writeHeapSnapshot = function (filename, options) {
        const resource = filename === undefined ? proc.cwd() : String(filename);
        if (!PN.has("fs.write", resource)) throw PN.denyError("fs.write", resource);
        return orig.call(this, filename, options);
      };
    }
  } catch (e) {}
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
