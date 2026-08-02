// node:os + node:tty JS layer partition.
//
// Shapes globalThis.__mbunOsNative (POSIX libc info, see runtime/node_os.inc)
// into the node:os and node:tty module objects: the Symbol.toPrimitive quirk on
// os getters, os.tmpdir's env logic, os.constants, the getPriority/setPriority
// SystemError contract, and node:tty's isatty/ReadStream/WriteStream with the
// full getColorDepth env matrix + hasColors/getWindowSize.
//
// NOTE: appended AFTER the master builtins IIFE (opened in bootstrap, closed by
// image_closure), so this is a self-contained IIFE that re-binds G = globalThis
// and overwrites the pure-JS os/tty stubs registered in bootstrap. It must not
// rely on the outer IIFE's aliases.
//
// Blueprint: bun src/js/node/os.ts, src/js/node/tty.ts, src/js/internal/tty.ts.
export module mbun.jsc.js_builtins:node_os;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kNodeOsJS = R"JS(
(function () {
  const G = globalThis;
  const M = G.__mbunNativeModules;
  const ON = G.__mbunOsNative;
  if (!M) return;
  const proc = G.process || {};
  const envOf = () => (G.process && G.process.env) || {};

  // ---------------------------------------------------------------- node:os
  if (ON && typeof ON.uname === "function") {
    const U = ON.uname();
    const END = ON.endianness();
    // Regular function expressions (not arrows): node exposes these as plain
    // functions, so os.<fn>.toString() must start with "function".
    const platform = function () { return proc.platform || "linux"; };
    const arch = function () { return proc.arch || "x64"; };
    const typeName = function () {
      const p = platform();
      if (p === "win32") return "Windows_NT";
      if (p === "darwin") return "Darwin";
      if (p === "linux" || p === "android") return "Linux";
      return U.sysname || "Linux";
    };

    function tmpdir() {
      const e = envOf();
      let p = e.TMPDIR || e.TMP || e.TEMP || "/tmp";
      const n = p.length;
      if (n > 1 && p[n - 1] === "/") p = p.slice(0, -1);
      return p;
    }
    tmpdir[Symbol.toPrimitive] = tmpdir;

    // errno (positive) → node/libuv SystemError shape.
    const ERRNO = {
      1: ["EPERM", "operation not permitted"],
      2: ["ENOENT", "no such file or directory"],
      3: ["ESRCH", "no such process"],
      13: ["EACCES", "permission denied"],
      22: ["EINVAL", "invalid argument"],
    };
    const sysErr = (syscall, en) => {
      const m = ERRNO[en] || ["UNKNOWN", "unknown error"];
      const uv = -en;
      const e = new Error(
        "A system error occurred: " + syscall + " returned " + m[0] + " (" + m[1] + ")",
      );
      e.name = "SystemError";
      e.code = "ERR_SYSTEM_ERROR";
      e.errno = uv;
      e.syscall = syscall;
      e.info = { errno: uv, code: m[0], message: m[1], syscall: syscall };
      return e;
    };

    // node validateInt32 shape: ERR_INVALID_ARG_TYPE for non-numbers,
    // ERR_OUT_OF_RANGE for non-integers or out-of-[min,max]. Messages match
    // node's (only the leading clause is asserted by the corpus tests).
    const osArgType = (name, actual) => {
      let recv;
      if (actual === null) recv = "null";
      else if (typeof actual === "object") recv = "an instance of " + ((actual && actual.constructor && actual.constructor.name) || "Object");
      else if (typeof actual === "string") recv = "type string ('" + actual + "')";
      else recv = "type " + typeof actual + " (" + String(actual) + ")";
      const e = new TypeError('The "' + name + '" argument must be of type number. Received ' + recv);
      e.code = "ERR_INVALID_ARG_TYPE";
      return e;
    };
    const osOutOfRange = (name, actual) => {
      const e = new RangeError('The value of "' + name + '" is out of range. It must be an integer. Received ' + String(actual));
      e.code = "ERR_OUT_OF_RANGE";
      return e;
    };
    const validateInt32 = (value, name, min, max) => {
      if (typeof value !== "number") throw osArgType(name, value);
      if (!Number.isInteger(value)) throw osOutOfRange(name, value);
      if (min === undefined) min = -2147483648;
      if (max === undefined) max = 2147483647;
      if (value < min || value > max) throw osOutOfRange(name, value);
      return value;
    };

    const prevOs = M["os"] || M["node:os"] || {};
    const constants = prevOs.constants || {};
    // Node exposes this nested constants table as immutable. Keep the outer
    // constants object extensible: only `signals` carries that contract.
    if (constants.signals && !Object.isFrozen(constants.signals)) Object.freeze(constants.signals);
    const internalOs = () => {
      try {
        return typeof G.__mbunInternalBinding === "function" ? G.__mbunInternalBinding("os") : null;
      } catch (e) { return null; }
    };
    const homeError = (ctx) => {
      const e = new Error("A system error occurred: " + ctx.syscall + " returned " + ctx.code + " (" + ctx.message + ")");
      e.name = "SystemError";
      e.code = "ERR_SYSTEM_ERROR";
      e.syscall = ctx.syscall;
      return e;
    };

    const os = {
      arch: arch,
      availableParallelism: function () { return ON.nprocs(); },
      cpus: function () { return ON.cpus(); },
      endianness: function () { return END; },
      freemem: function () { return ON.freemem(); },
      getPriority: function (pid) {
        if (pid === undefined) pid = 0; else validateInt32(pid, "pid");
        const r = ON.getPriority(pid);
        if (!r.ok) throw sysErr("uv_os_getpriority", r.errno | 0);
        return r.value;
      },
      homedir: function () {
        const binding = internalOs();
        if (!binding || typeof binding.getHomeDirectory !== "function") return ON.homedir();
        const ctx = {};
        const value = binding.getHomeDirectory(ctx);
        if (value === undefined && ctx.syscall !== undefined) throw homeError(ctx);
        return value;
      },
      hostname: function () { return ON.hostname(); },
      loadavg: function () { return ON.loadavg(); },
      machine: function () { return U.machine || "x86_64"; },
      networkInterfaces: function () { return ON.networkInterfaces(); },
      platform: platform,
      release: function () { return U.release || ""; },
      setPriority: function (pid, prio) {
        if (prio === undefined) { prio = pid; pid = 0; }
        validateInt32(pid, "pid");
        const pr = constants.priority || {};
        validateInt32(prio, "priority", pr.PRIORITY_HIGHEST, pr.PRIORITY_LOW);
        const r = ON.setPriority(pid, prio);
        if (!r.ok) throw sysErr("uv_os_setpriority", r.errno | 0);
        return undefined;
      },
      tmpdir: tmpdir,
      totalmem: function () { return ON.totalmem(); },
      type: typeName,
      uptime: function () { return ON.uptime(); },
      userInfo: function (opts) {
        // Read the option before crossing the native boundary. Besides matching
        // node's order, this lets a throwing `encoding` getter escape unchanged.
        const encoding = opts == null ? undefined : opts.encoding;
        const i = ON.userInfo();
        if (encoding === "buffer" && G.Buffer) {
          return {
            username: G.Buffer.from(i.username),
            uid: i.uid,
            gid: i.gid,
            shell: i.shell ? G.Buffer.from(i.shell) : null,
            homedir: G.Buffer.from(i.homedir),
          };
        }
        return { username: i.username, uid: i.uid, gid: i.gid, shell: i.shell, homedir: i.homedir };
      },
      version: function () { return U.version || ""; },
      getuid: function () { return proc.getuid ? proc.getuid() : ON.userInfo().uid; },
      constants: constants,
      devNull: platform() === "win32" ? "\\\\.\\nul" : "/dev/null",
      EOL: platform() === "win32" ? "\r\n" : "\n",
    };

    // os.EOL is a non-writable (but configurable) data property: assigning to
    // it throws TypeError in strict mode, while Object.defineProperty can still
    // redefine it (node lib/os.js does exactly this).
    Object.defineProperty(os, "EOL", {
      value: os.EOL, writable: false, enumerable: true, configurable: true,
    });

    // node implements Symbol.toPrimitive (not toString) on these getters so
    // `os.hostname + ""` coerces to the value instead of the function source.
    for (const key of ["arch", "availableParallelism", "endianness", "freemem", "homedir",
                       "hostname", "platform", "release", "tmpdir", "totalmem", "type",
                       "uptime", "version", "machine"]) {
      const fn = os[key];
      if (typeof fn === "function") fn[Symbol.toPrimitive] = function () { return os[key](); };
    }

    M["os"] = M["node:os"] = os;
  }

  // --------------------------------------------------------------- node:tty
  const COLORS_2 = 1, COLORS_16 = 4, COLORS_256 = 8, COLORS_16m = 24;
  const TERM_ENVS = {
    "eterm": COLORS_16, "cons25": COLORS_16, "console": COLORS_16, "cygwin": COLORS_16,
    "dtterm": COLORS_16, "gnome": COLORS_16, "hurd": COLORS_16, "jfbterm": COLORS_16,
    "konsole": COLORS_16, "kterm": COLORS_16, "mlterm": COLORS_16, "mosh": COLORS_16m,
    "putty": COLORS_16, "st": COLORS_16, "rxvt-unicode-24bit": COLORS_16m,
    "terminator": COLORS_16m, "xterm-kitty": COLORS_16m,
  };
  const CI_ENVS = {
    APPVEYOR: COLORS_256, BUILDKITE: COLORS_256, CIRCLECI: COLORS_16m, DRONE: COLORS_256,
    GITEA_ACTIONS: COLORS_16m, GITHUB_ACTIONS: COLORS_16m, GITLAB_CI: COLORS_256, TRAVIS: COLORS_256,
  };
  const TERM_ENVS_REG_EXP = [/ansi/, /color/, /linux/, /direct/, /^con[0-9]*x[0-9]/, /^rxvt/,
                             /^screen/, /^xterm/, /^vt100/, /^vt220/];

  function getColorDepth(env) {
    env = env || envOf();
    const FORCE_COLOR = env.FORCE_COLOR;
    if (FORCE_COLOR !== undefined) {
      switch (FORCE_COLOR) {
        case "": case "1": case "true": return COLORS_16;
        case "2": return COLORS_256;
        case "3": return COLORS_16m;
        default: return COLORS_2;
      }
    }
    if (
      (env.NODE_DISABLE_COLORS !== undefined && env.NODE_DISABLE_COLORS !== "") ||
      (env.NO_COLOR !== undefined && env.NO_COLOR !== "") ||
      env.TERM === "dumb"
    ) {
      return COLORS_2;
    }
    if ((proc.platform || "linux") === "win32") {
      const rel = (M["os"] && M["os"].release ? String(M["os"].release()) : "0").split(".");
      if (+rel[0] >= 10) {
        const build = +rel[2];
        if (build >= 14931) return COLORS_16m;
        if (build >= 10586) return COLORS_256;
      }
      return COLORS_16;
    }
    if (env.TMUX) return COLORS_16m;
    if ("TF_BUILD" in env && "AGENT_NAME" in env) return COLORS_16;
    if ("CI" in env) {
      for (const name in CI_ENVS) if (name in env) return CI_ENVS[name];
      if (env.CI_NAME === "codeship") return COLORS_256;
      return COLORS_2;
    }
    if ("TEAMCITY_VERSION" in env) {
      return /^(9\.(0*[1-9]\d*)\.|\d{2,}\.)/.test(env.TEAMCITY_VERSION) ? COLORS_16 : COLORS_2;
    }
    switch (env.TERM_PROGRAM) {
      case "iTerm.app":
        if (!env.TERM_PROGRAM_VERSION || /^[0-2]\./.test(env.TERM_PROGRAM_VERSION)) return COLORS_256;
        return COLORS_16m;
      case "HyperTerm": case "ghostty": case "WezTerm": case "MacTerm": return COLORS_16m;
      case "Apple_Terminal": return COLORS_256;
    }
    const COLORTERM = env.COLORTERM;
    if (COLORTERM === "truecolor" || COLORTERM === "24bit") return COLORS_16m;
    const TERM = env.TERM;
    if (TERM) {
      if (/truecolor/.test(TERM)) return COLORS_16m;
      if (TERM.startsWith("xterm-256")) return COLORS_256;
      const termEnv = TERM.toLowerCase();
      if (TERM_ENVS[termEnv]) return TERM_ENVS[termEnv];
      if (TERM_ENVS_REG_EXP.some((t) => t.test(termEnv))) return COLORS_16;
    }
    if (env.COLORTERM) return COLORS_16;
    return COLORS_2;
  }

  const isatty = (fd) => (ON && typeof ON.isatty === "function" ? !!ON.isatty(fd | 0) : false);
  const EventEmitter = M["events"] || M["node:events"];
  const BaseProto = EventEmitter ? EventEmitter.prototype : Object.prototype;
  const initEmitter = function (self) { if (EventEmitter) EventEmitter.call(self); };
  // node lib/tty.js: `ReadStream`/`WriteStream` ARE net.Sockets and their
  // constructors run `net.Socket.call(this, ...)`. node:tty is built before
  // node:net here, so js_net re-parents these prototypes after the fact (see
  // js_net.cppm "node lib/tty.js: ReadStream extends net.Socket"). That fixes
  // method lookup but not instance state: the inherited Duplex on()/emit()
  // then read stream state this constructor never created, so the first
  // `ttyStream.on("data", ...)` threw instead of registering a listener
  // (tty-reopen-after-stdin-eof: "TTY ReadStream should not set position for
  // character devices"). Initialise the base in place, WITHOUT `fd` — node
  // hands net.Socket a TTY handle rather than a descriptor to adopt, and
  // adopting it here would make `stream.destroy()` close a descriptor the
  // caller still owns and closes itself.
  const initSocketBase = function (self) {
    const netMod = M["net"] || M["node:net"];
    const S = netMod && netMod.Socket;
    if (typeof S === "function" && self instanceof S) {
      try { S.call(self, {}); return; } catch (e) {}
    }
    initEmitter(self);
  };

  function ReadStream(fd) {
    if (!(this instanceof ReadStream)) return new ReadStream(fd);
    initSocketBase(this);
    this.fd = fd | 0;
    this.readable = true;
    this.isRaw = false;
    this.isTTY = isatty(this.fd);
  }
  ReadStream.prototype = Object.create(BaseProto);
  Object.defineProperty(ReadStream.prototype, "constructor",
    { value: ReadStream, writable: true, configurable: true });
  ReadStream.prototype.setRawMode = function (flag) {
    flag = !!flag;
    if (ON && typeof ON.setRawMode === "function") {
      const err = ON.setRawMode(this.fd, flag);
      if (err) { this.emit("error", new Error("setRawMode failed with errno: " + err)); return this; }
    }
    this.isRaw = flag;
    return this;
  };
  ReadStream.prototype.ref = function () { return this; };
  ReadStream.prototype.unref = function () { return this; };
  ReadStream.prototype.setRawMode.displayName = "setRawMode";

  function WriteStream(fd) {
    if (!(this instanceof WriteStream)) return new WriteStream(fd);
    initSocketBase(this);
    this.fd = fd | 0;
    this.writable = true;
    this.columns = undefined;
    this.rows = undefined;
    this.isTTY = isatty(this.fd);
    if (this.isTTY && ON && typeof ON.ttySize === "function") {
      const s = ON.ttySize(this.fd);
      if (s) { this.columns = s[0]; this.rows = s[1]; }
    }
  }
  WriteStream.prototype = Object.create(BaseProto);
  Object.defineProperty(WriteStream.prototype, "constructor",
    { value: WriteStream, writable: true, configurable: true });
  WriteStream.prototype.getColorDepth = function (env) { return getColorDepth(env || envOf()); };
  WriteStream.prototype.hasColors = function (count, env) {
    if (env === undefined && (count === undefined || (typeof count === "object" && count !== null))) {
      env = count; count = 16;
    } else if (typeof count !== "number" || count < 2 || !Number.isInteger(count)) {
      const e = new RangeError('The value of "count" is out of range. It must be >= 2. Received ' + count);
      e.code = "ERR_OUT_OF_RANGE";
      throw e;
    }
    return count <= Math.pow(2, this.getColorDepth(env));
  };
  WriteStream.prototype.getWindowSize = function () { return [this.columns, this.rows]; };
  WriteStream.prototype._refreshSize = function () {
    if (!(ON && typeof ON.ttySize === "function")) return;
    const s = ON.ttySize(this.fd);
    if (s && (this.columns !== s[0] || this.rows !== s[1])) {
      this.columns = s[0]; this.rows = s[1]; this.emit("resize");
    }
  };
  const readlineCompat = () => M["readline"] || M["node:readline"];
  WriteStream.prototype.clearLine = function (dir, callback) {
    const rl = readlineCompat();
    return rl && typeof rl.clearLine === "function" ? rl.clearLine(this, dir, callback) : true;
  };
  WriteStream.prototype.clearScreenDown = function (callback) {
    const rl = readlineCompat();
    return rl && typeof rl.clearScreenDown === "function" ? rl.clearScreenDown(this, callback) : true;
  };
  WriteStream.prototype.cursorTo = function (x, y, callback) {
    const rl = readlineCompat();
    return rl && typeof rl.cursorTo === "function" ? rl.cursorTo(this, x, y, callback) : true;
  };
  WriteStream.prototype.moveCursor = function (dx, dy, callback) {
    const rl = readlineCompat();
    return rl && typeof rl.moveCursor === "function" ? rl.moveCursor(this, dx, dy, callback) : true;
  };
  // Write-only stream: async iteration completes immediately (node parity).
  WriteStream.prototype[Symbol.asyncIterator] = function () { return { next() { return Promise.resolve({ value: undefined, done: true }); }, [Symbol.asyncIterator]() { return this; } }; };

  const tty = { isatty: isatty, ReadStream: ReadStream, WriteStream: WriteStream };
  M["tty"] = M["node:tty"] = tty;

  // node's process.stdout/.stderr ARE tty.WriteStreams when the descriptor is a
  // terminal, so they carry columns/rows/getWindowSize. mbun's are native
  // objects that only ever gained write/isTTY/fd, so `process.stdout.columns`
  // read undefined under a pty -- which is what every "the child sees the
  // terminal it was given" assertion checks (Bun.Terminal({cols,rows}),
  // terminal.resize, SIGWINCH). Accessors, not the cached fields node refreshes
  // on SIGWINCH: a live TIOCGWINSZ is what makes "re-query the window size after
  // a resize" answer correctly without a signal handler in between.
  for (const name of ["stdout", "stderr"]) {
    const strm = G.process && G.process[name];
    if (!strm || !strm.isTTY || strm.columns !== undefined) continue;
    const size = () => (ON && typeof ON.ttySize === "function" ? ON.ttySize(strm.fd | 0) : null);
    try {
      Object.defineProperty(strm, "columns", { configurable: true, enumerable: true, get() { const s = size(); return s ? s[0] : undefined; } });
      Object.defineProperty(strm, "rows", { configurable: true, enumerable: true, get() { const s = size(); return s ? s[1] : undefined; } });
      if (typeof strm.getWindowSize !== "function") strm.getWindowSize = function () { const s = size(); return s ? [s[0], s[1]] : [undefined, undefined]; };
      // node re-reads the size here and emits 'resize' on a change. The accessors
      // above already re-read, so this only has to fire the event -- but it must
      // EXIST: code that wants a fresh size after a resize calls it (and node's
      // own SIGWINCH handler is what normally does).
      if (typeof strm._refreshSize !== "function") {
        let last = null;
        strm._refreshSize = function () {
          const s = size();
          if (!s) return;
          if (!last || last[0] !== s[0] || last[1] !== s[1]) { last = s; if (typeof strm.emit === "function") strm.emit("resize"); }
        };
      }
      if (typeof strm.getColorDepth !== "function") strm.getColorDepth = WriteStream.prototype.getColorDepth;
      if (typeof strm.hasColors !== "function") strm.hasColors = WriteStream.prototype.hasColors;
    } catch (e) {}
  }
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
