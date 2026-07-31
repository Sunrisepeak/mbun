// Bun.Terminal — a pseudo-terminal as a first-class object, plus the
// `Bun.spawn({terminal})` path that attaches a child to one.
//
// This partition is split out of :process_web for one mechanical reason: that
// partition's JS payload is a `constexpr std::string_view`, and GCC refuses a
// constant-evaluated strlen past 262144 characters. process_web sat ~2 KB under
// that ceiling before this feature existed, so the Terminal half moved here.
// The payloads are concatenated into ONE script (js_builtins.cppm
// kNodeBuiltinsJS), but NOT into one scope: :process_web's payload ends with the
// `if (globalThis.Bun) {` block still OPEN (a later partition closes it), so
// this text lands INSIDE that block. `spawnTerminal` is declared in the same
// block and therefore sees `Terminal`/`TREC` directly, while `__mbun_io_tick`
// -- declared at the enclosing IIFE level -- cannot. The reactor is handed to it
// through `G.__mbunTerminalReactor` for exactly that reason; do not "simplify"
// that back into a lexical reference.
//
// PORT-SOURCE: compat/bun/src/runtime/api/bun/Terminal.rs (the pty lifecycle,
// cooked-mode defaults and termios accessors) and
// compat/bun/src/runtime/api/Terminal.classes.ts (the exact prototype shape).
// The syscalls themselves live in modules/jsc/src/runtime/process_extended.inc
// (openPty / ptyTermios / ptySetTermios / ptySetRaw / ptyResize); this layer is
// argument validation, the write queue and callback dispatch, nothing more.
export module mbun.jsc.js_builtins:bun_terminal;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kBunTerminalJS = R"JS(
  // Live Bun.Terminal pseudo-terminals. A Terminal is NOT a child: the pty
  // exists on its own, before any process is attached and after it exits, so it
  // gets its own registry rather than a CHILDREN record with a fake pid (reap()
  // would waitpid() on it). __mbun_io_tick polls both in the same pass.
  const TERMINALS = (G.__mbunTerminals = G.__mbunTerminals || new Set());
  // Terminal instance -> its private record (fds, write queue, callbacks). The
  // record is what the reactor drives; the JS object is only the interface.
  const TREC = (G.__mbunTerminalRecs = G.__mbunTerminalRecs || new WeakMap());
  // A pty master never reports EOF while this process still holds the slave fd
  // open (that is why the Terminal keeps it): with no writer on the other side
  // the read is simply EAGAIN. So unlike drainOut there is no `ended` state to
  // reach — the loop stops on "nothing ready" and the fd closes only in
  // Terminal.close().
  const drainTerminal = (t) => {
    if (t.closed || t.master < 0) return;
    for (;;) {
      let b;
      try { b = PROC.readNB(t.master, 65536); } catch (e) { return; }
      if (b === null || b === "") return;
      if (typeof t.onData !== "function") continue;   // still drain, just discard
      try { t.onData(t.obj, _unb64(b)); } catch (e) { reportTerminalError(e); }
    }
  };

  // The `drain` callback is bun's "the writer has caught up" signal: it fires
  // once the queue empties after having held anything at all, which is exactly
  // what both drain tests observe (one write, and a second write that flushes
  // what the first buffered).
  const flushTerminal = (t) => {
    if (t.closed || t.writeFd < 0) return;
    while (t.wq.length) {
      const item = t.wq[0];
      let w;
      try { w = PROC.writeNB(t.writeFd, _b64(item.data.subarray(item.off)), 0); }
      catch (e) { t.wq.shift(); continue; }
      if (w < 0) { t.wq.shift(); continue; }   // pty gone; drop the rest
      if (w === 0) return;                     // EAGAIN — retry next tick
      item.off += w;
      if (item.off >= item.data.length) t.wq.shift();
      else return;
    }
    if (t.hadBuffered) {
      t.hadBuffered = false;
      if (typeof t.onDrain === "function") { try { t.onDrain(t.obj); } catch (e) { reportTerminalError(e); } }
    }
  };

  // A throw out of a terminal callback follows the same route as one out of a
  // child stream callback: process 'uncaughtException' if anyone is listening,
  // otherwise let it escape the tick.
  const reportTerminalError = (e) => {
    const pr = G.process;
    if (pr && typeof pr.listenerCount === "function" && pr.listenerCount("uncaughtException") > 0) pr.emit("uncaughtException", e);
    else throw e;
  };
  // ── Bun.Terminal ────────────────────────────────────────────────────────
  // A pseudo-terminal as a first-class object, independent of any child: the
  // pty is created by the constructor, pumped by __mbun_io_tick through
  // TERMINALS, and torn down by close(). `Bun.spawn({terminal})` then attaches
  // a child to an EXISTING pty (bun js_bun_spawn_bindings.rs:823 accepts either
  // a Terminal instance or an options object and creates one from the latter),
  // which is why spawnTerminal below routes both forms through this class
  // rather than keeping a second, parallel pty implementation.
  //
  // Everything that is really terminal semantics — pty creation, cooked-mode
  // defaults, termios access, raw mode — is a syscall in
  // runtime/process_extended.inc. This layer is argument validation, the write
  // queue, and callback dispatch.
  // PORT-SOURCE: compat/bun/src/runtime/api/bun/Terminal.rs,
  //              compat/bun/src/runtime/api/Terminal.classes.ts (the proto).
  const TCFLAG_MAX = 0xFFFFFFFF;
  // bun stores flags as tcflag_t and assigns `@max(0, @min(num, max))`, so a
  // non-finite input saturates: Infinity AND NaN both land on tcflag_t::MAX.
  const clampFlag = (v) => {
    const n = Number(v);
    if (Number.isNaN(n)) return TCFLAG_MAX;
    return Math.max(0, Math.min(n, TCFLAG_MAX)) >>> 0;
  };
  const termClosedError = () => new Error("Terminal is closed");
  // bun runs the constructor's callbacks through withAsyncContextIfNeeded, so a
  // Terminal created inside AsyncLocalStorage.run() still sees that store when
  // the pty later calls back (issue 26286). The timer hook pair is the same
  // capture/restore mbun already uses for setTimeout callbacks; it returns
  // `undefined` when there is nothing to capture, and then nothing is wrapped.
  const bindAsync = (fn, type) => {
    if (typeof fn !== "function") return null;
    const H = G.__mbunAsyncHookTimer;
    if (!H) return fn;
    let token;
    try { token = H.init(fn, type); } catch (e) { return fn; }
    if (token === undefined) return fn;
    return function () { return H.run(token, fn, this, Array.prototype.slice.call(arguments)); };
  };
  const validateDim = (v, what) => {
    if (typeof v !== "number" || !isFinite(v) || v <= 0 || v >= 65535) {
      const e = new TypeError('The "' + what + '" argument must be a positive integer, got ' + String(v));
      e.code = "ERR_INVALID_ARG_TYPE";
      throw e;
    }
    return v | 0;
  };
  class Terminal {
    constructor(options) {
      if (options === null || typeof options !== "object") {
        throw new TypeError("Terminal constructor requires an options object");
      }
      if (options.name !== undefined && typeof options.name !== "string") {
        const e = new TypeError('The "name" property must be of type string, got ' + typeof options.name);
        e.code = "ERR_INVALID_ARG_TYPE";
        throw e;
      }
      // An out-of-range or non-numeric size is IGNORED, not rejected: the
      // constructor tests pass cols:-1 / rows:0 / cols:"invalid" and expect a
      // working terminal at the defaults. resize() is the strict one.
      const cols = typeof options.cols === "number" && isFinite(options.cols) && options.cols > 0 && options.cols < 65535 ? options.cols | 0 : 80;
      const rows = typeof options.rows === "number" && isFinite(options.rows) && options.rows > 0 && options.rows < 65535 ? options.rows | 0 : 24;
      const h = PN.openPty(cols, rows);
      PN.setNonBlock(h.master);
      PN.setNonBlock(h.write);
      const t = {
        obj: this, master: h.master, writeFd: h.write, slave: h.slave,
        cols, rows, name: typeof options.name === "string" && options.name ? options.name : "xterm-256color",
        wq: [], hadBuffered: false, closed: false, unrefd: false, raw: false, savedFlags: null, exitFired: false,
        onData: bindAsync(options.data, "Terminal"),
        onDrain: bindAsync(options.drain, "Terminal"),
        onExit: bindAsync(options.exit, "Terminal"),
      };
      TREC.set(this, t);
      TERMINALS.add(t);
    }
    get closed() { const t = TREC.get(this); return !t || t.closed; }
    write(data) {
      const t = TREC.get(this);
      if (!t || t.closed) throw termClosedError();
      if (typeof data !== "string" && !ArrayBuffer.isView(data) && !(data instanceof ArrayBuffer)) {
        const e = new TypeError('The "data" argument must be of type string or an instance of ArrayBuffer or TypedArray, got ' + (data === null ? "null" : typeof data));
        e.code = "ERR_INVALID_ARG_TYPE";
        throw e;
      }
      const u = anyToU8(data);
      if (u.length === 0) return 0;
      t.wq.push({ data: u, off: 0 });
      t.hadBuffered = true;
      return u.length;
    }
    resize(cols, rows) {
      const t = TREC.get(this);
      if (!t || t.closed) throw termClosedError();
      const c = validateDim(cols, "cols"), r = validateDim(rows, "rows");
      PN.ptyResize(t.master, c, r);
      t.cols = c; t.rows = r;
    }
    setRawMode(enable) {
      const t = TREC.get(this);
      if (!t || t.closed) throw termClosedError();
      const on = !!enable;
      // Save this terminal's OWN pre-raw termios so a restore is exact and
      // independent of every other terminal (the "each terminal keeps its own
      // raw mode" regression).
      if (on && !t.raw) t.savedFlags = PN.ptyTermios(t.master);
      PN.ptySetRaw(t.master, on);
      if (!on && t.savedFlags) {
        PN.ptySetTermios(t.master, 0, t.savedFlags.iflag);
        PN.ptySetTermios(t.master, 1, t.savedFlags.oflag);
        PN.ptySetTermios(t.master, 2, t.savedFlags.cflag);
        PN.ptySetTermios(t.master, 3, t.savedFlags.lflag);
        t.savedFlags = null;
      }
      t.raw = on;
    }
    ref() { const t = TREC.get(this); if (t) t.unrefd = false; }
    unref() { const t = TREC.get(this); if (t) t.unrefd = true; }
    close() { __terminalClose(TREC.get(this), 0, null); }
    [Symbol.dispose]() { __terminalClose(TREC.get(this), 0, null); }
    [Symbol.asyncDispose]() { __terminalClose(TREC.get(this), 0, null); return Promise.resolve(); }
    get inputFlags() { return __termFlag(TREC.get(this), "iflag"); }
    set inputFlags(v) { __termSetFlag(TREC.get(this), 0, v); }
    get outputFlags() { return __termFlag(TREC.get(this), "oflag"); }
    set outputFlags(v) { __termSetFlag(TREC.get(this), 1, v); }
    get controlFlags() { return __termFlag(TREC.get(this), "cflag"); }
    set controlFlags(v) { __termSetFlag(TREC.get(this), 2, v); }
    get localFlags() { return __termFlag(TREC.get(this), "lflag"); }
    set localFlags(v) { __termSetFlag(TREC.get(this), 3, v); }
  }
  // A closed terminal reads 0 for every flag and swallows every write — the
  // tests assert both, and it keeps a stale handle from touching a recycled fd.
  const __termFlag = (t, field) => {
    if (!t || t.closed) return 0;
    const r = PN.ptyTermios(t.master);
    return r ? r[field] >>> 0 : 0;
  };
  const __termSetFlag = (t, which, v) => {
    if (!t || t.closed) return;
    PN.ptySetTermios(t.master, which, clampFlag(v));
  };
  const __terminalClose = (t, code, signal) => {
    if (!t || t.closed) return;
    drainTerminal(t);          // deliver whatever the pty still holds
    t.closed = true;
    TERMINALS.delete(t);
    for (const fd of [t.writeFd, t.slave, t.master]) { if (fd >= 0) { try { PROC.close(fd); } catch (e) {} } }
    t.master = -1; t.writeFd = -1; t.slave = -1; t.wq.length = 0;
    __terminalFireExit(t, code, signal);
  };
  // `exit` is a one-shot: it fires either when a spawn-attached child ends (the
  // pty reader's EOF, which is where bun raises it) or at close(), whichever
  // comes first -- never twice. "the exit callback must stay at one" is an
  // explicit assertion in terminal.test.ts.
  const __terminalFireExit = (t, code, signal) => {
    if (!t || t.exitFired) return;
    t.exitFired = true;
    if (t.onExit) { try { t.onExit(t.obj, code, signal); } catch (e) { reportTerminalError(e); } }
  };
  // Handed to :process_web's __mbun_io_tick, which is outside this scope.
  G.__mbunTerminalReactor = { set: TERMINALS, drain: drainTerminal, flush: flushTerminal };
  if (G.Bun) G.Bun.Terminal = Terminal;
  // Bun.spawn({terminal}) — run the child under a pseudo-terminal. `terminal`
  // is either a live Terminal or an options object one is built from; either
  // way the child's stdio 0/1/2 is that terminal's slave and its output
  // reaches JS through the terminal's own reader. The terminal OUTLIVES the
  // child in both forms ("Terminal should still be accessible after process
  // exit" asserts `closed === false` straight after `await proc.exited`); a
  // spawn-created one only stops PINNING the loop, since the child that
  // justified the pin is gone and nothing else would ever close it.
  // Ref: bun-ref subprocess.rs get_terminal + js_bun_spawn_bindings.rs:800.
  const spawnTerminal = (cmd, opts) => {
    const borrowed = opts.terminal instanceof Terminal;
    const term = borrowed ? opts.terminal : new Terminal(opts.terminal && typeof opts.terminal === "object" ? opts.terminal : {});
    const t = TREC.get(term);
    if (!t || t.closed) throw new TypeError("terminal is closed");
    const h = PN.spawnPty(cmd[0], cmd, { cwd: opts.cwd ? toStr(opts.cwd) : undefined, env: opts.env && typeof opts.env === "object" ? opts.env : (G.process && G.process.env) || undefined, slave: t.slave });
    if (h.errno != null) {
      if (!borrowed) __terminalClose(t, 0, null);  // nothing observed it yet
      const code = ERRNO[h.errno] || ("errno " + h.errno);
      const e = new Error("spawn " + cmd[0] + " " + code); e.code = code; e.errno = -1; e.syscall = "spawn " + cmd[0]; throw e;
    }
    let exitResolve; const exitedP = new Promise((r) => (exitResolve = r));
    const proc = { pid: h.pid, exitCode: null, signalCode: null, killed: false, exited: exitedP, ref() {}, unref() {}, resourceUsage() { return __mbunResourceUsage(); } };
    // outs is empty and stdinFd is -1: this record exists only to reap the
    // child. All of the pty's I/O rides the Terminal's own registry entry.
    const rec = { cp: null, pid: h.pid, outs: [], writers: [], stdinFd: -1, stdinBuf: [], stdinEnded: false, stdinClosed: true, exited: false, closed: false, done: false, code: null, signal: null };
    rec.cp = { emit: (ev, code, signal) => {
      if (ev === "exit") { proc.exitCode = signal ? null : code; proc.signalCode = signal || null; }
      else if (ev === "close") {
        proc.exitCode = signal ? null : code; proc.signalCode = signal || null;
        drainTerminal(t);                           // keep the pty; the caller closes it
        // A spawn-created terminal's `exit` fires HERE, not at close(): bun
        // fires it when the pty reader sees EOF (the child was the only slave
        // writer), and the test asserts it has already run by the time
        // `await proc.exited` resolves -- while `closed` is still false.
        if (!borrowed) { t.unrefd = true; __terminalFireExit(t, proc.exitCode, proc.signalCode); }
        exitResolve(proc.exitCode);
      }
    }, stdin: null };
    proc.terminal = term;
    proc.stdin = null; proc.stdout = null; proc.stderr = null;
    proc.kill = function (sig) { const s = bunMapSig(sig); PN.kill(h.pid, s); this.killed = true; return true; };
    proc[Symbol.dispose] = function () { try { this.kill(); } catch (e) {} };
    proc[Symbol.asyncDispose] = function () { try { this.kill(); } catch (e) {} return exitedP; };
    CHILDREN.add(rec);
    return proc;
  };
)JS";

}  // namespace mbun::jsc::builtins::detail
