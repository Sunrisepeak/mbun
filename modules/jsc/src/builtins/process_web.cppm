// Process and Web payload partition; keep raw bytes aligned with js_builtins.cppm lines 1573-2569.
export module mbun.jsc.js_builtins:process_web;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kProcessWebJS = R"JS(  // ---- child_process (real: spawnSync/execSync + async spawn/exec/execFile/fork)
  // Async children run over __mbunProcNative.spawnEx (fork/exec with live pipe
  // fds) and are driven by __mbun_io_tick(), which the event-loop pump calls each
  // turn to poll the fds, feed the stdout/stderr Readable streams, flush buffered
  // stdin non-blocking, reap the child, and emit 'exit'/'close'. IPC (fork channel)
  // is NOT implemented — fork() runs the module but send()/'message' are no-ops.
  const CP = globalThis.__mbunCpNative;
  const PROC = globalThis.__mbunProcNative;
  const normArgs = (a, o) => (Array.isArray(a) ? { args: a, opts: o || {} } : { args: [], opts: a || {} });
  const SIGMAP = { SIGHUP: 1, SIGINT: 2, SIGQUIT: 3, SIGILL: 4, SIGTRAP: 5, SIGABRT: 6, SIGIOT: 6, SIGBUS: 7, SIGFPE: 8, SIGKILL: 9, SIGUSR1: 10, SIGSEGV: 11, SIGUSR2: 12, SIGPIPE: 13, SIGALRM: 14, SIGTERM: 15, SIGCHLD: 17, SIGCONT: 18, SIGSTOP: 19, SIGTSTP: 20, SIGTTIN: 21, SIGTTOU: 22 };
  const SIGNAME = {}; for (const k in SIGMAP) if (!SIGNAME[SIGMAP[k]]) SIGNAME[SIGMAP[k]] = k;
  const ERRNO = { 1: "EPERM", 2: "ENOENT", 8: "ENOEXEC", 9: "EBADF", 11: "EAGAIN", 12: "ENOMEM", 13: "EACCES", 20: "ENOTDIR", 21: "EISDIR", 22: "EINVAL", 23: "ENFILE", 24: "EMFILE", 36: "ENAMETOOLONG" };
  // Node defers only these spawn errnos to the async 'error' event; the rest
  // (EPERM, EBADF, …) throw synchronously from spawn().
  const DELAYED = { ENOENT: 1, EACCES: 1, EAGAIN: 1, EMFILE: 1, ENFILE: 1 };
  const _u8 = (d) => (typeof d === "string" ? te.encode(d) : d instanceof Uint8Array ? d : ArrayBuffer.isView(d) ? new Uint8Array(d.buffer, d.byteOffset, d.byteLength) : d instanceof ArrayBuffer ? new Uint8Array(d) : te.encode(String(d)));
  const _b64 = (d) => { const u = _u8(d); let s = ""; for (let i = 0; i < u.length; i += 4096) s += String.fromCharCode.apply(null, u.subarray(i, i + 4096)); return G.btoa(s); };
  const _unb64 = (b) => { const s = G.atob(b); const u = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) u[i] = s.charCodeAt(i); return u; };
  const nextTick = (fn) => (G.process && G.process.nextTick ? G.process.nextTick(fn) : G.queueMicrotask(fn));

  // Active async children; __mbun_io_tick drives every entry each pump turn.
  const CHILDREN = (G.__mbunChildren = G.__mbunChildren || new Set());
  const decodeEnc = (bytes, enc) => Buffer.from(bytes).toString(enc === "utf-8" ? "utf8" : enc);

  // A child's stdout/stderr. `Readable` is lexically the bootstrap load-order
  // stub (this partition sits inside the master builtins IIFE); resolve the real
  // node:stream Readable at call time — spawn() only runs long after the
  // node_stream_* partitions have registered it — so a child's stdout really is
  // a Readable (node/bun contract; bun-ref test/regression/issue/08095).
  // push()/setEncoding()/destroyed/'end'/'close' then come from the real state
  // machine instead of being hand-emitted onto the stub.
  const makeReadable = () => {
    const S = M["stream"] || M["node:stream"];
    const RC = (S && S.Readable) || Readable;
    const r = new RC({ read() {} });
    r.bytesRead = 0;
    r.__data = (bytes) => {
      r.bytesRead += bytes.length;
      r.push(Buffer.from(bytes));  // setEncoding(), if set, decodes downstream
    };
    r.__end = () => { if (r._ended) return; r._ended = true; r.push(null); };
    return r;
  };

  const flushStdin = (rec) => {
    if (rec.stdinFd < 0 || rec.stdinClosed) return;
    while (rec.stdinBuf.length) {
      const item = rec.stdinBuf[0];
      if (item.data.length - item.off <= 0) { rec.stdinBuf.shift(); if (item.cb) try { item.cb(); } catch (e) {} continue; }
      const w = PROC.writeNB(rec.stdinFd, _b64(item.data.subarray(item.off)), 0);
      if (w < 0) { rec.stdinBuf.shift(); if (item.cb) try { item.cb(); } catch (e) {} continue; }  // broken pipe
      if (w === 0) return;  // EAGAIN — retry next tick
      item.off += w;
      if (item.off >= item.data.length) { rec.stdinBuf.shift(); if (item.cb) try { item.cb(); } catch (e) {} }
      else return;
    }
    if (rec.stdinEnded) { try { PROC.close(rec.stdinFd); } catch (e) {} rec.stdinClosed = true; if (rec.cp.stdin) rec.cp.stdin.destroyed = true; }
  };

  const drainOut = (o) => {
    for (;;) {
      const b = PROC.readNB(o.fd, 65536);
      if (b === null) { o.ended = true; try { PROC.close(o.fd); } catch (e) {} if (o.stream) o.stream.__end(); return; }
      if (b === "") return;  // nothing more ready now
      if (o.stream) o.stream.__data(_unb64(b));
    }
  };

  const maybeClose = (rec) => {
    if (!rec.exited || rec.closed) return;
    for (const o of rec.outs) if (!o.ended) return;  // wait for all pipes to hit EOF
    if (rec.stdinFd >= 0 && !rec.stdinClosed) { try { PROC.close(rec.stdinFd); } catch (e) {} rec.stdinClosed = true; if (rec.cp.stdin) rec.cp.stdin.destroyed = true; }
    rec.closed = true; rec.done = true;
    rec.cp.emit("close", rec.code, rec.signal);
  };

  const reap = (rec) => {
    if (!rec.exited) {
      const w = PROC.wait(rec.pid, true);
      if (w.exited) {
        rec.exited = true;
        rec.code = w.signal ? null : w.code;
        rec.signal = w.signal ? (SIGNAME[w.signal] || ("SIG" + w.signal)) : null;
        rec.cp.exitCode = rec.code; rec.cp.signalCode = rec.signal;
        rec.cp.emit("exit", rec.code, rec.signal);
      }
    }
    maybeClose(rec);
  };

  G.__mbun_io_tick = function () {
    if (!CHILDREN.size) return 0;
    const recs = [...CHILDREN];
    const readFds = [], readObjs = [];
    for (const rec of recs) for (const o of rec.outs) if (!o.ended && o.fd >= 0) { readFds.push(o.fd); readObjs.push(o); }
    if (readFds.length) {
      const ready = PROC.poll(readFds, 5);
      for (let i = 0; i < readObjs.length; i++) if (ready[i]) drainOut(readObjs[i]);
    } else {
      PROC.poll([], 2);  // brief real wait while waiting for a child to exit
    }
    for (const rec of recs) flushStdin(rec);
    for (const rec of recs) reap(rec);
    let active = 0;
    for (const rec of recs) { if (rec.done) CHILDREN.delete(rec); else active++; }
    return active;
  };

  const makeStdin = (fd, rec) => {
    const w = new Writable();
    w.writable = true; w.destroyed = false;
    w.write = (chunk, enc, cb) => {
      if (typeof enc === "function") { cb = enc; enc = null; }
      if (w.destroyed || rec.stdinEnded) { if (typeof cb === "function") nextTick(cb); return false; }
      const data = chunk == null ? new Uint8Array(0) : typeof chunk === "string" ? te.encode(chunk) : _u8(chunk);
      rec.stdinBuf.push({ data, off: 0, cb: typeof cb === "function" ? cb : null });
      flushStdin(rec);
      return true;
    };
    w.end = (chunk, enc, cb) => {
      if (typeof chunk === "function") { cb = chunk; chunk = null; }
      else if (typeof enc === "function") { cb = enc; enc = null; }
      if (chunk != null) w.write(chunk);
      rec.stdinEnded = true;
      if (typeof cb === "function") rec.stdinBuf.push({ data: new Uint8Array(0), off: 0, cb });
      flushStdin(rec);
      return w;
    };
    w.destroy = () => { w.destroyed = true; rec.stdinEnded = true; if (!rec.stdinClosed) { try { PROC.close(rec.stdinFd); } catch (e) {} rec.stdinClosed = true; } return w; };
    w.setDefaultEncoding = () => w; w.setEncoding = () => w; w.cork = () => {}; w.uncork = () => {};
    return w;
  };

  const mapSig = (sig) => {
    if (sig === 0 || sig === "0") return 0;
    if (typeof sig === "number") return sig;
    if (typeof sig === "string") return SIGMAP[sig] != null ? SIGMAP[sig] : 15;
    return 15;
  };
  const normStdio = (stdio) => {
    if (stdio == null) stdio = "pipe";
    if (typeof stdio === "string") stdio = [stdio, stdio, stdio];
    else stdio = stdio.slice();
    while (stdio.length < 3) stdio.push("pipe");
    return stdio.map((s) => (s == null ? "pipe" : typeof s === "number" ? s : s === "overlapped" ? "pipe" : s === "ipc" ? "ignore" : s));
  };

  class ChildProcess extends EventEmitter {
    constructor() {
      super();
      this.pid = undefined; this.stdin = null; this.stdout = null; this.stderr = null;
      this.stdio = [null, null, null]; this.exitCode = null; this.signalCode = null;
      this.killed = false; this.connected = false; this.spawnfile = undefined; this.spawnargs = [];
      this._rec = null; this._timeoutTimer = null;
    }
    spawn(options) {
      // node child_process.ts:1346-1396 validators (ERR_INVALID_ARG_TYPE).
      if (options === null || typeof options !== "object") { const e = new TypeError('The "options" argument must be of type object. Received ' + (options === null ? "null" : typeof options)); e.code = "ERR_INVALID_ARG_TYPE"; throw e; }
      if (typeof options.file !== "string") { const e = new TypeError('The "options.file" property must be of type string. Received ' + (options.file === undefined ? "undefined" : typeof options.file)); e.code = "ERR_INVALID_ARG_TYPE"; throw e; }
      if (options.args !== undefined && !Array.isArray(options.args)) { const e = new TypeError('The "options.args" property must be an instance of Array. Received ' + typeof options.args); e.code = "ERR_INVALID_ARG_TYPE"; throw e; }
      const __hasIpc = Array.isArray(options.stdio) && options.stdio.includes("ipc");
      if (__hasIpc && options.envPairs !== undefined && !Array.isArray(options.envPairs)) { const e = new TypeError('The "options.envPairs" property must be an instance of Array. Received ' + typeof options.envPairs); e.code = "ERR_INVALID_ARG_TYPE"; throw e; }
      const file = toStr(options.file != null ? options.file : options.execPath);
      let args = options.args && options.args.length ? options.args.map(toStr) : [file];
      if (options.argv0 != null) args[0] = toStr(options.argv0);
      this.spawnfile = file; this.spawnargs = args;
      const stdio = normStdio(options.stdio);
      const sopts = { stdio };
      if (options.cwd != null) sopts.cwd = toStr(options.cwd);
      // node inherits process.env when env is unset (undefined/null); an explicit
      // {} means an empty environment. Snapshot process.env so the child sees the
      // JS-visible env (harness-injected vars), not just the raw OS environ.
      sopts.env = options.env && typeof options.env === "object" ? options.env : (G.process && G.process.env) || {};
      if (options.detached) sopts.detached = true;
      if (typeof options.uid === "number") sopts.uid = options.uid;
      if (typeof options.gid === "number") sopts.gid = options.gid;
      const self = this;
      const h = PROC.spawnEx(file, args, sopts);
      if (h.errno != null) {
        const code = ERRNO[h.errno] || ("errno " + h.errno);
        const err = new Error("spawn " + file + " " + code);
        err.errno = -1; err.code = code; err.path = file; err.spawnargs = args.slice(1);
        if (DELAYED[code]) {
          err.syscall = "spawn " + file;
          nextTick(() => { self.emit("error", err); self.emit("close", null, null); });
          return this;
        }
        err.syscall = "spawn";
        throw err;
      }
      this.pid = h.pid;
      const fds = h.fds || [];
      const rec = { cp: this, pid: h.pid, outs: [], stdinFd: -1, stdinBuf: [], stdinEnded: false, stdinClosed: false, exited: false, closed: false, done: false, code: null, signal: null };
      const stdioArr = [];
      for (let i = 0; i < stdio.length; i++) {
        const fd = fds[i] != null ? fds[i] : -1;
        if (stdio[i] === "pipe" && fd >= 0) {
          if (i === 0) { PROC.setNonBlock(fd); rec.stdinFd = fd; const wr = makeStdin(fd, rec); this.stdin = wr; stdioArr[i] = wr; }
          // Out fds MUST be O_NONBLOCK: drainOut loops readNB until "" (EAGAIN);
          // on a blocking fd the read AFTER a partial chunk wedges the JS thread
          // while a long-lived child sits between replies (duplex protocols).
          else { PROC.setNonBlock(fd); const rd = makeReadable(); rec.outs.push({ fd, stream: rd, ended: false }); stdioArr[i] = rd; if (i === 1) this.stdout = rd; else if (i === 2) this.stderr = rd; }
        } else { stdioArr[i] = null; }
      }
      this.stdio = stdioArr;
      this._rec = rec;
      CHILDREN.add(rec);
      if (options.timeout && options.timeout > 0) {
        this._timeoutTimer = G.setTimeout(() => { self.kill(options.killSignal || "SIGTERM"); }, options.timeout);
      }
      const sig = options.signal;
      if (sig) {
        // node abortChildProcess (child_process.ts:1808-1817): on abort, kill AND
        // emit an AbortError carrying the signal's reason as .cause.
        const doAbort = () => {
          if (self.kill(options.killSignal || "SIGTERM")) {
            const e = new Error("The operation was aborted"); e.name = "AbortError"; e.code = "ABORT_ERR"; e.cause = sig.reason;
            nextTick(() => self.emit("error", e));
          }
        };
        if (sig.aborted) nextTick(doAbort);
        else if (typeof sig.addEventListener === "function") sig.addEventListener("abort", doAbort, { once: true });
      }
      // 'exit' clears any pending timeout so it cannot kill a reused pid.
      this.once("exit", () => { if (self._timeoutTimer) { G.clearTimeout(self._timeoutTimer); self._timeoutTimer = null; } });
      nextTick(() => self.emit("spawn"));
      return this;
    }
    kill(sig) {
      // node convertToValidSignal (child_process.ts:808-816): unknown signal name
      // is ERR_UNKNOWN_SIGNAL, not a silent default to SIGTERM.
      if (typeof sig === "string" && sig !== "0" && SIGMAP[sig] == null) { const e = new TypeError("Unknown signal: " + sig); e.code = "ERR_UNKNOWN_SIGNAL"; throw e; }
      if (this.pid == null) return false;
      const ok = PROC.kill(this.pid, mapSig(sig));
      if (ok) this.killed = true;
      return ok;
    }
    ref() { return this; }
    unref() { return this; }
    disconnect() {}
    send() { return false; }
    [Symbol.dispose]() { this.kill(); }
  }

  const resolveExe = (file) => {
    if (file.indexOf("/") >= 0) return F.exists(file) ? file : null;
    try { return G.Bun && Bun.which ? Bun.which(file) : file; } catch (e) { return file; }
  };

  function spawnSync(cmd, a, o) {
    const n = normArgs(a, o);
    cmd = toStr(cmd);
    const exe = resolveExe(cmd);
    if (exe === null) {
      const err = new Error("spawnSync " + cmd + " ENOENT");
      err.code = "ENOENT"; err.errno = -2; err.syscall = "spawnSync " + cmd; err.path = cmd; err.spawnargs = n.args.map(toStr);
      return { pid: 0, output: [null, null, null], stdout: null, stderr: null, status: null, signal: null, error: err };
    }
    const r = CP.spawnSync(cmd, n.args.map(toStr), n.opts);
    if (r.errno != null) {  // child-side pre-exec failure (EPERM/ENOENT/…)
      const code = ERRNO[r.errno] || ("errno " + r.errno);
      const err = new Error("spawnSync " + cmd + " " + code);
      err.code = code; err.errno = -1; err.syscall = "spawnSync " + cmd; err.path = cmd; err.spawnargs = n.args.map(toStr);
      return { pid: r.pid, output: [null, null, null], stdout: null, stderr: null, status: null, signal: null, error: err };
    }
    if (r.signal != null) r.signal = SIGNAME[+r.signal] || ("SIG" + r.signal);
    const enc = n.opts && n.opts.encoding;
    // Node: with a string encoding stdout/stderr are decoded strings; with no
    // encoding (or "buffer") they are Buffers. Piped-but-empty stays a 0-len
    // Buffer; a non-piped fd (inherit/ignore) stays null.
    const asStr = enc && enc !== "buffer";
    if (r.stdout != null) r.stdout = asStr ? String(r.stdout) : Buffer.from(_u8(r.stdout));
    if (r.stderr != null) r.stderr = asStr ? String(r.stderr) : Buffer.from(_u8(r.stderr));
    r.output = [null, r.stdout, r.stderr];
    return r;
  }
  function execSync(command, o) {
    const r = CP.spawnSync("/bin/sh", ["-c", toStr(command)], o || {});
    if (r.status !== 0) { const e = new Error("Command failed: " + command + "\n" + r.stderr); e.status = r.status; e.stdout = r.stdout; e.stderr = r.stderr; throw e; }
    const enc = o && o.encoding;
    return enc === "buffer" || enc == null ? Buffer.from(_u8(r.stdout)) : r.stdout;
  }
  function execFileSync(file, a, o) {
    const n = normArgs(a, o);
    const r = CP.spawnSync(toStr(file), n.args.map(toStr), n.opts);
    if (r.status !== 0) { const e = new Error("execFileSync failed: " + file); e.status = r.status; e.stderr = r.stderr; throw e; }
    const enc = n.opts && n.opts.encoding;
    return enc === "buffer" || enc == null ? Buffer.from(_u8(r.stdout)) : r.stdout;
  }

  const collectExec = (child, options, cb, cmd) => {
    const enc = options.encoding === undefined ? "utf8" : options.encoding;
    const maxBuffer = options.maxBuffer == null ? 1024 * 1024 : options.maxBuffer;
    const outs = [], errs = [];
    let outLen = 0, errLen = 0, maxErr = null, done = false;
    const add = (which, name, bytes) => {
      const arr = which === 0 ? outs : errs;
      const len = which === 0 ? outLen : errLen;
      if (len + bytes.length > maxBuffer) {
        const take = Math.max(0, maxBuffer - len);
        if (take > 0) arr.push(Buffer.from(bytes.subarray(0, take)));
        if (which === 0) outLen = maxBuffer; else errLen = maxBuffer;
        if (!maxErr) { maxErr = new Error(name + " maxBuffer length exceeded"); maxErr.code = "ERR_CHILD_PROCESS_STDIO_MAXBUFFER"; maxErr.cmd = cmd; child.kill(); }
      } else { arr.push(Buffer.from(bytes)); if (which === 0) outLen += bytes.length; else errLen += bytes.length; }
    };
    if (child.stdout) child.stdout.on("data", (d) => add(0, "stdout", _u8(d)));
    if (child.stderr) child.stderr.on("data", (d) => add(1, "stderr", _u8(d)));
    const toOut = (b) => (enc === "buffer" || enc == null ? b : b.toString(enc === "utf-8" ? "utf8" : enc));
    const finish = (code, signal) => {
      if (done) return; done = true;
      const outBuf = Buffer.concat(outs), errBuf = Buffer.concat(errs);
      let err = maxErr;
      if (!err && ((code !== 0 && code != null) || signal)) {
        err = new Error("Command failed: " + cmd + (errBuf.length ? "\n" + errBuf.toString("utf8") : ""));
        err.code = signal ? null : code; err.killed = child.killed || false; err.signal = signal || null; err.cmd = cmd;
      }
      if (cb) cb(err || null, toOut(outBuf), toOut(errBuf));
    };
    child.on("close", (code, signal) => finish(code, signal));
    child.on("error", (e) => { if (done) return; done = true; if (cb) cb(e, toOut(Buffer.alloc(0)), toOut(Buffer.alloc(0))); });
  };

  function spawn(file, args, options) {
    if (typeof file !== "string") { const e = new TypeError('The "file" argument must be of type string. Received type ' + typeof file + ' (' + file + ')'); e.code = "ERR_INVALID_ARG_TYPE"; throw e; }
    if (!Array.isArray(args)) { options = args; args = []; }
    options = options || {};
    let cmd = file, argv = [file].concat((args || []).map(toStr));
    if (options.shell) {
      const command = [file].concat((args || []).map(toStr)).join(" ");
      const sh = options.shell === true ? "/bin/sh" : toStr(options.shell);
      cmd = sh; argv = [sh, "-c", command];
    }
    const child = new ChildProcess();
    child.spawn({ file: cmd, args: argv, cwd: options.cwd, env: options.env, stdio: options.stdio, detached: options.detached, uid: options.uid, gid: options.gid, argv0: options.argv0, timeout: options.timeout, killSignal: options.killSignal, signal: options.signal });
    return child;
  }

  function exec(command, options, cb) {
    if (typeof options === "function") { cb = options; options = {}; }
    options = options || {};
    const sh = options.shell ? (options.shell === true ? "/bin/sh" : toStr(options.shell)) : "/bin/sh";
    const child = new ChildProcess();
    child.spawn({ file: sh, args: [sh, "-c", toStr(command)], cwd: options.cwd, env: options.env, stdio: ["pipe", "pipe", "pipe"], timeout: options.timeout, killSignal: options.killSignal, signal: options.signal });
    collectExec(child, options, cb, command);
    return child;
  }
  exec[Symbol.for("nodejs.util.promisify.custom")] = (command, options) => new Promise((resolve, reject) => {
    exec(command, options, (err, stdout, stderr) => { if (err) { err.stdout = stdout; err.stderr = stderr; reject(err); } else resolve({ stdout, stderr }); });
  });

  function execFile(file, args, options, cb) {
    if (typeof args === "function") { cb = args; args = []; options = {}; }
    else if (!Array.isArray(args)) { cb = typeof options === "function" ? options : cb; options = args; args = []; }
    if (typeof options === "function") { cb = options; options = {}; }
    options = options || {};
    const child = new ChildProcess();
    child.spawn({ file: toStr(file), args: [toStr(file)].concat((args || []).map(toStr)), cwd: options.cwd, env: options.env, stdio: ["pipe", "pipe", "pipe"], timeout: options.timeout, killSignal: options.killSignal, signal: options.signal });
    collectExec(child, options, cb, file);
    return child;
  }
  execFile[Symbol.for("nodejs.util.promisify.custom")] = (file, args, options) => new Promise((resolve, reject) => {
    execFile(file, args, options, (err, stdout, stderr) => { if (err) { err.stdout = stdout; err.stderr = stderr; reject(err); } else resolve({ stdout, stderr }); });
  });

  function fork(modulePath, args, options) {
    if (typeof modulePath !== "string") { const e = new TypeError('The "modulePath" argument must be of type string or an instance of Buffer or URL. Received ' + (modulePath === null ? "null" : modulePath === undefined ? "undefined" : typeof modulePath === "object" ? "an instance of " + ((modulePath.constructor && modulePath.constructor.name) || "Object") : typeof modulePath === "symbol" ? "type symbol" : typeof modulePath)); e.code = "ERR_INVALID_ARG_TYPE"; throw e; }
    if (!Array.isArray(args)) { options = args; args = []; }
    if (options !== undefined && options !== null && typeof options !== "object") { const e = new TypeError('The "options" argument must be of type object. Received ' + (typeof options === "symbol" ? "type symbol" : typeof options)); e.code = "ERR_INVALID_ARG_TYPE"; throw e; }
    options = options || {};
    const exe = toStr(options.execPath || (G.process && G.process.execPath) || "bun");
    // IPC unimplemented → default stdio has no channel; stdin stays non-pipe so
    // child.stdin === null (matches the fork() default-stdio expectation).
    const stdio = options.stdio || (options.silent ? ["pipe", "pipe", "pipe"] : ["inherit", "inherit", "inherit"]);
    const child = new ChildProcess();
    child.spawn({ file: exe, args: [exe, toStr(modulePath)].concat((args || []).map(toStr)), cwd: options.cwd, env: options.env, stdio, detached: options.detached, timeout: options.timeout, killSignal: options.killSignal, signal: options.signal });
    return child;
  }

  def(["child_process"], { spawnSync, execSync, execFileSync, execFile, exec, spawn, fork, ChildProcess });
  // node:console — the global console methods + a Console class over given streams.
  // Args format via util.formatWithOptions (strings pass through, non-strings are
  // inspected, %-specifiers honored) so objects render `{ foo: 'bar' }`, not
  // `[object Object]`. colorMode ('auto'|true|false) drives inspect colors; 'auto'
  // follows the target stream's isTTY. ref bun src/js/node/console.ts formatWithOptions.
  class Console { constructor(out) { const o = out && out.stdout ? out : { stdout: out }; Object.assign(this, G.console); const colorMode = o.colorMode === undefined ? "auto" : o.colorMode; const io = o.inspectOptions; const colorsFor = (s) => colorMode === "auto" ? !!(s && s.isTTY) : !!colorMode; const fmt = (s, a) => util.formatWithOptions(Object.assign({}, io, { colors: colorsFor(s) }), ...a) + "\n"; this.log = this.info = (...a) => { (o.stdout && o.stdout.write) ? o.stdout.write(fmt(o.stdout, a)) : G.console.log(...a); }; this.error = this.warn = (...a) => { (o.stderr && o.stderr.write) ? o.stderr.write(fmt(o.stderr, a)) : G.console.error(...a); }; } }
  // console.write(...chunks) — raw (no newline/format) write to stdout, returns
  // bytes written. ref: bun src/js/builtins/ConsoleObject.ts write(): the private
  // "writer" slot lookup rejects a non-object `this` (surfaces as ERR_INVALID_THIS).
  if (typeof G.console.write !== "function") {
    G.console.write = function write(input) {
      if (this == null || typeof this !== "object") {
        const e = new TypeError('The "this" value must be a Console instance');
        e.code = "ERR_INVALID_THIS";
        throw e;
      }
      let wrote = 0;
      const out = G.process && G.process.stdout;
      for (let i = 0; i < arguments.length; i++) {
        const chunk = arguments[i];
        if (typeof chunk === "string") {
          if (out && out.write) out.write(chunk);
          wrote += G.Buffer ? G.Buffer.byteLength(chunk) : chunk.length;
        } else if (ArrayBuffer.isView(chunk) || chunk instanceof ArrayBuffer) {
          if (out && out.write) out.write(chunk);
          wrote += chunk.byteLength;
        } else {
          const s = String(chunk);
          if (out && out.write) out.write(s);
          wrote += G.Buffer ? G.Buffer.byteLength(s) : s.length;
        }
      }
      return wrote;
    };
  }
  const consoleMod = Object.assign(Object.create(null), G.console, { Console });
  G.console.Console = Console;
  M["console"] = M["node:console"] = consoleMod;
  // The global console is a Console instance: its methods are own props, so
  // re-parenting to Console.prototype keeps them and makes `console instanceof
  // Console` true. _stdout/_stderr mirror process.stdout/stderr as non-enumerable
  // data props. ref bun src/js/node/console.ts (global console = new Console).
  try { Object.setPrototypeOf(G.console, Console.prototype); } catch (e) {}
  if (G.process) {
    try { Object.defineProperty(G.console, "_stdout", { value: G.process.stdout, writable: true, enumerable: false, configurable: true }); } catch (e) {}
    try { Object.defineProperty(G.console, "_stderr", { value: G.process.stderr, writable: true, enumerable: false, configurable: true }); } catch (e) {}
  }

  // ---- Web/node globals: TextEncoder/TextDecoder, Buffer, btoa/atob ----
  if (typeof G.TextEncoder === "undefined") {
    G.TextEncoder = class TextEncoder {
      get [Symbol.toStringTag]() { return "TextEncoder"; }
      get encoding() { return "utf-8"; }
      encode(str = "") {
        str = String(str); const out = [];
        for (let i = 0; i < str.length; i++) {
          let c = str.charCodeAt(i);
          if (c < 0x80) out.push(c);
          else if (c < 0x800) out.push(0xc0 | (c >> 6), 0x80 | (c & 0x3f));
          else if (c >= 0xd800 && c <= 0xdbff) {
            const trail = i + 1 < str.length ? str.charCodeAt(i + 1) : 0;
            if (trail >= 0xdc00 && trail <= 0xdfff) {
              const cp = 0x10000 + ((c - 0xd800) << 10) + (trail - 0xdc00); i++;
              out.push(0xf0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3f), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f));
            } else out.push(0xef, 0xbf, 0xbd);
          } else if (c >= 0xdc00 && c <= 0xdfff) out.push(0xef, 0xbf, 0xbd);
          else out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 0x3f), 0x80 | (c & 0x3f));
        }
        return new Uint8Array(out);
      }
      encodeInto(str, dest) {
        str = String(str);
        if (!(dest instanceof Uint8Array)) throw new TypeError("The destination must be a Uint8Array");
        let read = 0, written = 0;
        while (read < str.length) {
          const first = str.charCodeAt(read); let consumed = 1, bytes;
          if (first < 0x80) bytes = [first];
          else if (first < 0x800) bytes = [0xc0 | (first >> 6), 0x80 | (first & 0x3f)];
          else if (first >= 0xd800 && first <= 0xdbff) {
            const trail = read + 1 < str.length ? str.charCodeAt(read + 1) : 0;
            if (trail >= 0xdc00 && trail <= 0xdfff) { const cp = 0x10000 + ((first - 0xd800) << 10) + (trail - 0xdc00); consumed = 2; bytes = [0xf0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3f), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f)]; }
            else bytes = [0xef, 0xbf, 0xbd];
          } else if (first >= 0xdc00 && first <= 0xdfff) bytes = [0xef, 0xbf, 0xbd];
          else bytes = [0xe0 | (first >> 12), 0x80 | ((first >> 6) & 0x3f), 0x80 | (first & 0x3f)];
          if (written + bytes.length > dest.length) break;
          for (let i = 0; i < bytes.length; i++) dest[written++] = bytes[i];
          read += consumed;
        }
        return { read, written };
      }
    };
  }
  if (typeof G.TextDecoder === "undefined") {
    // encoding label → canonical name (WHATWG Encoding standard subset).
    const ENC_ALIAS = { "unicode-1-1-utf-8": "utf-8", "unicode11utf8": "utf-8", "unicode20utf8": "utf-8", "utf-8": "utf-8", "utf8": "utf-8", "x-unicode20utf8": "utf-8", "latin1": "windows-1252", "l1": "windows-1252", "iso-8859-1": "windows-1252", "iso8859-1": "windows-1252", "iso88591": "windows-1252", "cp1252": "windows-1252", "cp819": "windows-1252", "ibm819": "windows-1252", "csisolatin1": "windows-1252", "windows-1252": "windows-1252", "x-cp1252": "windows-1252", "ascii": "windows-1252", "us-ascii": "windows-1252", "ansi_x3.4-1968": "windows-1252", "utf-16le": "utf-16le", "utf-16": "utf-16le", "ucs-2": "utf-16le", "unicodefeff": "utf-16le", "csunicode": "utf-16le", "utf-16be": "utf-16be", "unicodefffe": "utf-16be" };
    // windows-1252 0x80–0x9F differ from latin1; other bytes map identically.
    const W1252 = { 128: 0x20ac, 130: 0x201a, 131: 0x0192, 132: 0x201e, 133: 0x2026, 134: 0x2020, 135: 0x2021, 136: 0x02c6, 137: 0x2030, 138: 0x0160, 139: 0x2039, 140: 0x0152, 142: 0x017d, 145: 0x2018, 146: 0x2019, 147: 0x201c, 148: 0x201d, 149: 0x2022, 150: 0x2013, 151: 0x2014, 152: 0x02dc, 153: 0x2122, 154: 0x0161, 155: 0x203a, 156: 0x0153, 158: 0x017e, 159: 0x0178 };
    // WHATWG legacy single-byte encodings (0x80-0xFF tables generated from the
    // Encoding standard indexes); plus x-user-defined (formula) and replacement.
    const SBT = {"ibm866":"\u0410\u0411\u0412\u0413\u0414\u0415\u0416\u0417\u0418\u0419\u041a\u041b\u041c\u041d\u041e\u041f\u0420\u0421\u0422\u0423\u0424\u0425\u0426\u0427\u0428\u0429\u042a\u042b\u042c\u042d\u042e\u042f\u0430\u0431\u0432\u0433\u0434\u0435\u0436\u0437\u0438\u0439\u043a\u043b\u043c\u043d\u043e\u043f\u2591\u2592\u2593\u2502\u2524\u2561\u2562\u2556\u2555\u2563\u2551\u2557\u255d\u255c\u255b\u2510\u2514\u2534\u252c\u251c\u2500\u253c\u255e\u255f\u255a\u2554\u2569\u2566\u2560\u2550\u256c\u2567\u2568\u2564\u2565\u2559\u2558\u2552\u2553\u256b\u256a\u2518\u250c\u2588\u2584\u258c\u2590\u2580\u0440\u0441\u0442\u0443\u0444\u0445\u0446\u0447\u0448\u0449\u044a\u044b\u044c\u044d\u044e\u044f\u0401\u0451\u0404\u0454\u0407\u0457\u040e\u045e\u00b0\u2219\u00b7\u221a\u2116\u00a4\u25a0\u00a0","iso-8859-3":"\u0080\u0081\u0082\u0083\u0084\u0085\u0086\u0087\u0088\u0089\u008a\u008b\u008c\u008d\u008e\u008f\u0090\u0091\u0092\u0093\u0094\u0095\u0096\u0097\u0098\u0099\u009a\u009b\u009c\u009d\u009e\u009f\u00a0\u0126\u02d8\u00a3\u00a4\ufffd\u0124\u00a7\u00a8\u0130\u015e\u011e\u0134\u00ad\ufffd\u017b\u00b0\u0127\u00b2\u00b3\u00b4\u00b5\u0125\u00b7\u00b8\u0131\u015f\u011f\u0135\u00bd\ufffd\u017c\u00c0\u00c1\u00c2\ufffd\u00c4\u010a\u0108\u00c7\u00c8\u00c9\u00ca\u00cb\u00cc\u00cd\u00ce\u00cf\ufffd\u00d1\u00d2\u00d3\u00d4\u0120\u00d6\u00d7\u011c\u00d9\u00da\u00db\u00dc\u016c\u015c\u00df\u00e0\u00e1\u00e2\ufffd\u00e4\u010b\u0109\u00e7\u00e8\u00e9\u00ea\u00eb\u00ec\u00ed\u00ee\u00ef\ufffd\u00f1\u00f2\u00f3\u00f4\u0121\u00f6\u00f7\u011d\u00f9\u00fa\u00fb\u00fc\u016d\u015d\u02d9","iso-8859-5":"\u0080\u0081\u0082\u0083\u0084\u0085\u0086\u0087\u0088\u0089\u008a\u008b\u008c\u008d\u008e\u008f\u0090\u0091\u0092\u0093\u0094\u0095\u0096\u0097\u0098\u0099\u009a\u009b\u009c\u009d\u009e\u009f\u00a0\u0401\u0402\u0403\u0404\u0405\u0406\u0407\u0408\u0409\u040a\u040b\u040c\u00ad\u040e\u040f\u0410\u0411\u0412\u0413\u0414\u0415\u0416\u0417\u0418\u0419\u041a\u041b\u041c\u041d\u041e\u041f\u0420\u0421\u0422\u0423\u0424\u0425\u0426\u0427\u0428\u0429\u042a\u042b\u042c\u042d\u042e\u042f\u0430\u0431\u0432\u0433\u0434\u0435\u0436\u0437\u0438\u0439\u043a\u043b\u043c\u043d\u043e\u043f\u0440\u0441\u0442\u0443\u0444\u0445\u0446\u0447\u0448\u0449\u044a\u044b\u044c\u044d\u044e\u044f\u2116\u0451\u0452\u0453\u0454\u0455\u0456\u0457\u0458\u0459\u045a\u045b\u045c\u00a7\u045e\u045f","iso-8859-6":"\u0080\u0081\u0082\u0083\u0084\u0085\u0086\u0087\u0088\u0089\u008a\u008b\u008c\u008d\u008e\u008f\u0090\u0091\u0092\u0093\u0094\u0095\u0096\u0097\u0098\u0099\u009a\u009b\u009c\u009d\u009e\u009f\u00a0\ufffd\ufffd\ufffd\u00a4\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\u060c\u00ad\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\u061b\ufffd\ufffd\ufffd\u061f\ufffd\u0621\u0622\u0623\u0624\u0625\u0626\u0627\u0628\u0629\u062a\u062b\u062c\u062d\u062e\u062f\u0630\u0631\u0632\u0633\u0634\u0635\u0636\u0637\u0638\u0639\u063a\ufffd\ufffd\ufffd\ufffd\ufffd\u0640\u0641\u0642\u0643\u0644\u0645\u0646\u0647\u0648\u0649\u064a\u064b\u064c\u064d\u064e\u064f\u0650\u0651\u0652\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd","iso-8859-7":"\u0080\u0081\u0082\u0083\u0084\u0085\u0086\u0087\u0088\u0089\u008a\u008b\u008c\u008d\u008e\u008f\u0090\u0091\u0092\u0093\u0094\u0095\u0096\u0097\u0098\u0099\u009a\u009b\u009c\u009d\u009e\u009f\u00a0\u2018\u2019\u00a3\u20ac\u20af\u00a6\u00a7\u00a8\u00a9\u037a\u00ab\u00ac\u00ad\ufffd\u2015\u00b0\u00b1\u00b2\u00b3\u0384\u0385\u0386\u00b7\u0388\u0389\u038a\u00bb\u038c\u00bd\u038e\u038f\u0390\u0391\u0392\u0393\u0394\u0395\u0396\u0397\u0398\u0399\u039a\u039b\u039c\u039d\u039e\u039f\u03a0\u03a1\ufffd\u03a3\u03a4\u03a5\u03a6\u03a7\u03a8\u03a9\u03aa\u03ab\u03ac\u03ad\u03ae\u03af\u03b0\u03b1\u03b2\u03b3\u03b4\u03b5\u03b6\u03b7\u03b8\u03b9\u03ba\u03bb\u03bc\u03bd\u03be\u03bf\u03c0\u03c1\u03c2\u03c3\u03c4\u03c5\u03c6\u03c7\u03c8\u03c9\u03ca\u03cb\u03cc\u03cd\u03ce\ufffd","iso-8859-8":"\u0080\u0081\u0082\u0083\u0084\u0085\u0086\u0087\u0088\u0089\u008a\u008b\u008c\u008d\u008e\u008f\u0090\u0091\u0092\u0093\u0094\u0095\u0096\u0097\u0098\u0099\u009a\u009b\u009c\u009d\u009e\u009f\u00a0\ufffd\u00a2\u00a3\u00a4\u00a5\u00a6\u00a7\u00a8\u00a9\u00d7\u00ab\u00ac\u00ad\u00ae\u00af\u00b0\u00b1\u00b2\u00b3\u00b4\u00b5\u00b6\u00b7\u00b8\u00b9\u00f7\u00bb\u00bc\u00bd\u00be\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\u2017\u05d0\u05d1\u05d2\u05d3\u05d4\u05d5\u05d6\u05d7\u05d8\u05d9\u05da\u05db\u05dc\u05dd\u05de\u05df\u05e0\u05e1\u05e2\u05e3\u05e4\u05e5\u05e6\u05e7\u05e8\u05e9\u05ea\ufffd\ufffd\u200e\u200f\ufffd","koi8-u":"\u2500\u2502\u250c\u2510\u2514\u2518\u251c\u2524\u252c\u2534\u253c\u2580\u2584\u2588\u258c\u2590\u2591\u2592\u2593\u2320\u25a0\u2219\u221a\u2248\u2264\u2265\u00a0\u2321\u00b0\u00b2\u00b7\u00f7\u2550\u2551\u2552\u0451\u0454\u2554\u0456\u0457\u2557\u2558\u2559\u255a\u255b\u0491\u255d\u255e\u255f\u2560\u2561\u0401\u0404\u2563\u0406\u0407\u2566\u2567\u2568\u2569\u256a\u0490\u256c\u00a9\u044e\u0430\u0431\u0446\u0434\u0435\u0444\u0433\u0445\u0438\u0439\u043a\u043b\u043c\u043d\u043e\u043f\u044f\u0440\u0441\u0442\u0443\u0436\u0432\u044c\u044b\u0437\u0448\u044d\u0449\u0447\u044a\u042e\u0410\u0411\u0426\u0414\u0415\u0424\u0413\u0425\u0418\u0419\u041a\u041b\u041c\u041d\u041e\u041f\u042f\u0420\u0421\u0422\u0423\u0416\u0412\u042c\u042b\u0417\u0428\u042d\u0429\u0427\u042a","windows-1253":"\u20ac\ufffd\u201a\u0192\u201e\u2026\u2020\u2021\ufffd\u2030\ufffd\u2039\ufffd\ufffd\ufffd\ufffd\ufffd\u2018\u2019\u201c\u201d\u2022\u2013\u2014\ufffd\u2122\ufffd\u203a\ufffd\ufffd\ufffd\ufffd\u00a0\u0385\u0386\u00a3\u00a4\u00a5\u00a6\u00a7\u00a8\u00a9\ufffd\u00ab\u00ac\u00ad\u00ae\u2015\u00b0\u00b1\u00b2\u00b3\u0384\u00b5\u00b6\u00b7\u0388\u0389\u038a\u00bb\u038c\u00bd\u038e\u038f\u0390\u0391\u0392\u0393\u0394\u0395\u0396\u0397\u0398\u0399\u039a\u039b\u039c\u039d\u039e\u039f\u03a0\u03a1\ufffd\u03a3\u03a4\u03a5\u03a6\u03a7\u03a8\u03a9\u03aa\u03ab\u03ac\u03ad\u03ae\u03af\u03b0\u03b1\u03b2\u03b3\u03b4\u03b5\u03b6\u03b7\u03b8\u03b9\u03ba\u03bb\u03bc\u03bd\u03be\u03bf\u03c0\u03c1\u03c2\u03c3\u03c4\u03c5\u03c6\u03c7\u03c8\u03c9\u03ca\u03cb\u03cc\u03cd\u03ce\ufffd","windows-1255":"\u20ac\ufffd\u201a\u0192\u201e\u2026\u2020\u2021\u02c6\u2030\ufffd\u2039\ufffd\ufffd\ufffd\ufffd\ufffd\u2018\u2019\u201c\u201d\u2022\u2013\u2014\u02dc\u2122\ufffd\u203a\ufffd\ufffd\ufffd\ufffd\u00a0\u00a1\u00a2\u00a3\u20aa\u00a5\u00a6\u00a7\u00a8\u00a9\u00d7\u00ab\u00ac\u00ad\u00ae\u00af\u00b0\u00b1\u00b2\u00b3\u00b4\u00b5\u00b6\u00b7\u00b8\u00b9\u00f7\u00bb\u00bc\u00bd\u00be\u00bf\u05b0\u05b1\u05b2\u05b3\u05b4\u05b5\u05b6\u05b7\u05b8\u05b9\ufffd\u05bb\u05bc\u05bd\u05be\u05bf\u05c0\u05c1\u05c2\u05c3\u05f0\u05f1\u05f2\u05f3\u05f4\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\u05d0\u05d1\u05d2\u05d3\u05d4\u05d5\u05d6\u05d7\u05d8\u05d9\u05da\u05db\u05dc\u05dd\u05de\u05df\u05e0\u05e1\u05e2\u05e3\u05e4\u05e5\u05e6\u05e7\u05e8\u05e9\u05ea\ufffd\ufffd\u200e\u200f\ufffd","windows-1257":"\u20ac\ufffd\u201a\ufffd\u201e\u2026\u2020\u2021\ufffd\u2030\ufffd\u2039\ufffd\u00a8\u02c7\u00b8\ufffd\u2018\u2019\u201c\u201d\u2022\u2013\u2014\ufffd\u2122\ufffd\u203a\ufffd\u00af\u02db\ufffd\u00a0\ufffd\u00a2\u00a3\u00a4\ufffd\u00a6\u00a7\u00d8\u00a9\u0156\u00ab\u00ac\u00ad\u00ae\u00c6\u00b0\u00b1\u00b2\u00b3\u00b4\u00b5\u00b6\u00b7\u00f8\u00b9\u0157\u00bb\u00bc\u00bd\u00be\u00e6\u0104\u012e\u0100\u0106\u00c4\u00c5\u0118\u0112\u010c\u00c9\u0179\u0116\u0122\u0136\u012a\u013b\u0160\u0143\u0145\u00d3\u014c\u00d5\u00d6\u00d7\u0172\u0141\u015a\u016a\u00dc\u017b\u017d\u00df\u0105\u012f\u0101\u0107\u00e4\u00e5\u0119\u0113\u010d\u00e9\u017a\u0117\u0123\u0137\u012b\u013c\u0161\u0144\u0146\u00f3\u014d\u00f5\u00f6\u00f7\u0173\u0142\u015b\u016b\u00fc\u017c\u017e\u02d9","windows-874":"\u20ac\ufffd\ufffd\ufffd\ufffd\u2026\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\u2018\u2019\u201c\u201d\u2022\u2013\u2014\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\u00a0\u0e01\u0e02\u0e03\u0e04\u0e05\u0e06\u0e07\u0e08\u0e09\u0e0a\u0e0b\u0e0c\u0e0d\u0e0e\u0e0f\u0e10\u0e11\u0e12\u0e13\u0e14\u0e15\u0e16\u0e17\u0e18\u0e19\u0e1a\u0e1b\u0e1c\u0e1d\u0e1e\u0e1f\u0e20\u0e21\u0e22\u0e23\u0e24\u0e25\u0e26\u0e27\u0e28\u0e29\u0e2a\u0e2b\u0e2c\u0e2d\u0e2e\u0e2f\u0e30\u0e31\u0e32\u0e33\u0e34\u0e35\u0e36\u0e37\u0e38\u0e39\u0e3a\ufffd\ufffd\ufffd\ufffd\u0e3f\u0e40\u0e41\u0e42\u0e43\u0e44\u0e45\u0e46\u0e47\u0e48\u0e49\u0e4a\u0e4b\u0e4c\u0e4d\u0e4e\u0e4f\u0e50\u0e51\u0e52\u0e53\u0e54\u0e55\u0e56\u0e57\u0e58\u0e59\u0e5a\u0e5b\ufffd\ufffd\ufffd\ufffd"};
    SBT["iso-8859-8-i"] = SBT["iso-8859-8"];
    const SB_ALIAS = { "ibm866":"ibm866","866":"ibm866","cp866":"ibm866","csibm866":"ibm866", "iso-8859-3":"iso-8859-3","iso8859-3":"iso-8859-3","iso88593":"iso-8859-3","latin3":"iso-8859-3","l3":"iso-8859-3","csisolatin3":"iso-8859-3","iso-ir-109":"iso-8859-3","iso_8859-3":"iso-8859-3","iso_8859-3:1988":"iso-8859-3", "iso-8859-5":"iso-8859-5","iso8859-5":"iso-8859-5","iso88595":"iso-8859-5","cyrillic":"iso-8859-5","csisolatincyrillic":"iso-8859-5","iso-ir-144":"iso-8859-5","iso_8859-5":"iso-8859-5","iso_8859-5:1988":"iso-8859-5", "iso-8859-6":"iso-8859-6","iso8859-6":"iso-8859-6","iso88596":"iso-8859-6","arabic":"iso-8859-6","csisolatinarabic":"iso-8859-6","ecma-114":"iso-8859-6","asmo-708":"iso-8859-6","iso-ir-127":"iso-8859-6","iso_8859-6":"iso-8859-6","iso_8859-6:1987":"iso-8859-6", "iso-8859-7":"iso-8859-7","iso8859-7":"iso-8859-7","iso88597":"iso-8859-7","greek":"iso-8859-7","greek8":"iso-8859-7","ecma-118":"iso-8859-7","elot_928":"iso-8859-7","csisolatingreek":"iso-8859-7","iso-ir-126":"iso-8859-7","iso_8859-7":"iso-8859-7","iso_8859-7:1987":"iso-8859-7","sun_eu_greek":"iso-8859-7", "iso-8859-8":"iso-8859-8","iso8859-8":"iso-8859-8","iso88598":"iso-8859-8","hebrew":"iso-8859-8","visual":"iso-8859-8","csisolatinhebrew":"iso-8859-8","iso-ir-138":"iso-8859-8","iso_8859-8":"iso-8859-8","iso_8859-8:1988":"iso-8859-8","csiso88598e":"iso-8859-8","iso-8859-8-e":"iso-8859-8", "iso-8859-8-i":"iso-8859-8-i","csiso88598i":"iso-8859-8-i","logical":"iso-8859-8-i", "koi8-u":"koi8-u","koi8-ru":"koi8-u", "windows-1253":"windows-1253","cp1253":"windows-1253","x-cp1253":"windows-1253", "windows-1255":"windows-1255","cp1255":"windows-1255","x-cp1255":"windows-1255", "windows-1257":"windows-1257","cp1257":"windows-1257","x-cp1257":"windows-1257", "windows-874":"windows-874","cp874":"windows-874","dos-874":"windows-874","iso-8859-11":"windows-874","iso8859-11":"windows-874","iso885911":"windows-874","tis-620":"windows-874", "x-user-defined":"x-user-defined", "replacement":"replacement","csiso2022kr":"replacement","hz-gb-2312":"replacement","iso-2022-cn":"replacement","iso-2022-cn-ext":"replacement","iso-2022-kr":"replacement" };
    G.TextDecoder = class TextDecoder {
      get [Symbol.toStringTag]() { return "TextDecoder"; }
      constructor(enc, opts) {
        const key = String(enc === undefined ? "utf-8" : enc).toLowerCase().trim();
        this.encoding = ENC_ALIAS[key] || SB_ALIAS[key];
        if (!this.encoding || this.encoding === "replacement") throw new RangeError("The encoding label provided ('" + enc + "') is invalid.");
        if (opts !== undefined && (opts === null || typeof opts !== "object")) throw new TypeError("TextDecoder(options) is invalid");
        this.fatal = !!(opts && opts.fatal);
        if (opts && opts.ignoreBOM !== undefined && typeof opts.ignoreBOM !== "boolean") throw new TypeError("TextDecoder(options) ignoreBOM is invalid. Expected boolean value");
        this.ignoreBOM = !!(opts && opts.ignoreBOM);
        this._doNotFlush = false;
      }
      decode(input, opts) {
        const stream = !!(opts && opts.stream);
        let b;
        if (input == null) b = new Uint8Array(0);
        else if (input instanceof ArrayBuffer) b = input.detached ? new Uint8Array(0) : new Uint8Array(input);
        else if (ArrayBuffer.isView(input)) {
          const byteLength = input.byteLength;
          b = byteLength === 0 ? new Uint8Array(0) : new Uint8Array(input.buffer, input.byteOffset, byteLength);
        }
        else throw new TypeError("TextDecoder.decode expects an ArrayBuffer or TypedArray");
        if (!this._doNotFlush) { this._pending = null; this._leadSurrogate = null; this._bomSeen = false; }
        this._doNotFlush = stream;
        // Streaming: prepend bytes held back from a prior decode(_, {stream:true}).
        if (this._pending && this._pending.length) {
          const m = new Uint8Array(this._pending.length + b.length);
          m.set(this._pending, 0); m.set(b, this._pending.length);
          b = m;
        }
        this._pending = null;
        const enc = this.encoding;
        // ---- utf-8 / utf-16 streaming decoders (BOM + incomplete-sequence across chunks) ----
        if (enc === "utf-8" || enc === "utf-16le" || enc === "utf-16be") {
          let i = 0;
          let out = "";
          const F = () => { const error = new TypeError("The encoded data was not valid for encoding " + enc + "."); error.code = "ERR_ENCODING_INVALID_ENCODED_DATA"; throw error; };
          const serialize = () => {
            if (!this.ignoreBOM && !this._bomSeen && out.length) {
              this._bomSeen = true;
              if (out.charCodeAt(0) === 0xfeff) out = out.slice(1);
            }
            return out;
          };
          if (enc === "utf-8") {
            // WHATWG UTF-8 decoder: overlong/surrogate bounds on the first continuation,
            // an invalid continuation byte is NOT consumed (reprocessed as a new lead).
            while (i < b.length) {
              const c = b[i];
              if (c < 0x80) { out += String.fromCharCode(c); i++; continue; }
              let need, cp, lo = 0x80, hi = 0xbf;
              if (c >= 0xc2 && c <= 0xdf) { need = 1; cp = c & 0x1f; }
              else if (c >= 0xe0 && c <= 0xef) { need = 2; cp = c & 0x0f; if (c === 0xe0) lo = 0xa0; else if (c === 0xed) hi = 0x9f; }
              else if (c >= 0xf0 && c <= 0xf4) { need = 3; cp = c & 0x07; if (c === 0xf0) lo = 0x90; else if (c === 0xf4) hi = 0x8f; }
              else { if (this.fatal) F(); out += "\ufffd"; i++; continue; }
              let j = i + 1, k = 0, bad = false, incomplete = false;
              for (; k < need; k++, j++) {
                if (j >= b.length) { incomplete = true; break; }
                const cc = b[j], lo2 = k === 0 ? lo : 0x80, hi2 = k === 0 ? hi : 0xbf;
                if (cc < lo2 || cc > hi2) { bad = true; break; }
                cp = (cp << 6) | (cc & 0x3f);
              }
              if (incomplete) { if (stream) { this._pending = b.slice(i); return serialize(); } if (this.fatal) F(); out += "\ufffd"; break; }
              if (bad) { if (this.fatal) F(); out += "\ufffd"; i = j; continue; }  // reprocess offending byte
              if (cp < 0x10000) out += String.fromCharCode(cp);
              else { cp -= 0x10000; out += String.fromCharCode(0xd800 + (cp >> 10), 0xdc00 + (cp & 0x3ff)); }
              i = j;
            }
          } else {
            const le = enc === "utf-16le";
            const emit = (u) => {
              if (this._leadSurrogate != null) {
                const lead = this._leadSurrogate; this._leadSurrogate = null;
                if (u >= 0xdc00 && u <= 0xdfff) { out += String.fromCharCode(lead, u); return; }
                if (this.fatal) F(); out += "\ufffd";
              }
              if (u >= 0xd800 && u <= 0xdbff) { this._leadSurrogate = u; return; }
              if (u >= 0xdc00 && u <= 0xdfff) { if (this.fatal) F(); out += "\ufffd"; return; }
              out += String.fromCharCode(u);
            };
            while (i + 1 < b.length) { emit(le ? (b[i] | (b[i + 1] << 8)) : ((b[i] << 8) | b[i + 1])); i += 2; }
            if (i < b.length) {
              if (stream) { this._pending = b.slice(i); return serialize(); }
              if (this.fatal) F(); out += "\ufffd";
            }
            if (!stream && this._leadSurrogate != null) { this._leadSurrogate = null; if (this.fatal) F(); out += "\ufffd"; }
          }
          return serialize();
        }
        // ---- legacy single-byte + specials (no multi-byte state) ----
        if (enc === "windows-1252") { let out = ""; for (let i = 0; i < b.length; i++) out += String.fromCharCode(W1252[b[i]] || b[i]); return out; }
        if (enc === "replacement") { if (b.length === 0) return ""; if (this.fatal) throw new TypeError("The encoded data was not valid."); return "\ufffd"; }
        if (enc === "x-user-defined") { let out = ""; for (let i = 0; i < b.length; i++) { const c = b[i]; out += String.fromCharCode(c < 0x80 ? c : 0xf780 + (c - 0x80)); } return out; }
        const sbt = SBT[enc]; if (sbt !== undefined) { let out = ""; for (let i = 0; i < b.length; i++) { const c = b[i]; if (c < 0x80) out += String.fromCharCode(c); else { const ch = sbt.charCodeAt(c - 0x80); if (ch === 0xfffd && this.fatal) throw new TypeError("The encoded data was not valid."); out += String.fromCharCode(ch); } } return out; }
        return "";
      }
    };
  }
  const B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  if (typeof G.btoa === "undefined") {
    G.btoa = function (s) { if (arguments.length === 0) throw new TypeError("btoa requires 1 argument (a string), but only 0 were passed"); s = String(s); for (let i = 0; i < s.length; i++) if (s.charCodeAt(i) > 0xff) throw new G.DOMException("The string to be encoded contains characters outside of the Latin1 range.", "InvalidCharacterError"); let o = ""; for (let i = 0; i < s.length;) { const c1 = s.charCodeAt(i++), c2 = s.charCodeAt(i++), c3 = s.charCodeAt(i++); const e1 = c1 >> 2, e2 = ((c1 & 3) << 4) | (c2 >> 4), e3 = isNaN(c2) ? 64 : ((c2 & 15) << 2) | (c3 >> 6), e4 = isNaN(c3) ? 64 : c3 & 63; o += B64[e1] + B64[e2] + (e3 === 64 ? "=" : B64[e3]) + (e4 === 64 ? "=" : B64[e4]); } return o; };
    G.atob = function (s) { if (arguments.length === 0) throw new TypeError("atob requires 1 argument (a string), but only 0 were passed"); s = String(s).replace(/[ \t\n\f\r]/g, ""); if (s.length % 4 === 0) s = s.replace(/={1,2}$/, ""); if (s.length % 4 === 1 || /[^A-Za-z0-9+/]/.test(s)) throw new G.DOMException("The string contains invalid characters.", "InvalidCharacterError"); let o = ""; for (let i = 0; i < s.length;) { const b1 = B64.indexOf(s[i++]), b2 = B64.indexOf(s[i++]), b3 = B64.indexOf(s[i++]), b4 = B64.indexOf(s[i++]); o += String.fromCharCode((b1 << 2) | (b2 >> 4)); if (b3 >= 0) o += String.fromCharCode(((b2 & 15) << 4) | (b3 >> 2)); if (b4 >= 0) o += String.fromCharCode(((b3 & 3) << 6) | b4); } return o; };
  }
  const te = new G.TextEncoder(), td = new G.TextDecoder();
  class Buffer extends Uint8Array {
    static from(data, e) {
      if (typeof data === "string") {
        if (e === "hex") { const a = []; for (let i = 0; i < data.length; i += 2) a.push(parseInt(data.substr(i, 2), 16)); return new Buffer(a); }
        if (e === "base64") { const s = G.atob(data); const a = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) a[i] = s.charCodeAt(i); return new Buffer(a); }
        // utf16le/ucs2: two little-endian bytes per UTF-16 code unit.
        if (e === "utf16le" || e === "ucs2" || e === "ucs-2" || e === "utf-16le") { const a = new Uint8Array(data.length * 2); for (let i = 0; i < data.length; i++) { const c = data.charCodeAt(i); a[i * 2] = c & 0xff; a[i * 2 + 1] = (c >> 8) & 0xff; } return new Buffer(a); }
        if (e === "latin1" || e === "binary" || e === "ascii") { const a = new Uint8Array(data.length); for (let i = 0; i < data.length; i++) a[i] = data.charCodeAt(i) & 0xff; return new Buffer(a); }
        return new Buffer(te.encode(data));
      }
      // node: Buffer.from(arrayBuffer[, byteOffset[, length]]) shares the memory.
      if (data instanceof ArrayBuffer) { const off = e >>> 0; return arguments[2] === undefined ? new Buffer(data, off) : new Buffer(data, off, arguments[2] >>> 0); }  // omit length -> length-tracking view over a resizable AB
      // node: Buffer.from(typedArray/array) copies element values as bytes (each
      // truncated to 0-255); the Uint8Array ctor performs the per-element ToUint8.
      if (Array.isArray(data) || (ArrayBuffer.isView(data) && !(data instanceof DataView))) return new Buffer(data);
      if (typeof data === "number") return new Buffer(data);
      return new Buffer(0);
    }
    static alloc(n, fill, encoding) { const b = new Buffer(n); if (fill != null) b.fill(fill, 0, b.length, encoding); return b; }
    static allocUnsafe(n) { return new Buffer(n); }
    // node: string/Buffer fills repeat the encoded byte pattern across the range.
    fill(value, offset, end, encoding) {
      // node JSBuffer.cpp fillBody (L1361-1479): resolve/validate a string
      // pattern's encoding first (its toString may detach); coerce offset/end;
      // short-circuit an empty range BEFORE coercing a numeric value; then re-read
      // length fresh (detached -> 0) and clamp before writing.
      if (typeof offset === "string") { encoding = offset; offset = undefined; end = undefined; }
      else if (typeof end === "string") { encoding = end; end = undefined; }
      const limit = this.length;
      const isBytes = value && (value instanceof Uint8Array || value._u8);
      let pat = null;
      if (typeof value === "string") pat = Buffer.from(value, encoding);  // validates encoding (may detach)
      else if (isBytes) pat = Buffer.from(value);
      let start = offset === undefined ? 0 : (Math.trunc(Number(offset)) || 0);
      let stop = end === undefined ? limit : (Math.trunc(Number(end)) || 0);
      if (start < 0) start = 0; else if (start > limit) start = limit;
      if (stop < 0) stop = 0; else if (stop > limit) stop = limit;
      if (start >= stop) return this;
      let numVal = 0;
      if (pat === null) numVal = (Math.trunc(Number(value)) || 0) & 0xff;  // valueOf may detach
      const postLimit = this.length;
      if (start > postLimit) start = postLimit;
      if (stop > postLimit) stop = postLimit;
      if (start >= stop) return this;
      if (pat === null) { for (let i = start; i < stop; i++) this[i] = numVal; return this; }
      if (pat.length === 0) { for (let i = start; i < stop; i++) this[i] = 0; return this; }
      for (let i = start; i < stop; i++) this[i] = pat[(i - start) % pat.length];
      return this;
    }
    static isBuffer(x) { return x instanceof Buffer; }
    static byteLength(s) { return typeof s === "string" ? te.encode(s).length : s.length; }
    static concat(list, total) { let len = total; if (len == null) { len = 0; for (const b of list) len += b.length; } const out = new Buffer(len); let off = 0; for (const b of list) { if (off >= len) break; out.set(b.subarray(0, Math.min(b.length, len - off)), off); off += b.length; } return out; }
    toString(e, start, end) {
      const s = this.subarray(start || 0, end == null ? this.length : end);
      if (e === "hex") { let o = ""; for (const x of s) o += x.toString(16).padStart(2, "0"); return o; }
      if (e === "base64") { let bin = ""; for (const x of s) bin += String.fromCharCode(x); return G.btoa(bin); }
      if (e === "latin1" || e === "binary" || e === "ascii") { let o = ""; for (const x of s) o += String.fromCharCode(x); return o; }
      // utf16le/ucs2: reassemble each UTF-16 code unit from two little-endian
      // bytes (a trailing odd byte is dropped, matching node). Decoding per byte
      // as latin1 would corrupt every multi-byte code unit / SIMD lane.
      if (e === "utf16le" || e === "ucs2" || e === "ucs-2" || e === "utf-16le") { let o = ""; for (let i = 0; i + 1 < s.length; i += 2) o += String.fromCharCode(s[i] | (s[i + 1] << 8)); return o; }
      return td.decode(s);
    }
    write(str, offset) { const e2 = te.encode(str); this.set(e2.subarray(0, this.length - (offset || 0)), offset || 0); return e2.length; }
    equals(o) { if (this.length !== o.length) return false; for (let i = 0; i < this.length; i++) if (this[i] !== o[i]) return false; return true; }
    slice(a, b) { return new Buffer(this.subarray(a, b)); }
    subarray(a, b) { return new Buffer(Uint8Array.prototype.subarray.call(this, a, b)); }
    copy(target, targetStart, sourceStart, sourceEnd) {
      // node JSBuffer.cpp copyBody (L1242-1301): coerce every numeric arg first
      // (each valueOf may detach/resize), THEN re-read lengths fresh (detached ->
      // 0), then clamp. Ordering is pinned by the detach tests.
      let ts = 0;
      if (targetStart !== undefined) { ts = Math.trunc(Number(targetStart)) || 0;
        if (ts < 0) { const e = new RangeError('The value of "targetStart" is out of range. It must be >= 0. Received ' + targetStart); e.code = "ERR_OUT_OF_RANGE"; throw e; } }
      let ss = 0;
      if (sourceStart !== undefined) { ss = Math.trunc(Number(sourceStart)) || 0;
        const slen = this.length;
        if (ss < 0 || ss > slen) { const e = new RangeError('The value of "sourceStart" is out of range. It must be >= 0 && <= ' + slen + '. Received ' + sourceStart); e.code = "ERR_OUT_OF_RANGE"; throw e; } }
      const seGiven = sourceEnd !== undefined;
      let seD = 0;
      if (seGiven) { seD = Math.trunc(Number(sourceEnd)) || 0;
        if (seD < 0) { const e = new RangeError('The value of "sourceEnd" is out of range. It must be >= 0. Received ' + sourceEnd); e.code = "ERR_OUT_OF_RANGE"; throw e; } }
      const sourceLength = this.length, targetLength = target.length;
      let se = seGiven ? Math.min(seD, sourceLength) : sourceLength;
      if (ts >= targetLength || ss >= se) return 0;
      if (se - ss > targetLength - ts) se = ss + (targetLength - ts);
      target.set(Uint8Array.prototype.subarray.call(this, ss, se), ts);
      return se - ss;
    }
    indexOf(v, off, enc) {
      // node JSBuffer.cpp: coerce byteOffset (toNumber) and encoding (toString)
      // FIRST — each can run JS that detaches the buffer — then re-read lengths
      // fresh (a detached buffer reads length 0).
      if (typeof off === "string") { enc = off; off = undefined; }
      if (enc !== undefined) String(enc);
      off = off === undefined ? 0 : (Number(off) | 0);
      if (typeof v === "string") v = te.encode(v);
      if (typeof v === "number") { if (this.length === 0) return -1; return Uint8Array.prototype.indexOf.call(this, v, off); }  // off coercion may have detached -> length 0 -> no match
      const hlen = this.length, nlen = v.length;
      let start = off < 0 ? Math.max(hlen + off, 0) : off;
      if (nlen === 0) return start <= hlen ? start : hlen;
      for (let i = start; i <= hlen - nlen; i++) { let m = true; for (let j = 0; j < nlen; j++) if (this[i + j] !== v[j]) { m = false; break; } if (m) return i; }
      return -1;
    }
    lastIndexOf(v, off, enc) {
      if (typeof off === "string") { enc = off; off = undefined; }
      if (enc !== undefined) String(enc);
      const hasOff = off !== undefined;
      off = hasOff ? (Number(off) | 0) : 0;
      if (typeof v === "string") v = te.encode(v);
      if (typeof v === "number") { if (this.length === 0) return -1; return Uint8Array.prototype.lastIndexOf.call(this, v, hasOff ? off : this.length - 1); }  // detached -> length 0 -> no match
      const hlen = this.length, nlen = v.length;
      let start = hasOff ? off : hlen - nlen;
      if (start < 0) start = hlen + start;
      if (start > hlen - nlen) start = hlen - nlen;
      if (nlen === 0) return start < 0 ? 0 : (start > hlen ? hlen : start);
      for (let i = start; i >= 0; i--) { let m = true; for (let j = 0; j < nlen; j++) if (this[i + j] !== v[j]) { m = false; break; } if (m) return i; }
      return -1;
    }
    includes(v, off) { return this.indexOf(v, off) !== -1; }
    readUInt8(o) { return this[o || 0]; }
    readInt8(o) { const v = this[o || 0]; return v < 128 ? v : v - 256; }
    writeUInt8(v, o) { o = o || 0; this[o] = v & 0xff; return o + 1; }
    writeInt8(v, o) { return this.writeUInt8(v, o); }
    readUInt16BE(o) { o = o || 0; return (this[o] << 8) | this[o + 1]; }
    readUInt16LE(o) { o = o || 0; return this[o] | (this[o + 1] << 8); }
    readUInt32BE(o) { o = o || 0; return ((this[o] << 24) | (this[o + 1] << 16) | (this[o + 2] << 8) | this[o + 3]) >>> 0; }
    readUInt32LE(o) { o = o || 0; return (this[o] | (this[o + 1] << 8) | (this[o + 2] << 16) | (this[o + 3] << 24)) >>> 0; }
    writeUInt16BE(v, o) { o = o || 0; this[o] = (v >> 8) & 0xff; this[o + 1] = v & 0xff; return o + 2; }
    writeUInt16LE(v, o) { o = o || 0; this[o] = v & 0xff; this[o + 1] = (v >> 8) & 0xff; return o + 2; }
    writeUInt32BE(v, o) { o = o || 0; this[o] = (v >>> 24) & 0xff; this[o + 1] = (v >>> 16) & 0xff; this[o + 2] = (v >>> 8) & 0xff; this[o + 3] = v & 0xff; return o + 4; }
    writeUInt32LE(v, o) { o = o || 0; this[o] = v & 0xff; this[o + 1] = (v >>> 8) & 0xff; this[o + 2] = (v >>> 16) & 0xff; this[o + 3] = (v >>> 24) & 0xff; return o + 4; }
    toJSON() { return { type: "Buffer", data: Array.from(this) }; }
  }
  Buffer.prototype.compare = function (target, ts, te, ss, se) { ts = ts || 0; te = te == null ? target.length : te; ss = ss || 0; se = se == null ? this.length : se; const a = this.subarray(ss, se), b = target.subarray(ts, te); const n = Math.min(a.length, b.length); for (let i = 0; i < n; i++) { if (a[i] < b[i]) return -1; if (a[i] > b[i]) return 1; } return a.length < b.length ? -1 : a.length > b.length ? 1 : 0; };
  // node accepts plain Uint8Arrays here, not just Buffers.
  Buffer.compare = (a, b) => (typeof a.compare === "function" ? a : Buffer.from(a.buffer, a.byteOffset, a.byteLength)).compare(b);
  Buffer.prototype.readUIntLE = function (o, len) { let v = 0, m = 1; for (let i = 0; i < len; i++) { v += this[o + i] * m; m *= 256; } return v; };
  Buffer.prototype.readUIntBE = function (o, len) { let v = 0; for (let i = 0; i < len; i++) v = v * 256 + this[o + i]; return v; };
  // signed 16/32-bit reads (writes reuse the unsigned writers — masking makes them
  // sign-correct) + IEEE float/double via a DataView over this backing buffer.
  Buffer.prototype.readInt16BE = function (o) { const v = this.readUInt16BE(o); return v & 0x8000 ? v - 0x10000 : v; };
  Buffer.prototype.readInt16LE = function (o) { const v = this.readUInt16LE(o); return v & 0x8000 ? v - 0x10000 : v; };
  Buffer.prototype.readInt32BE = function (o) { o = o || 0; return (this[o] << 24) | (this[o + 1] << 16) | (this[o + 2] << 8) | this[o + 3]; };
  Buffer.prototype.readInt32LE = function (o) { o = o || 0; return this[o] | (this[o + 1] << 8) | (this[o + 2] << 16) | (this[o + 3] << 24); };
  Buffer.prototype.writeInt16BE = Buffer.prototype.writeUInt16BE;
  Buffer.prototype.writeInt16LE = Buffer.prototype.writeUInt16LE;
  Buffer.prototype.writeInt32BE = Buffer.prototype.writeUInt32BE;
  Buffer.prototype.writeInt32LE = Buffer.prototype.writeUInt32LE;
  const bufDV = (b) => new DataView(b.buffer, b.byteOffset, b.byteLength);
  Buffer.prototype.readFloatBE = function (o) { return bufDV(this).getFloat32(o || 0, false); };
  Buffer.prototype.readFloatLE = function (o) { return bufDV(this).getFloat32(o || 0, true); };
  Buffer.prototype.readDoubleBE = function (o) { return bufDV(this).getFloat64(o || 0, false); };
  Buffer.prototype.readDoubleLE = function (o) { return bufDV(this).getFloat64(o || 0, true); };
  Buffer.prototype.writeFloatBE = function (v, o) { bufDV(this).setFloat32(o || 0, v, false); return (o || 0) + 4; };
  Buffer.prototype.writeFloatLE = function (v, o) { bufDV(this).setFloat32(o || 0, v, true); return (o || 0) + 4; };
  Buffer.prototype.writeDoubleBE = function (v, o) { bufDV(this).setFloat64(o || 0, v, false); return (o || 0) + 8; };
  Buffer.prototype.writeDoubleLE = function (v, o) { bufDV(this).setFloat64(o || 0, v, true); return (o || 0) + 8; };
  Buffer.prototype.readBigUInt64BE = function (o) { return bufDV(this).getBigUint64(o || 0, false); };
  Buffer.prototype.readBigUInt64LE = function (o) { return bufDV(this).getBigUint64(o || 0, true); };
  Buffer.prototype.writeBigUInt64BE = function (v, o) { bufDV(this).setBigUint64(o || 0, BigInt(v), false); return (o || 0) + 8; };
  Buffer.prototype.writeBigUInt64LE = function (v, o) { bufDV(this).setBigUint64(o || 0, BigInt(v), true); return (o || 0) + 8; };
  // node aliases: each read/write UInt* method also exists as Uint* (same fn).
  for (const m of Object.getOwnPropertyNames(Buffer.prototype)) {
    if (m.includes("UInt")) Buffer.prototype[m.replace("UInt", "Uint")] = Buffer.prototype[m];
  }
  // node:stream captures this Buffer before node_buffer_extra installs the full
  // one; isEncoding must exist here for WritableState encoding validation.
  if (typeof Buffer.isEncoding !== "function") {
    const VALID_ENC = { utf8: 1, "utf-8": 1, ucs2: 1, "ucs-2": 1, utf16le: 1, "utf-16le": 1, latin1: 1, binary: 1, ascii: 1, base64: 1, base64url: 1, hex: 1 };
    Buffer.isEncoding = function isEncoding(enc) { return typeof enc === "string" && VALID_ENC[enc.toLowerCase()] === 1; };
  }
  G.Buffer = Buffer;
  def(["buffer"], { Buffer, constants: { MAX_LENGTH: 0x7fffffff }, kMaxLength: 0x7fffffff });

  // ---- minimal React ----
  // JSX lowers to the AUTOMATIC runtime by default (jsxDEV/jsx/jsxs imported
  // from react/jsx-{dev-,}runtime), and to React.createElement only under
  // `@jsxRuntime classic` / `--jsx-runtime classic`. This shim has to cover
  // both, because it is what a JSX file resolves to when no real react is
  // installed.
  //
  // ⚠️ `jsxDEV` is why this needs saying. The dev entry used to be a plain alias
  // of the production one — `def(["react/jsx-dev-runtime"], M["react/jsx-runtime"])`
  // — which exports `jsx`/`jsxs`/`Fragment` and NO `jsxDEV`. Nothing noticed
  // while mbun lowered every file to `React.createElement`, since the dev entry
  // was then unreachable. It is the DEFAULT target now, so the alias surfaced as
  // `TypeError: jsxDEV_7x81h0kn is not a function` on any `<div/>` with no react
  // installed. The entries are spelled out per module now rather than aliased,
  // so each exports what its own name promises.
  //
  // (bun itself ships no shim at all: `bun run x.tsx` with no react is
  // `error: ENOENT while resolving package 'react/jsx-dev-runtime'`. This shim is
  // mbun's own convenience and is left in place — but it must at least be
  // self-consistent with the runtime mbun emits.)
  if (typeof G.React === "undefined") {
    const Fragment = Symbol.for("react.fragment");
    G.React = {
      Fragment,
      createElement(type, props, ...children) {
        const c = children.length === 1 ? children[0] : (children.length ? children : undefined);
        return { $$typeof: Symbol.for("react.element"), type, key: (props && props.key) || null, ref: (props && props.ref) || null, props: Object.assign({}, props, c !== undefined ? { children: c } : {}) };
      },
      isValidElement: (o) => !!(o && o.$$typeof === Symbol.for("react.element")),
    };
    // The automatic runtime passes `key` as its own argument and keeps children
    // inside `props`, where createElement takes key FROM props — so the key has
    // to be put back rather than dropped. The old `(t, p, k) => createElement(t, p)`
    // dropped it, which was latent rather than live: the classic lowering these
    // entries stood behind puts `key` in props, so `<li key={i}>` did keep its
    // key, and `k` only ever arrived for a caller importing `react/jsx-runtime`
    // by hand. With the automatic runtime now the default it is on the hot path.
    const mk = (t, p, k) => {
      const e = G.React.createElement(t, p);
      if (k !== undefined) e.key = k;
      return e;
    };
    def(["react/jsx-runtime"], { jsx: mk, jsxs: mk, Fragment });
    def(["react/jsx-dev-runtime"], { jsxDEV: mk, jsx: mk, jsxs: mk, Fragment });
  }

  // ---- real-time timers (setTimeout/setInterval/setImmediate) ----
  // Deadlines are wall-clock (Date.now()); __mbun_drain_timers() — driven by the
  // C++ pumps — fires only timers that are actually due, and the pumps sleep
  // until the earliest deadline via __mbun_pump_idle_ms() instead of busy-
  // spinning (bun: uSockets timer sweep on the event loop). setTimeout(cb, 100)
  // therefore takes ~100ms of real time, so `elapsed >= N` assertions hold.
  // unref/ref follow node semantics: only ref'd timers keep the process alive
  // (__mbun_timers_refd feeds the pumps' exit conditions); unref'd timers still
  // fire while the loop runs for other reasons.
  const T = (G.__mbunTimers = G.__mbunTimers || { q: [], id: 1, now: 0, fired: 0 });
  // node returns a Timeout object (coerces to the numeric id for clear*) with
  // ref/unref/hasRef/refresh/close; many tests call setInterval(...).unref().
  const findT = (id) => T.q.find((x) => x.id === id);
  const mkTimer = (id) => ({ _id: id, [Symbol.toPrimitive]: () => id,
    ref() { const it = findT(id); if (it) it.refd = true; return this; },
    unref() { const it = findT(id); if (it) it.refd = false; return this; },
    hasRef() { const it = findT(id); return it ? !!it.refd : false; },
    refresh() { const it = findT(id); if (it) it.at = Date.now() + (it.iv || it.d || 0); return this; },
    close() { G.clearTimeout(id); return this; }, [Symbol.dispose]() { G.clearTimeout(id); } });
  const timerId = (t) => (t && typeof t === "object" ? t._id : t);
  G.setTimeout = function (fn, delay) { const a = Array.prototype.slice.call(arguments, 2); const id = T.id++; const d = +delay || 0; T.q.push({ id: id, fn: fn, at: Date.now() + d, d: d, a: a, iv: 0, refd: true }); return mkTimer(id); };
  G.setInterval = function (fn, delay) { const a = Array.prototype.slice.call(arguments, 2); const id = T.id++; const d = +delay || 1; T.q.push({ id: id, fn: fn, at: Date.now() + d, d: d, a: a, iv: d, refd: true }); return mkTimer(id); };
  G.clearTimeout = function (t) { const id = timerId(t); for (let i = 0; i < T.q.length; i++) if (T.q[i].id === id) { T.q.splice(i, 1); return; } };
  G.clearInterval = G.clearTimeout;
  G.setImmediate = function (fn) { const a = Array.prototype.slice.call(arguments, 1); const id = T.id++; T.q.push({ id: id, fn: fn, at: 0, d: 0, a: a, iv: 0, refd: true }); return mkTimer(id); };
  G.clearImmediate = G.clearTimeout;
  // Fire up to `budget` DUE timers (earliest deadline first); returns the count
  // of timers still due right now (0 → the pump may sleep). Intervals
  // reschedule from the current time (bun Timer.zig update()). A per-call fire
  // budget plus a global fired cap bound runaway tight intervals.
  G.__mbun_drain_timers = function (budget) {
    let fired = 0; budget = budget || 100;
    while (T.q.length && fired < budget && T.fired < 200000) {
      const now = Date.now();
      let mi = -1; for (let i = 0; i < T.q.length; i++) if (T.q[i].at <= now && (mi === -1 || T.q[i].at < T.q[mi].at)) mi = i;
      if (mi === -1) break;  // nothing due yet — real time gates firing
      const t = T.q[mi]; T.now = now;
      if (t.iv > 0) t.at = now + t.iv; else T.q.splice(mi, 1);
      try { t.fn.apply(null, t.a); } catch (e) {}
      fired++; T.fired++;
    }
    const now2 = Date.now();
    let due = 0; for (let i = 0; i < T.q.length; i++) if (T.q[i].at <= now2) due++;
    return due;
  };
  // Ref'd timers only — the pumps' "does the loop still have work" gauge
  // (node/bun: unref'd timers don't keep the process alive).
  G.__mbun_timers_refd = function () { let n = 0; for (let i = 0; i < T.q.length; i++) if (T.q[i].refd) n++; return n; };
  // How long the pump may sleep NOW: 0 when a timer is due (keep pumping),
  // otherwise time-to-earliest-deadline capped so socket/child IO stays
  // responsive (2ms with live net activity, 25ms otherwise). Microtask chains
  // never span pump iterations (each eval boundary drains them fully), so
  // sleeping when nothing is due and net isn't progressing is safe.
  G.__mbun_pump_idle_ms = function () {
    const NET = G.__mbunNet;
    const inflight = NET && NET.pending > 0;
    if (inflight && NET.stall === 0) return 0;  // net actively progressing
    // The park below blocks in poll(POLLIN) over the live net fds (listeners,
    // sockets, the serve reactor's epoll fd — engine.inc → NN.wait), so fd
    // READABILITY wakes it instantly and the park can be long (libuv/uSockets
    // semantics: block until the next timer deadline or an fd event; an idle
    // `node server.js` used to burn a full core here, then ticked at 25ms).
    // Only non-readability work still needs a short tick: stalled in-flight
    // ops and sockets with an EAGAIN write backlog (both may be waiting for
    // WRITABILITY, which the park does not watch). Children never reach the
    // park (the pump's ioBusy gate) and workers ride a self-rearming 1ms
    // timer (node_worker.cppm armWorkerPump), so neither can be starved.
    let shortTick = 0;
    if (inflight) shortTick = 2;
    else if (NET && NET.items) {
      for (const it of NET.items) if (it._wq && it._wq.length) { shortTick = 2; break; }
    }
    const held = NET && (NET.serveActive > 0 || inflight ||
                         (NET.items && NET.items.size > 0));
    const LONG_PARK = 60000;
    if (!T.q.length) {
      if (!held) return 0;
      return shortTick || LONG_PARK;
    }
    const now = Date.now();
    let mn = Infinity; for (let i = 0; i < T.q.length; i++) if (T.q[i].at < mn) mn = T.q[i].at;
    const d = mn - now;
    if (d <= 0) return 0;
    // An unref'd timer cannot hold the loop open, so once nothing ref'd is left
    // and the net is idle the pump is only running out its IDLE_GRACE_ROUNDS
    // before exiting (engine.inc). Sleeping toward such a timer's deadline just
    // adds latency to every shutdown: elysia arms a 4m55s cache-GC timer and
    // unref's it (elysia src/sucrose.ts:629-636). The unref'd deadline still
    // gates the park whenever something else (net, a live socket) is keeping
    // the loop alive, so unref'd timers stay punctual.
    if (!held) {
      let refd = 0;
      for (let i = 0; i < T.q.length; i++) if (T.q[i].refd) { refd = 1; break; }
      if (!refd) return 0;
    }
    const cap = shortTick || LONG_PARK;
    return d > cap ? cap : d;
  };
  G.__mbun_timers_reset = function () { T.q = []; T.now = 0; T.fired = 0; };

  // ---- Headers (WHATWG, case-insensitive multi-map) ----
  if (typeof G.Headers === "undefined") {
    G.Headers = class Headers {
      constructor(init) {
        this._m = new Map();  // lowercased name → value string
        this._sc = [];        // Set-Cookie values kept individually (getSetCookie)
        this._names = new Map();  // lowercased → original-case name (bun HTTPHeaderMap wire name)
        if (init) {
          if (typeof init.forEach === "function" && !Array.isArray(init)) init.forEach((v, k) => this.append(k, v));
          else if (Array.isArray(init)) for (const [k, v] of init) this.append(k, v);
          else for (const k of Object.keys(init)) this.append(k, init[k]);
          if (init._sc && init._sc.length) this._sc = init._sc.slice();  // exact copy from another Headers
          if (init._names instanceof Map) for (const [lk, n] of init._names) if (!this._names.has(lk)) this._names.set(lk, n);
        }
      }
      append(k, v) { const lk = String(k).toLowerCase(); if (lk === "set-cookie") this._sc.push(String(v)); if (!this._names.has(lk)) this._names.set(lk, String(k)); const e = this._m.get(lk); this._m.set(lk, e == null ? String(v) : e + ", " + v); }
      set(k, v) { const lk = String(k).toLowerCase(); if (lk === "set-cookie") this._sc = [String(v)]; this._names.set(lk, String(k)); this._m.set(lk, String(v)); }
      get(k) { const v = this._m.get(String(k).toLowerCase()); return v == null ? null : v; }
      getAll(k) { return String(k).toLowerCase() === "set-cookie" ? this._sc.slice() : (this.has(k) ? [this.get(k)] : []); }
      getSetCookie() { return this._sc.slice(); }
      has(k) { return this._m.has(String(k).toLowerCase()); }
      delete(k) { const lk = String(k).toLowerCase(); if (lk === "set-cookie") this._sc = []; this._m.delete(lk); this._names.delete(lk); }
      forEach(cb, thisArg) { for (const [k, v] of this._m) cb.call(thisArg, v, k, this); }
      keys() { return this._m.keys(); }
      values() { return this._m.values(); }
      entries() { return this._m.entries(); }
      [Symbol.iterator]() { return this._m.entries(); }
      toJSON() { return Object.fromEntries(this._m); }
    };
  }

  // ---- TextEncoderStream / TextDecoderStream (transform-style, minimal) ----
  if (typeof G.TextEncoderStream === "undefined") {
    G.TextEncoderStream = class TextEncoderStream {
      constructor() {
        this.encoding = "utf-8"; const enc = new G.TextEncoder(); let ctrl, pending = "";
        this.readable = new G.ReadableStream({ start(c) { ctrl = c; } });
        this.writable = new G.WritableStream({
          write(chunk) {
            // A throwing toString() must error both sides of the transform
            // (WPT encode-bad-chunks): rethrow errors the writable, ctrl.error
            // errors the coupled readable.
            let str;
            try { str = String(chunk); } catch (e) { if (ctrl && ctrl.error) ctrl.error(e); throw e; }
            let input = pending + str; pending = "";
            if (input.length) { const last = input.charCodeAt(input.length - 1); if (last >= 0xd800 && last <= 0xdbff) { pending = input.slice(-1); input = input.slice(0, -1); } }
            const bytes = enc.encode(input); if (bytes.length) ctrl.enqueue(bytes);
          },
          close() { if (pending) ctrl.enqueue(enc.encode(pending)); ctrl.close && ctrl.close(); }
        });
      }
    };
  }
  if (typeof G.TextDecoderStream === "undefined") {
    G.TextDecoderStream = class TextDecoderStream {
      constructor(label, opts) {
        opts = opts == null ? {} : opts;
        const dec = new G.TextDecoder(label === undefined ? "utf-8" : label, { fatal: !!opts.fatal, ignoreBOM: !!opts.ignoreBOM });
        this.encoding = dec.encoding; this.fatal = dec.fatal; this.ignoreBOM = dec.ignoreBOM; let ctrl;
        this.readable = new G.ReadableStream({ start(c) { ctrl = c; } });
        this.writable = new G.WritableStream({ write(chunk) { const s = dec.decode(chunk, { stream: true }); if (s) ctrl.enqueue(s); }, close() { const t = dec.decode(); if (t) ctrl.enqueue(t); ctrl.close && ctrl.close(); } });
      }
    };
  }

  // ---- Response (spec-shaped body consumption, incl. ReadableStream bodies) ----
  // formData() MIME gate. Port of bun_core/util.rs:5647 form_data::Encoding::get
  // and :5677 form_data::get_boundary. The old regex /boundary=("?)([^";]+)\1/i
  // both over- and under-matched: it accepted a `boundary=` substring belonging to
  // ANOTHER parameter (`xboundary=FAKE`), and it never checked the media type at
  // all, so ANY content-type fell through to __mbunParseFormData's URL-encoded
  // branch -- `new Request(url, {body: "foobarbaz"})` (content-type
  // text/plain;charset=UTF-8) silently decoded to [["foobarbaz", ""]] instead of
  // rejecting. Nothing threw; formData() just quietly returned the wrong answer.

  // Index of the next `;` in `s` not inside an RFC 7230 quoted-string (`\`
  // escapes the following byte inside quotes). Port of :5706
  // index_of_unquoted_semicolon.
  const indexOfUnquotedSemicolon = (s) => {
    let inQuotes = false;
    for (let i = 0; i < s.length; i++) {
      const c = s[i];
      if (c === '"') inQuotes = !inQuotes;
      else if (c === "\\" && inQuotes) i += 1;
      else if (c === ";" && !inQuotes) return i;
    }
    return -1;
  };
  // Borrow the `boundary=` value out of a Content-Type. Parameters are
  // `;`-delimited per RFC 7231 and the parameter NAME must be exactly `boundary`
  // (case-insensitive), so `xboundary=FAKE` is not picked up. The value is matched
  // byte-exact -- the delimiter in the body must equal it verbatim, so the
  // original (non-lowercased) header is what we scan. undefined => malformed.
  const getFormDataBoundary = (ct) => {
    let rest = ct;
    for (;;) {
      const semi = indexOfUnquotedSemicolon(rest);
      if (semi < 0) return undefined;
      rest = rest.slice(semi + 1);
      const param = rest.replace(/^[ \t]+/, "");
      const eq = param.indexOf("=");
      if (eq < 0) continue;
      if (param.slice(0, eq).toLowerCase() !== "boundary") continue;
      const begin = param.slice(eq + 1);
      if (begin.length === 0) return undefined;
      let end = begin.indexOf(";");
      if (end < 0) end = begin.length;
      if (begin[0] === '"') {
        if (end > 1 && begin[end - 1] === '"') return begin.slice(1, end - 1);
        return undefined;  // opening quote, no closing quote -- malformed
      }
      return begin.slice(0, end);
    }
  };
  // null => "incorrect MIME type/boundary". RFC 2045 5.1 / RFC 7231 3.1.1.1: the
  // media type is case-insensitive, and bun matches it with an UNANCHORED
  // case-insensitive substring search (contains_case_insensitive_ascii), not a
  // prefix/parse -- mirrored here.
  const formDataEncodingFromCT = (ct) => {
    if (ct === null || ct === undefined) return null;
    const lc = ct.toLowerCase();
    if (lc.includes("application/x-www-form-urlencoded")) return { urlencoded: true };
    if (!lc.includes("multipart/form-data")) return null;
    const boundary = getFormDataBoundary(ct);
    if (boundary === undefined) return null;
    return { urlencoded: false, boundary };
  };
  const formDataEncodingForHeaders = (headers) =>
    formDataEncodingFromCT(headers && headers.get ? headers.get("content-type") : null);
  // Body.rs:2068-2077 -- ErrorCode::FORMDATA_PARSE_ERROR is `instanceof TypeError`
  // and carries code ERR_FORMDATA_PARSE_ERROR (jsc/bindings/ErrorCode.ts:77).
  // Message is verbatim; bun REJECTS with it rather than throwing.
  const formDataMimeError = () => {
    const e = new TypeError("Can't decode form data from body because of incorrect MIME type/boundary");
    e.code = "ERR_FORMDATA_PARSE_ERROR";
    return e;
  };
  // Parse an already-gated body. Failures out of the multipart parser surface as
  // Body.rs:2114 `FormData parse error {name}` -- deliberately NOT the wording
  // FormData.from uses for the same failure (FormData.rs:197), so the wrapping
  // stays at the caller instead of inside the shared parser.
  const formDataParseBody = (u8, enc) => {
    try {
      return G.__mbunParseFormData(u8, enc.urlencoded ? undefined : enc.boundary);
    } catch (e) {
      if (e && e.__mbunFormDataErrName) {
        const err = new TypeError("FormData parse error " + e.__mbunFormDataErrName);
        err.code = "ERR_FORMDATA_PARSE_ERROR";
        throw err;
      }
      throw e;
    }
  };
  const isStream = (b) => G.__mbunStreams && G.__mbunStreams.isReadableStream(b);
  // Fetch "body unusable": the body is non-null and its stream is disturbed or
  // locked (https://fetch.spec.whatwg.org/#body-unusable). A byte body is never
  // unusable until it has actually been consumed, and a body that was never
  // there cannot be disturbed at all — bun's body_stream_check only reports true
  // for Used / Locked, falling through to `_ => false` for Empty/Null, so
  // `new Response(null).text()` leaves bodyUsed false and still permits clone().
  // `byteBody` is the _b/_body slot; undefined there with no stream means "no
  // body at all" (note "" IS a body).
  // Blueprint: bun-ref/src/runtime/webcore/Body.rs:1835 body_stream_check.
  const hasBody = (byteBody, stream) => byteBody !== undefined || isStream(stream);
  const bodyDisturbed = (byteBody, used, stream) =>
    hasBody(byteBody, stream) &&
    !!(used || (isStream(stream) && (G.__mbunStreams.isDisturbed(stream) || stream.locked)));
  // Step 1 of both clone() algorithms. bodyUsed uses is_disturbed alone; clone()
  // additionally rejects a locked (reader-held) stream — same check, wider
  // predicate. Blueprint: Body.rs:1860 get_body_used / :1867 throw_if_body_unusable
  // (message is verbatim; bun raises it with ErrorCode::BODY_ALREADY_USED).
  const throwIfBodyUnusable = (byteBody, used, stream) => {
    if (bodyDisturbed(byteBody, used, stream) || (isStream(stream) && stream.locked)) {
      const e = new TypeError("Body is disturbed or locked");
      e.code = "ERR_BODY_ALREADY_USED";     // ErrorCode.rs:72 -> ErrorCode.ts:23 maps this code to TypeError
      throw e;
    }
  };
  // Blueprint: Body.rs:2208 handle_body_already_used. Every reader (get_text
  // :1784/:1793/:1805, get_json :1884.., get_array_buffer, get_bytes,
  // get_form_data, get_blob) checks the BODY's own used/locked state and
  // `.reject()`s with this — it never throws synchronously, and it never
  // reaches the stream. Letting a used body fall through to the stream's own
  // consumer instead surfaces the STREAM-level message
  // ("Invalid state: ReadableStream has already been used", ERR_INVALID_STATE),
  // which bun raises only for a direct `response.body.text()`. Same ErrorCode
  // as throw_if_body_unusable, deliberately different message: clone() says
  // "Body is disturbed or locked", readers say "Body already used".
  const bodyAlreadyUsed = () => { const e = new TypeError("Body already used"); e.code = "ERR_BODY_ALREADY_USED"; return e; };
  // Fetch spec / Body.rs:1009: a stream handed in as a body init must be
  // neither disturbed nor locked. This one is a plain throw_type_error with no
  // ErrorCode, and its message is distinct from both of the above.
  const throwIfBodyInitUnusable = (s) => {
    if (isStream(s) && (G.__mbunStreams.isDisturbed(s) || s.locked))
      throw new TypeError("Body object should not be disturbed or locked");
  };
  // Status validation lives in ONE place: the ResponseInit parsing path
  // (Response.rs:1420 Init::init), and only fires when `status` is present.
  // `Response.error()` is legal at status 0 precisely because construct_error
  // (Response.rs:1140) builds the struct directly and never parses an init;
  // clone_value and construct_redirect's default do the same. RAW_STATUS is the
  // JS stand-in for "construct the struct directly" — a module-private symbol
  // user code cannot forge, so the public `status` path stays validated.
  const RAW_STATUS = Symbol("mbun.rawStatus");
  // JSC coerce_to_int64 (Response.rs:1421): ToNumber, truncate toward zero, and
  // saturate anything not representable as i64 (NaN / +-Infinity / |n| >= 2^63)
  // to i64::MIN — which the RangeError then echoes back verbatim. A string that
  // fails to parse coerces to 0 rather than NaN.
  const I64_MIN = -9223372036854775808;
  const toInt64 = (v) => {
    const n = Number(v);
    if (Number.isNaN(n)) return typeof v === "string" ? 0 : I64_MIN;
    if (!Number.isFinite(n) || n >= 9223372036854775808 || n < I64_MIN) return I64_MIN;
    return Math.trunc(n);
  };
  // The message echoes the coerced i64 verbatim, so it must be formatted as an
  // i64 and not as a double: JS prints -(2**63) as "-9223372036854776000"
  // (shortest round-trip), where Rust's i64 Display prints the exact
  // "-9223372036854775808". Every other reachable value formats identically.
  const i64Str = (n) => (n === I64_MIN ? "-9223372036854775808" : String(n));
  const validateStatus = (v) => {
    const n = toInt64(v);
    if ((n >= 200 && n < 600) || n === 101) return n;   // 101 is a single special case, not a range
    throw new RangeError("The status provided (" + i64Str(n) + ") must be 101 or in the range of [200, 599]");
  };
  // Response.rs:1045 validate_redirect_status_code. Note the message does NOT
  // interpolate the offending status, unlike the init-path message above.
  const validateRedirectStatus = (n) => {
    if (n === 301 || n === 302 || n === 303 || n === 307 || n === 308) return n;
    throw new RangeError("Failed to execute 'redirect' on 'Response': Invalid status code");
  };
  // Body -> ReadableStream. Streams pass through (throwing if already used);
  // async/sync iterables stream each yield; everything else is a single chunk.
  const bodyToStream = (body) => {
    if (isStream(body)) {
      throwIfBodyInitUnusable(body);   // Body.rs:1009 — disturbed OR locked, one message
      return body;
    }
    if (body == null) return null;
    // FormData is iterable: without this guard it hits the generic iterable
    // branch below and each [k, v] entry is mangled into bytes. The constructors
    // normalize it first (so the content-type boundary matches); this covers any
    // other caller.
    if (G.FormData && body instanceof G.FormData && G.__mbunSerializeFormData) body = G.__mbunSerializeFormData(body).u8;
    // URLSearchParams / non-Uint8Array TypedArray views / ArrayBuffer are all
    // iterable-or-buffer-like; without these guards they fall into the generic
    // iterable branch below and get mangled (each element re-wrapped as bytes).
    if (G.URLSearchParams && body instanceof G.URLSearchParams) body = body.toString();
    else if (ArrayBuffer.isView(body) && !(body instanceof Uint8Array)) {
      const u8 = new Uint8Array(body.buffer, body.byteOffset, body.byteLength);
      return new G.ReadableStream({ start(c) { c.enqueue(u8); c.close(); } });
    } else if (body instanceof ArrayBuffer) {
      const u8 = new Uint8Array(body);
      return new G.ReadableStream({ start(c) { c.enqueue(u8); c.close(); } });
    }
    if (typeof body !== "string" && !(body instanceof Uint8Array) && !(body && body._u8) && (typeof body[Symbol.asyncIterator] === "function" || typeof body[Symbol.iterator] === "function")) {
      let it = null;
      const iter = body;
      return new G.ReadableStream({
        async pull(c) {
          if (it === null) it = iter[Symbol.asyncIterator] ? iter[Symbol.asyncIterator]() : iter[Symbol.iterator]();
          try {
            const { value, done } = await it.next();
            if (done) { c.close(); return; }
            c.enqueue(typeof value === "string" ? te.encode(value) : (value instanceof Uint8Array ? value : new Uint8Array(value)));
          } catch (e) { c.error(e); }
        },
      });
    }
    return new G.ReadableStream({ start(c) { c.enqueue(typeof body === "string" ? te.encode(body) : (body instanceof Uint8Array ? body : (body && body._u8) ? new Uint8Array(body._u8) : te.encode(String(body)))); c.close(); } });
  };
  // bun bun_core::fmt SizeFormatter (fmt.rs:2614-2660): 0 -> "0 KB", <512 -> "N bytes",
  // else nv = v/1000^mag (mag = min(floor(log2 v)/9, 8)), 1 decimal if near-integer.
  const __mbunFmtSize = (v) => {
    if (!(v > 0)) return "0 KB";
    if (v < 512) return v + " bytes";
    const MAGS = " KMGTPEZY";
    const mag = Math.min(Math.floor(Math.floor(Math.log2(v)) / 9), 8);
    const nv = v / Math.pow(1000, mag);
    const prec = Math.abs(nv - Math.trunc(nv)) <= 0.1 ? 1 : 2;
    return nv.toFixed(prec) + " " + MAGS[mag] + "B";
  };
  if (typeof G.Response === "undefined") {
    G.Response = class Response {
      constructor(body, init) {
        // Response.rs:1238-1247 — an init that is present but not an object (nor
        // null/undefined) is a TypeError, not a silently-ignored default:
        // `new Response("", 0)` must throw. Functions pass (bun uses is_object()).
        if (init != null && typeof init !== "object" && typeof init !== "function")
          throw new TypeError("Failed to construct 'Response': The provided body value is not of type 'ResponseInit'");
        // RAW_STATUS = "built as a struct, not parsed from an init" (error() /
        // redirect() / json(data, number) / clone). Everything else goes through
        // Init::init's range check — Response.rs:1420.
        const raw = init ? init[RAW_STATUS] : undefined;
        this.status = raw !== undefined ? raw
          : (init && init.status !== undefined) ? validateStatus(init.status) : 200;
        this.ok = this.status >= 200 && this.status <= 299;   // Response.rs:580 is_ok
        this.headers = new G.Headers(init ? init.headers : undefined);
        // Response.rs:1440 uses fast_get_truthy, which despite the name skips
        // only undefined/null and then ToStrings whatever remains — verified
        // against bun-rust: statusText 0 -> "0", false -> "false", NaN -> "NaN".
        // `|| ""` dropped those on the floor, and left non-strings unconverted.
        const st = init ? init.statusText : undefined;
        this.statusText = st === undefined || st === null ? "" : String(st);
        this.redirected = false;                              // Response.rs:604 get_redirected
        this._url = "";                                       // Response.rs:206 — url defaults to ""
        this._used = false;
        const norm = G.__mbunNormalizeBody ? G.__mbunNormalizeBody(body) : { body, contentType: null };
        body = norm.body;
        if (norm.contentType && !this.headers.has("content-type")) this.headers.set("content-type", norm.contentType);
        this._b = (body != null && !isStream(body) && (typeof body === "string" || body instanceof Uint8Array || (body && body._u8))) ? body : undefined;
        this._stream = bodyToStream(body);
      }
      [Symbol.for("nodejs.util.inspect.custom")]() {
        // bun Response.rs:687-801 write_format + Body.rs FileRef/Blob (Blob.rs:1113-1150).
        const q = (s) => JSON.stringify(String(s));
        const b = this._b;
        const len = b ? (b.size || (b._u8 ? b._u8.byteLength : 0)) : 0;
        let out = "Response (" + __mbunFmtSize(len) + ") {\n";
        out += "  ok: " + this.ok + ",\n";
        out += "  url: " + q(this._url || "") + ",\n";
        out += "  status: " + this.status + ",\n";
        out += "  statusText: " + q(this.statusText) + ",\n";
        out += "  headers: Headers {\n";
        for (const [k, v] of this.headers.entries()) out += "    " + q(k) + ": " + q(v) + ",\n";
        out += "  },\n";
        out += "  redirected: " + this.redirected + ",\n";
        out += "  bodyUsed: " + this.bodyUsed;
        if (b && b.__isBunFile) out += ",\n  FileRef (" + q(b.__name) + ") {\n    type: " + q(b.type || "") + "\n  }";
        else if (b && b._u8) out += ",\n  Blob (" + __mbunFmtSize(b.size || b._u8.byteLength) + ") {\n    type: " + q(b.type || "") + "\n  }";
        out += "\n}";
        return out;
      }
      get body() { return this._stream || null; }
      // Disturbance is a property of the stream, not a flag we set: reading via
      // response.body.getReader() must flip bodyUsed too. Blueprint: Body.rs:1860
      // get_body_used = body_stream_check(ReadableStream::is_disturbed).
      get bodyUsed() { return bodyDisturbed(this._b, this._used, this._stream); }
      _consume(kind) {
        // Body.rs:1784 — the body's own state is checked BEFORE the stream, and
        // rejects rather than throws. `bodyDisturbed` keeps the Empty/Null
        // distinction: `new Response(null).text()` is repeatable (no body to
        // disturb), `new Response("").text()` is not ("" IS a body).
        if (bodyDisturbed(this._b, this._used, this._stream) || (isStream(this._stream) && this._stream.locked))
          return Promise.reject(bodyAlreadyUsed());
        // Body.rs:2038-2077 get_form_data resolves the encoder BEFORE it touches
        // the body (use_as_any_blob), and AFTER the used/locked check -- so an
        // already-used body reports "Body already used" (that check is above),
        // while a bad MIME type leaves bodyUsed false and the body still readable.
        // Verified against bun-rust 1.4.0: text() after a MIME-rejected formData()
        // still returns the body. Gating inside the .then() below would consume it.
        let fdEncoding;
        if (kind === "formData") {
          fdEncoding = formDataEncodingForHeaders(this.headers);
          if (fdEncoding === null) return Promise.reject(formDataMimeError());
        }
        this._used = true;
        const b = this._b;
        if (this._stream && isStream(this._stream)) {
          const S = G.__mbunStreams;
          if (kind === "text") return S.text(this._stream);
          if (kind === "bytes") return S.bytes(this._stream);
          if (kind === "arrayBuffer") return S.arrayBuffer(this._stream);
          if (kind === "blob") return S.array(this._stream).then((cs) => new G.Blob(cs, { type: (this.headers.get && this.headers.get("content-type")) || "" }));
          if (kind === "formData") return S.bytes(this._stream).then((u8) => formDataParseBody(u8, fdEncoding));
        }
        // non-stream bodies
        // Body.rs materializations are capped by the synthetic allocation limit
        // (see G.__mbunCheckAllocLimit); arrayBuffer() goes through "bytes" in
        // bun too but ArrayBuffer is exempt there, so it re-reads without a cap.
        if (kind === "text") { if (b == null) return Promise.resolve(""); if (typeof b === "string") return Promise.resolve(b); if (b instanceof Uint8Array) { G.__mbunCheckAllocLimit(b.length, "text"); return Promise.resolve(td.decode(b)); } if (typeof b.text === "function") return b.text(); return Promise.resolve(String(b)); }
        if (kind === "bytes") { if (b instanceof Uint8Array) { G.__mbunCheckAllocLimit(b.length, "bytes"); return Promise.resolve(new Uint8Array(b)); } if (b && b._u8) { G.__mbunCheckAllocLimit(b._u8.length, "bytes"); return Promise.resolve(new Uint8Array(b._u8)); } return this._consume("text").then((t) => te.encode(t)); }
        if (kind === "arrayBuffer") {
          // Exempt from the allocation cap (bun: ArrayBuffer has no such limit),
          // so it must not route through the capped "bytes" branch.
          const u = b instanceof Uint8Array ? b : (b && b._u8 instanceof Uint8Array ? b._u8 : null);
          if (u) return Promise.resolve(u.buffer.slice(u.byteOffset, u.byteOffset + u.byteLength));
          return this._consume("bytes").then((u8) => u8.buffer.slice(u8.byteOffset, u8.byteOffset + u8.byteLength));
        }
        if (kind === "blob") return Promise.resolve(b instanceof G.Blob ? b : new G.Blob([b == null ? "" : b], { type: (this.headers.get && this.headers.get("content-type")) || "" }));
        if (kind === "formData") return this._consume("bytes").then((u8) => formDataParseBody(u8, fdEncoding));
      }
      text() { return this._consume("text"); }
      json() { return this._consume("text").then((t) => JSON.parse(t)); }
      arrayBuffer() { return this._consume("arrayBuffer"); }
      bytes() { return this._consume("bytes"); }
      blob() { return this._consume("blob"); }
      formData() { return this._consume("formData"); }
      // Blueprint: Body.rs:1591 clone_with_readable_stream. Handing the clone
      // `this._stream || this._b` shared ONE stream between both bodies, so the
      // second reader got "already used" — and it silently demoted a byte body to
      // a stream body, which is why served static routes framed chunked instead
      // of Content-Length (the server picks framing off the same _b/_stream
      // discriminant: js_net.cppm:948, mirroring RequestContext.rs:3118).
      clone() {
        throwIfBodyUnusable(this._b, this._used, this._stream);   // spec step 1
        let r;
        if (this._b !== undefined) {
          // Byte body: hand over the same bytes; never tee. bun refcounts the
          // store (Blob::dupe_with_content_type / WTFStringImpl::ref) rather than
          // splitting a stream — Body.rs:1599-1615. Staying a byte body is what
          // keeps the clone Content-Length-framed and independently readable.
          r = new G.Response(this._b, { [RAW_STATUS]: this.status, statusText: this.statusText });
        } else if (isStream(this._stream)) {
          // Locked body: tee, keeping branch 0 as our own readable and giving the
          // clone branch 1 ("Keep the current readable as a strong reference when
          // cloning, and return the second one" — Body.rs:1479). The tee precedes
          // any byte extraction, per the ordering note at Body.rs:1596.
          const [mine, theirs] = this._stream.tee();
          this._stream = mine;
          r = new G.Response(null, { [RAW_STATUS]: this.status, statusText: this.statusText });
          r._stream = theirs;
        } else {
          r = new G.Response(null, { [RAW_STATUS]: this.status, statusText: this.statusText });
        }
        // clone_value copies the struct (Response.rs:830), so it must not go
        // back through init validation — `Response.error().clone()` carries
        // status 0. `ok` and `type` are derived from `status` (Response.rs:580 /
        // :592), so copying the status is what carries them; assigning `type`
        // here would throw, since its accessor has no setter (as in bun).
        r.headers = new G.Headers(this.headers);
        r._url = this._url;
        r.redirected = this.redirected;
        return r;
      }
      // Response.json defaults the content-type but must not clobber one the
      // caller passed in init.headers: bun uses put_default (set-if-absent), not
      // a plain set — Response.rs:1031 construct_json. The value carries the
      // charset: MimeType.rs:297 JSON = "application/json;charset=utf-8".
      // Response.rs:1015: a NUMBER second arg skips ResponseInit parsing and
      // CLAMPS to [0, 65535] instead of validating — `Response.json({}, 0)` is
      // legal and has type "error", while `Response.json({}, {status: 0})`
      // throws. Divergent behaviour between the two arg shapes, deliberately.
      static json(data, init) {
        let __body;
        try { __body = JSON.stringify(data); }
        catch (e) {
          if (e instanceof TypeError && /BigInt/.test(e.message))
            throw new TypeError("Do not know how to serialize a BigInt");
          throw e;
        }
        if (__body === undefined) throw new TypeError("Value is not JSON serializable");
        const r = typeof init === "number"
          ? new G.Response(__body, { [RAW_STATUS]: Math.min(Math.max(0, init | 0), 65535) })
          : new G.Response(__body, init);
        if (!r.headers.has("content-type")) r.headers.set("content-type", "application/json;charset=utf-8");
        return r;
      }
      // Response.rs:1140 construct_error — builds the struct directly with
      // status_code 0 and BodyValue::Empty. This is the whole reason status 0
      // is reachable: it never touches the [200,599]/101 check.
      static error() {
        const r = new G.Response(null, { [RAW_STATUS]: 0 });
        // construct_error uses BodyValue::Empty (Response.rs:1148), NOT Null:
        // an *empty* body, which surfaces as an already-closed ReadableStream.
        // `new Response(null)` is BodyValue::Null and surfaces as `body === null`.
        // Both read as "" and both leave bodyUsed false; only `.body` differs.
        r._stream = new G.ReadableStream({ start(c) { c.close(); } });
        return r;
      }
      // Response.rs:1078 construct_redirect_impl. Default 302. A NUMBER status
      // is validated against the redirect set (to_int32 first). An OBJECT is
      // parsed as a full ResponseInit — so [200,599]/101 applies — and is only
      // re-checked against the redirect set `if status != 200`. That guard is
      // why `Response.redirect(u, {status: 200})` keeps 200, and why a bare
      // `Response.redirect(u, {})` yields 200 rather than the 302 default:
      // Init::init's own default (200) replaces it. Any other type (string,
      // boolean, null, undefined) leaves the 302 default alone.
      static redirect(url, status) {
        let init;
        if (typeof status === "number") init = { [RAW_STATUS]: validateRedirectStatus(status | 0) };
        else if (status !== null && status !== undefined && typeof status === "object") {
          const s = status.status !== undefined ? validateStatus(status.status) : 200;
          init = { [RAW_STATUS]: s !== 200 ? validateRedirectStatus(s) : 200,
                   headers: status.headers, statusText: status.statusText };
        } else init = { [RAW_STATUS]: 302 };
        const r = new G.Response(null, init);
        // Response.rs:1133: Location carries the SERIALIZATION of the parsed
        // url (href), not the raw input — but a non-absolute input keeps its
        // raw string, which is documented Bun behaviour, not an oversight.
        const raw = url === undefined ? "" : String(url);
        let href = raw;
        try { href = new G.URL(raw).href; } catch (e) { href = raw; }
        r.headers.set("location", href);
        return r;
      }
    };
    // `type` and `url` are prototype accessors, enumerable and non-configurable
    // (response.classes.ts:97/112 codegen), NOT own data properties.
    //
    // type: Response.rs:592 get_response_type. There is no ResponseType field
    // and no enum — the value is DERIVED from the status code, so only "error"
    // and "default" are reachable ("basic"/"cors"/"opaque"/"opaqueredirect" are
    // never produced by this blueprint). Deriving it is what makes the edges
    // fall out for free: status 101 is < 200, so `new Response(null,{status:101})
    // .type === "error"`, and clone() carries type simply by carrying status.
    Object.defineProperty(G.Response.prototype, "type", {
      get() { return this.status < 200 ? "error" : "default"; },
      enumerable: true, configurable: false,
    });
    // url: Response.rs:587 get_url, backed by a field defaulting to "" (:206).
    // bun's accessor is readonly (assigning throws) and is written from Rust via
    // Response::set_url (:321) on the fetch/server paths. Our fetch path is JS
    // (js_net.cppm:2203 `res.url = url`), so the setter is this port's stand-in
    // for set_url. Deliberate, documented deviation: `r.url = x` throws in bun
    // but assigns here; every read-side value matches.
    Object.defineProperty(G.Response.prototype, "url", {
      get() { return this._url === undefined ? "" : this._url; },
      set(v) { this._url = String(v); },
      enumerable: true, configurable: false,
    });
  }

  // ---- Bun.spawn / Bun.spawnSync (sync under the hood; correct final results.
  // Async streaming / kill-mid-run DEFERRED — no host loop) ----
  const CPN = globalThis.__mbunCpNative;
  const spawnArgs = (a, b) => {
    let cmd, opts;
    if (Array.isArray(a)) { cmd = a; opts = b || {}; }
    else { opts = a || {}; cmd = opts.cmd || []; }
    // Reject a spoofed/oversized length before mapping (a 4G-length array would
    // otherwise iterate for billions of steps). Matches bun's early guard.
    if (cmd && cmd.length > 0x100000) throw new RangeError("cmd array is too large");
    const mapped = cmd.map(toStr);
    // Null-byte injection guard: the command, each arg, and every env key/value
    // must be NUL-free (a NUL would truncate the C string passed to exec/env).
    const nul = (s, where) => { if (String(s).indexOf("\0") >= 0) { const e = new TypeError('The "' + where + '" argument must be a string without null bytes.'); e.code = "ERR_INVALID_ARG_VALUE"; throw e; } };
    for (let i = 0; i < mapped.length; i++) nul(mapped[i], i === 0 ? "cmd" : "args[" + i + "]");
    if (opts && opts.env && typeof opts.env === "object") for (const k of Object.keys(opts.env)) { nul(k, "env"); const v = opts.env[k]; if (v != null) nul(v, "env"); }
    return { cmd: mapped, opts };
  };
  const runNative = (cmd, opts) =>
    CPN.spawnSync(cmd[0], cmd.slice(1), { cwd: opts.cwd ? toStr(opts.cwd) : undefined,
      input: typeof opts.stdin === "string" ? opts.stdin : undefined,
      env: opts.env && typeof opts.env === "object" ? opts.env : undefined });
  const bunBody = (data) => ({
    text: () => Promise.resolve(data),
    json: () => Promise.resolve(JSON.parse(data)),
    arrayBuffer: () => Promise.resolve(te.encode(data).buffer),
    bytes: () => Promise.resolve(te.encode(data)),
    blob: () => Promise.resolve(new G.Blob([data])),
    // Bun.spawn stdout/stderr is a ReadableStream: yield the captured buffer once.
    getReader() { let done = false; const chunk = te.encode(data); return { read() { if (done || chunk.length === 0) { return Promise.resolve({ value: undefined, done: true }); } done = true; return Promise.resolve({ value: chunk, done: false }); }, releaseLock() {}, cancel() { return Promise.resolve(); }, closed: Promise.resolve() }; },
    pipeTo() { return Promise.resolve(); }, cancel() { return Promise.resolve(); }, tee() { return [bunBody(data), bunBody(data)]; },
    async *[Symbol.asyncIterator]() { if (data) yield te.encode(data); },
  });
  // Binary-safe byte<->base64 bridges for __mbunProcNative (bytes cross the JS
  // boundary as base64 so pipes carry arbitrary binary, e.g. bun:jsc payloads).
  const PN = globalThis.__mbunProcNative;
  const anyToU8 = (d) => (typeof d === "string" ? te.encode(d) : d instanceof Uint8Array ? d : ArrayBuffer.isView(d) ? new Uint8Array(d.buffer, d.byteOffset, d.byteLength) : d instanceof ArrayBuffer ? new Uint8Array(d) : te.encode(String(d)));
  const u8ToB64 = (d) => { const u = anyToU8(d); let s = ""; for (let i = 0; i < u.length; i += 4096) s += String.fromCharCode.apply(null, u.subarray(i, i + 4096)); return G.btoa(s); };
  const b64ToU8 = (b) => { const s = G.atob(b); const u = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) u[i] = s.charCodeAt(i); return u; };
  const SIGNAMES = { 1: "SIGHUP", 2: "SIGINT", 3: "SIGQUIT", 6: "SIGABRT", 9: "SIGKILL", 13: "SIGPIPE", 15: "SIGTERM" };
  if (globalThis.Bun) {
    const Bun = globalThis.Bun;
    // Async Bun.spawn with real pipes: used when stdin is "pipe" (interactive
    // duplex protocols). Reads are blocking natives surfaced as resolved promises.
    const spawnPipes = (cmd, opts) => {
      const h = PN.spawn(cmd[0], cmd.slice(1), { cwd: opts.cwd ? toStr(opts.cwd) : undefined, env: opts.env && typeof opts.env === "object" ? opts.env : undefined });
      const pipeStream = (fd) => {
        let eof = false;
        const readChunk = () => { if (eof) return null; const b = PN.read(fd, 65536); if (b === null) { eof = true; try { PN.close(fd); } catch (e) {} return null; } return b64ToU8(b); };
        const drain = () => { const cs = []; for (;;) { const c = readChunk(); if (!c) break; cs.push(c); } let t = 0; for (const c of cs) t += c.length; const out = new Uint8Array(t); let o = 0; for (const c of cs) { out.set(c, o); o += c.length; } return out; };
        // text()/bytes() return lazy thenables: draining only starts when the
        // caller awaits, so grabbing `proc.stderr.text()` at spawn time is safe.
        const lazy = (make) => ({ then(res, rej) { try { return Promise.resolve(make()).then(res, rej); } catch (e) { return Promise.reject(e).then(res, rej); } } });
        return {
          getReader() { return { read: () => { try { const c = readChunk(); return Promise.resolve(c ? { value: c, done: false } : { value: undefined, done: true }); } catch (e) { return Promise.reject(e); } }, releaseLock() {}, closed: Promise.resolve(), cancel() { eof = true; try { PN.close(fd); } catch (e) {} return Promise.resolve(); } }; },
          text: () => lazy(() => td.decode(drain())),
          bytes: () => lazy(drain),
          arrayBuffer: () => lazy(() => drain().buffer),
          json: () => lazy(() => JSON.parse(td.decode(drain()))),
          async *[Symbol.asyncIterator]() { for (;;) { const c = readChunk(); if (!c) return; yield c; } },
        };
      };
      const st = { done: false };
      const proc = {
        pid: h.pid, exitCode: null, signalCode: null,
        stdin: {
          write(d) { try { PN.write(h.in, u8ToB64(d)); return true; } catch (e) { return false; } },
          flush() {}, end() { try { PN.close(h.in); } catch (e) {} }, close() { this.end(); },
        },
        stdout: pipeStream(h.out), stderr: pipeStream(h.err),
        exited: { then(res, rej) { try { proc._reap(false); return Promise.resolve(proc.exitCode).then(res, rej); } catch (e) { return Promise.reject(e).then(res, rej); } } },
        _reap(nohang) { if (st.done) return; const w = PN.wait(h.pid, !!nohang); if (w.exited) { st.done = true; proc.exitCode = w.code; proc.signalCode = w.signal ? SIGNAMES[w.signal] || "SIG" + w.signal : null; } },
        kill(sig) { try { PN.kill(h.pid, typeof sig === "number" ? sig : 15); } catch (e) {} try { PN.close(h.in); } catch (e) {} try { proc._reap(false); } catch (e) {} },
        ref() {}, unref() {}, resourceUsage() { return {}; },
        [Symbol.dispose]() { this.kill(); },
        [Symbol.asyncDispose]() { this.kill(); return Promise.resolve(); },
      };
      return proc;
    };
    // Fully async Bun.spawn over spawnEx + the io_tick pump: real live process
    // that can be killed mid-run, with lazy stdout/stderr bodies and an `exited`
    // promise resolved when the child is reaped. Used when stdin is a keyword/fd
    // (not "pipe" — that stays on the duplex spawnPipes path, and not raw stdin
    // data — that stays on the sync runNative path).
    const stdinIsKeyword = (v) => v == null || v === "ignore" || v === "inherit" || typeof v === "number";
    const bunMapSig = (sig) => {
      if (sig === undefined || sig === null || sig === "") return 15;
      if (typeof sig === "number") { if (Number.isNaN(sig)) return 15; if (!isFinite(sig)) throw new RangeError("Unknown signal: " + sig); return sig; }
      if (typeof sig === "string") { if (SIGMAP[sig] == null) { const e = new TypeError("signal must be one of 'SIGHUP', 'SIGINT', 'SIGQUIT', 'SIGILL', 'SIGTRAP', 'SIGABRT', 'SIGBUS', 'SIGFPE', 'SIGKILL', 'SIGUSR1', 'SIGSEGV', 'SIGUSR2', 'SIGPIPE', 'SIGALRM', 'SIGTERM', 'SIG16', 'SIGCHLD', 'SIGCONT', 'SIGSTOP', 'SIGTSTP', 'SIGTTIN', 'SIGTTOU', 'SIGURG', 'SIGXCPU', 'SIGXFSZ', 'SIGVTALRM', 'SIGPROF', 'SIGWINCH', 'SIGIO', 'SIGPWR' or 'SIGSYS'"); e.code = "ERR_INVALID_ARG_TYPE"; throw e; } return SIGMAP[sig]; }
      throw new TypeError("Invalid signal: " + String(sig));
    };
    const makeBunReadable = () => {
      // Incremental readable driven by __mbun_io_tick: __data/__end wake every
      // pending waiter, so getReader().read() and for-await yield each chunk as
      // it arrives (duplex pipe protocols), while text()/bytes() wait for EOF.
      const chunks = []; let ended = false; const waiters = [];
      const wake = () => { while (waiters.length) waiters.shift()(); };
      const waitEvent = () => new Promise((r) => waiters.push(r));
      const whenDone = async () => { while (!ended) await waitEvent(); };
      const nextChunk = async (i) => { while (i >= chunks.length && !ended) await waitEvent(); return i < chunks.length ? chunks[i] : undefined; };
      const all = () => { let t = 0; for (const c of chunks) t += c.length; const o = new Uint8Array(t); let p = 0; for (const c of chunks) { o.set(c, p); p += c.length; } return o; };
      const body = () => whenDone().then(all);
      return {
        __data: (bytes) => { chunks.push(bytes); wake(); },
        __end: () => { ended = true; wake(); },
        text: () => body().then((u) => td.decode(u)),
        bytes: () => body(),
        arrayBuffer: () => body().then((u) => u.buffer),
        blob: () => body().then((u) => new G.Blob([u])),
        json: () => body().then((u) => JSON.parse(td.decode(u))),
        getReader() { let i = 0; return { read: () => nextChunk(i).then((c) => (c === undefined ? { value: undefined, done: true } : (i++, { value: c, done: false }))), releaseLock() {}, cancel() { return Promise.resolve(); }, closed: whenDone() }; },
        pipeTo() { return whenDone(); }, cancel() { return Promise.resolve(); },
        async *[Symbol.asyncIterator]() { let i = 0; for (;;) { const c = await nextChunk(i); if (c === undefined) return; i++; yield c; } },
      };
    };
    const spawnAsyncBun = (cmd, opts) => {
      let sin, sout, serr;
      if (Array.isArray(opts.stdio)) { sin = opts.stdio[0]; sout = opts.stdio[1]; serr = opts.stdio[2]; }
      sin = opts.stdin != null ? opts.stdin : sin != null ? sin : "ignore";
      sout = opts.stdout != null ? opts.stdout : sout != null ? sout : "pipe";
      serr = opts.stderr != null ? opts.stderr : serr != null ? serr : "inherit";
      const norm = (v, d) => (v == null ? d : v === "pipe" || v === "ignore" || v === "inherit" || typeof v === "number" ? v : "ignore");
      const stdio = [norm(sin, "ignore"), norm(sout, "pipe"), norm(serr, "inherit")];
      const h = PN.spawnEx(cmd[0], cmd, { cwd: opts.cwd ? toStr(opts.cwd) : undefined, env: opts.env && typeof opts.env === "object" ? opts.env : (G.process && G.process.env) || undefined, stdio });
      if (h.errno != null) { const code = ERRNO[h.errno] || ("errno " + h.errno); const e = new Error("spawn " + cmd[0] + " " + code); e.code = code; e.errno = -1; e.syscall = "spawn " + cmd[0]; throw e; }
      let exitResolve; const exitedP = new Promise((r) => (exitResolve = r));
      const proc = { pid: h.pid, exitCode: null, signalCode: null, killed: false, exited: exitedP, ref() {}, unref() {}, resourceUsage() { return {}; } };
      const rec = { cp: null, pid: h.pid, outs: [], stdinFd: -1, stdinBuf: [], stdinEnded: false, stdinClosed: false, exited: false, closed: false, done: false, code: null, signal: null };
      rec.cp = { emit: (ev, code, signal) => {
        if (ev === "exit") { proc.exitCode = signal ? null : code; proc.signalCode = signal || null; }
        else if (ev === "close") { proc.exitCode = signal ? null : code; proc.signalCode = signal || null; exitResolve(proc.exitCode); }
      }, stdin: null };
      const fds = h.fds || [];
      const outStd = stdio[1] === "pipe" && fds[1] >= 0 ? makeBunReadable() : null;
      const errStd = stdio[2] === "pipe" && fds[2] >= 0 ? makeBunReadable() : null;
      // Same O_NONBLOCK requirement as the child_process driver above: drainOut's
      // readNB loop must see EAGAIN, not park the JS thread on a blocking read.
      if (outStd) { PN.setNonBlock(fds[1]); rec.outs.push({ fd: fds[1], stream: outStd, ended: false }); }
      if (errStd) { PN.setNonBlock(fds[2]); rec.outs.push({ fd: fds[2], stream: errStd, ended: false }); }
      proc.stdout = outStd || bunBody(""); proc.stderr = errStd || bunBody("");
      proc.stdin = null;
      if (stdio[0] === "pipe" && fds[0] >= 0) {
        // FileSink-flavoured stdin over the non-blocking write queue: bytes are
        // buffered on rec.stdinBuf and flushed by __mbun_io_tick (never blocks
        // the JS thread — an in-process Bun.serve keeps being pumped while a
        // child talks back to it, e.g. the bun-install dummy registry).
        PN.setNonBlock(fds[0]);
        rec.stdinFd = fds[0];
        const sink = {
          destroyed: false,
          write(d) { const u = anyToU8(d); rec.stdinBuf.push({ data: u, off: 0, cb: null }); return u.length; },
          flush() {},
          end() { rec.stdinEnded = true; return Promise.resolve(); },
          close() { this.end(); },
          destroy() { this.end(); },
          [Symbol.dispose]() { this.end(); },
        };
        proc.stdin = sink;
        rec.cp.stdin = sink;
      }
      proc.kill = function (sig) { const s = bunMapSig(sig); PN.kill(h.pid, s); this.killed = true; return true; };
      proc[Symbol.dispose] = function () { try { this.kill(); } catch (e) {} };
      proc[Symbol.asyncDispose] = function () { try { this.kill(); } catch (e) {} return exitedP; };
      CHILDREN.add(rec);
      return proc;
    };
    // Bun.spawn({terminal}) — run the child under a pseudo-terminal (Bun.Terminal).
    // The master fd is pumped by the same __mbun_io_tick reactor as spawnEx: reads
    // deliver child output to terminal.data(t, chunk); writes queue through the
    // non-blocking stdin path; child reap fires terminal.exit(). proc.terminal
    // exposes write/close/resize. Ref: bun-ref subprocess.rs get_terminal +
    // Terminal.rs (POSIX openpty; stdio 0/1/2 = slave).
    const spawnTerminal = (cmd, opts) => {
      const term = opts.terminal || {};
      const cols = typeof term.cols === "number" && term.cols > 0 ? term.cols : 80;
      const rows = typeof term.rows === "number" && term.rows > 0 ? term.rows : 24;
      const h = PN.spawnPty(cmd[0], cmd, { cwd: opts.cwd ? toStr(opts.cwd) : undefined, env: opts.env && typeof opts.env === "object" ? opts.env : (G.process && G.process.env) || undefined, cols, rows });
      if (h.errno != null) { const code = ERRNO[h.errno] || ("errno " + h.errno); const e = new Error("spawn " + cmd[0] + " " + code); e.code = code; e.errno = -1; e.syscall = "spawn " + cmd[0]; throw e; }
      let exitResolve; const exitedP = new Promise((r) => (exitResolve = r));
      const proc = { pid: h.pid, exitCode: null, signalCode: null, killed: false, exited: exitedP, ref() {}, unref() {}, resourceUsage() { return {}; } };
      const rec = { cp: null, pid: h.pid, outs: [], stdinFd: h.write, stdinBuf: [], stdinEnded: false, stdinClosed: false, exited: false, closed: false, done: false, code: null, signal: null };
      const terminalObj = {
        cols, rows,
        write(d) { const u = anyToU8(d); rec.stdinBuf.push({ data: u, off: 0, cb: null }); return u.length; },
        resize(c, r) { if (typeof PN.ptyResize === "function") PN.ptyResize(h.master, c | 0, r | 0); this.cols = c | 0; this.rows = r | 0; },
        flush() {},
        close() { rec.stdinEnded = true; },
        [Symbol.dispose]() { rec.stdinEnded = true; },
      };
      const ptyStream = {
        __data: (bytes) => { if (typeof term.data === "function") { try { term.data(terminalObj, bytes); } catch (e) {} } },
        __end: () => {},
      };
      PN.setNonBlock(h.master);
      rec.outs.push({ fd: h.master, stream: ptyStream, ended: false });
      rec.cp = { emit: (ev, code, signal) => {
        if (ev === "exit") { proc.exitCode = signal ? null : code; proc.signalCode = signal || null; }
        else if (ev === "close") {
          proc.exitCode = signal ? null : code; proc.signalCode = signal || null;
          if (typeof term.exit === "function") { try { term.exit(terminalObj, proc.exitCode, proc.signalCode); } catch (e) {} }
          exitResolve(proc.exitCode);
        }
      }, stdin: null };
      proc.terminal = terminalObj;
      proc.stdin = null; proc.stdout = null; proc.stderr = null;
      proc.kill = function (sig) { const s = bunMapSig(sig); PN.kill(h.pid, s); this.killed = true; return true; };
      proc[Symbol.dispose] = function () { try { this.kill(); } catch (e) {} };
      proc[Symbol.asyncDispose] = function () { try { this.kill(); } catch (e) {} return exitedP; };
      CHILDREN.add(rec);
      return proc;
    };
    Bun.spawn = function (a, b) {
      const s = spawnArgs(a, b);
      if (s.opts.terminal && PN && PN.spawnPty) return spawnTerminal(s.cmd, s.opts);
      // stdin: "pipe" rides the fully async spawnEx/io_tick path too — the old
      // spawnPipes path drains stdout/stderr with BLOCKING reads, which parks
      // the JS thread and starves the virtual event loop (deadlocking a child
      // that talks to an in-process Bun.serve, e.g. bun-install's registry).
      if (PN && PN.spawnEx && (stdinIsKeyword(s.opts.stdin) || s.opts.stdin === "pipe")) {
        return spawnAsyncBun(s.cmd, s.opts);
      }
      if (PN && PN.spawn && s.opts.stdin === "pipe") return spawnPipes(s.cmd, s.opts);
      const r = runNative(s.cmd, s.opts);
      return {
        pid: r.pid, exitCode: r.status, signalCode: r.signal,
        exited: Promise.resolve(r.status),
        stdout: bunBody(r.stdout), stderr: bunBody(r.stderr),
        stdin: { write() {}, end() {}, flush() {}, close() {} },
        kill() {}, ref() {}, unref() {}, resourceUsage() { return {}; },
        [Symbol.dispose]() { this.kill(); },
        [Symbol.asyncDispose]() { this.kill(); return Promise.resolve(); },
      };
    };
    // Bun.spawnSync's result object. Unlike the async Subprocess — whose
    // `signalCode` getter returns null when the child took no signal
    // (subprocess.rs:1339-1354) — the SYNC result only gets a `signalCode`
    // property when a signal actually killed the child, so with none it reads
    // back as `undefined`, not null (ref: bun-ref/src/runtime/api/bun/
    // js_bun_spawn_bindings.rs:1999-2002 — the put is behind
    // `if !signal_code.is_empty_or_undefined_or_null()`). The native reports
    // "no signal" as null (process_base.inc:149/:485).
    const syncResult = (r, stdout, stderr) => {
      const out = {
        pid: r.pid, exitCode: r.status, success: r.status === 0,
        stdout: stdout, stderr: stderr,
        resourceUsage() { return {}; },
      };
      // The signal is reported as its NAME when known ("SIGTERM"), falling back
      // to the raw number — get_signal_code returns `sys_sig.name()` and only
      // drops to `js_number(signal)` when the code has no name
      // (subprocess.rs:1339-1350). The native hands us the number as a string.
      if (r.signal !== null && r.signal !== undefined) {
        const n = Number(r.signal);
        out.signalCode = Number.isNaN(n) ? r.signal : (SIGNAMES[n] || n);
      }
      return out;
    };
    Bun.spawnSync = function (a, b) {
      const s = spawnArgs(a, b);
      // Binary-safe path: byte payload on stdin, exact bytes back from stdout.
      if (PN && PN.spawnSyncB64) {
        const stdin = s.opts.stdin;
        const r = PN.spawnSyncB64(s.cmd[0], s.cmd.slice(1), {
          cwd: s.opts.cwd ? toStr(s.opts.cwd) : undefined,
          env: s.opts.env && typeof s.opts.env === "object" ? s.opts.env : undefined,
          inputB64: stdin != null && typeof stdin !== "string" && typeof stdin === "object" && (ArrayBuffer.isView(stdin) || stdin instanceof ArrayBuffer) ? u8ToB64(stdin) : typeof stdin === "string" && stdin !== "pipe" && stdin !== "inherit" && stdin !== "ignore" ? u8ToB64(stdin) : undefined,
        });
        return syncResult(r, Buffer.from(b64ToU8(r.stdoutB64)), Buffer.from(b64ToU8(r.stderrB64)));
      }
      const r = runNative(s.cmd, s.opts);
      return syncResult(r, Buffer.from(r.stdout), Buffer.from(r.stderr));
    };
    // Bun.stdin — real fd-0 reads (blocking natives; EOF ends the stream).
    if (typeof Bun.stdin === "undefined" && PN) {
      // stdin is a non-seekable pipe: slice() cannot apply a start offset, but it
      // DOES cap the read to (end - start) bytes (bun: sliced-blob byte length).
      const makeStdinBlob = (cap) => {
        let remaining = cap;
        const readChunk = () => {
          if (remaining <= 0) return null;
          const want = remaining < 65536 ? remaining : 65536;
          const b = PN.read(0, want); if (b === null) return null;
          let c = b64ToU8(b); if (c.length > remaining) c = c.subarray(0, remaining);
          remaining -= c.length; return c.length ? c : null;
        };
        const drainAll = () => { const cs = []; for (;;) { const c = readChunk(); if (!c) break; cs.push(c); } let t = 0; for (const c of cs) t += c.length; const out = new Uint8Array(t); let o = 0; for (const c of cs) { out.set(c, o); o += c.length; } return out; };
        return {
          stream() { return { async *[Symbol.asyncIterator]() { for (;;) { const c = readChunk(); if (!c) return; yield c; } }, getReader() { return { read: () => { const c = readChunk(); return Promise.resolve(c ? { value: c, done: false } : { value: undefined, done: true }); }, releaseLock() {}, cancel() { return Promise.resolve(); } }; } }; },
          bytes() { return Promise.resolve().then(drainAll); },
          arrayBuffer() { return Promise.resolve().then(() => drainAll().buffer); },
          text() { return Promise.resolve().then(() => td.decode(drainAll())); },
          json() { return Promise.resolve().then(() => JSON.parse(td.decode(drainAll()))); },
          slice(start, end) { const s = start === undefined ? 0 : (Number(start) || 0); const e = end === undefined ? Infinity : Number(end); const nc = Math.max(0, e - s); return makeStdinBlob(Math.min(cap, nc)); },
        };
      };
      Bun.stdin = makeStdinBlob(Infinity);
    }
    // Binary process.stdout/.stderr writes (Buffers must not be UTF-8 mangled).
    if (PN && G.process) {
      for (const [name, fd] of [["stdout", 1], ["stderr", 2]]) {
        const strm = G.process[name];
        if (strm && typeof strm.write === "function" && !strm.__mbunBinWrite) {
          const textWrite = strm.write.bind(strm);
          strm.write = (d, ...rest) => { if (d instanceof ArrayBuffer || ArrayBuffer.isView(d)) { PN.write(fd, u8ToB64(d)); return true; } return textWrite(d, ...rest); };
          strm.__mbunBinWrite = true;
          strm.flush = strm.flush || (() => {});
          // node: a write-only stdout/stderr's async iterator completes at once
          // (nothing to read). Present so `for await (const _ of process.stdout)`
          // doesn't throw "is not async iterable".
          strm[Symbol.asyncIterator] = strm[Symbol.asyncIterator] || function () { return { next() { return Promise.resolve({ value: undefined, done: true }); }, [Symbol.asyncIterator]() { return this; } }; };
        }
      }
    }
    if (typeof Bun.write === "undefined") {
      Bun.write = function (dest, data, opts) {
        const target = toStr(dest && dest.name ? dest.name : dest);
        // bun aligns: createPath defaults to true -> recursively create missing
        // parent dirs before writing; { createPath: false } skips (write then
        // fails with ENOENT if the parent dir is absent).
        if (!(opts && opts.createPath === false)) {
          const dir = path.dirname(target);
          if (dir && dir !== "." && dir !== "/" && dir !== target) {
            try { F.mkdir(dir, true); } catch (e) {}
          }
        }
        F.writeFile(target,
                    data instanceof Uint8Array ? td.decode(data) : toStr(data));
        return Promise.resolve((data && data.length) || 0);
      };
    }
    if (typeof Bun.sleep === "undefined") Bun.sleep = (ms) => new Promise((r) => G.setTimeout(r, +ms || 0));
    if (typeof Bun.sleepSync === "undefined") Bun.sleepSync = () => {};
    if (typeof Bun.which === "undefined") {
      Bun.which = (c) => { try { const r = CPN.spawnSync("/bin/sh", ["-c", "command -v " + toStr(c)], {}); const o = (r.stdout || "").trim(); return o || null; } catch (e) { return null; } };
    }
    if (typeof Bun.env === "undefined") Bun.env = globalThis.process.env;
    if (typeof Bun.gc === "undefined") Bun.gc = () => {};
    if (typeof Bun.allocUnsafe !== "function") Bun.allocUnsafe = (size) => new Uint8Array((size >>> 0));
    // Bun.unsafe: low-level knobs. gcAggressionLevel(v?) reads/sets the level and
    // returns the previous one (drives harness withoutAggressiveGC). No-op GC
    // tuning here (mbun has no aggressive-GC mode) — behaviourally inert.
    if (typeof Bun.unsafe === "undefined") Bun.unsafe = {};
    if (typeof Bun.unsafe.gcAggressionLevel !== "function") {
      let __gcLevel = 0;
      Bun.unsafe.gcAggressionLevel = (v) => { const prev = __gcLevel; if (v !== undefined) __gcLevel = v | 0; return prev; };
    }
    if (typeof Bun.unsafe.arrayBufferToString !== "function") {
      // ref UnsafeObject.rs: 16-bit views decode as native-endian UTF-16, all
      // else (Uint8Array/ArrayBuffer/other views) as Latin-1. No copy semantics.
      Bun.unsafe.arrayBufferToString = (input) => {
        if (input == null || !(input instanceof ArrayBuffer || ArrayBuffer.isView(input))) throw new TypeError("Expected an ArrayBuffer");
        const is16 = (typeof Uint16Array !== "undefined" && input instanceof Uint16Array) || (typeof Int16Array !== "undefined" && input instanceof Int16Array);
        const bytes = input instanceof ArrayBuffer ? new Uint8Array(input) : new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
        return new TextDecoder(is16 ? "utf-16le" : "latin1").decode(bytes);
      };
    }
    try { if (Bun[Symbol.toStringTag] === undefined) Object.defineProperty(Bun, Symbol.toStringTag, { value: "Bun", configurable: true }); } catch (e) {}
    if (typeof Bun.indexOfLine === "undefined") Bun.indexOfLine = (buf, off) => { let o = Math.trunc(Number(off)); if (!Number.isFinite(o) || o < 0) o = 0; for (let i = o; i < buf.length; i++) if (buf[i] === 10) return i; return -1; };
    // Bun.cron.parse(expr, from): next UTC Date matching a 5-field cron expr after
    // `from` (exclusive), or null if none within ~5 years. In-process/OS scheduling
    // overloads are DEFERRED.
    if (typeof Bun.cron === "undefined") {
      const MON = { jan: 1, feb: 2, mar: 3, apr: 4, may: 5, jun: 6, jul: 7, aug: 8, sep: 9, oct: 10, nov: 11, dec: 12 };
      const DOW = { sun: 0, mon: 1, tue: 2, wed: 3, thu: 4, fri: 5, sat: 6 };
      const resolveTok = (s, names) => { const l = String(s).toLowerCase(); return names && names[l] !== undefined ? names[l] : parseInt(s, 10); };
      const parseField = (f, min, max, names) => { const set = new Set(); for (const part of String(f).split(",")) { let step = 1, range = part; const sl = part.indexOf("/"); if (sl >= 0) { step = parseInt(part.slice(sl + 1), 10) || 1; range = part.slice(0, sl); } let lo, hi; if (range === "*" || range === "") { lo = min; hi = max; } else if (range.indexOf("-") > 0) { const parts = range.split("-"); lo = resolveTok(parts[0], names); hi = resolveTok(parts[1], names); } else { lo = resolveTok(range, names); hi = sl >= 0 ? max : lo; } if (isNaN(lo) || isNaN(hi)) return null; for (let v = lo; v <= hi; v += step) set.add(v); } return set; };
      const parse = (expr, from) => {
        const fields = String(expr).trim().split(/\s+/);
        if (fields.length !== 5) return null;
        const minSet = parseField(fields[0], 0, 59), hourSet = parseField(fields[1], 0, 23), domSet = parseField(fields[2], 1, 31), monSet = parseField(fields[3], 1, 12, MON), dowRaw = parseField(fields[4], 0, 7, DOW);
        if (!minSet || !hourSet || !domSet || !monSet || !dowRaw) return null;
        const dowSet = new Set(); for (const v of dowRaw) dowSet.add(v % 7);
        const domR = fields[2] !== "*", dowR = fields[4] !== "*";
        const d = new Date((from instanceof Date ? from.getTime() : new Date(from).getTime()));
        d.setUTCSeconds(0, 0); d.setUTCMinutes(d.getUTCMinutes() + 1);
        const limit = d.getTime() + 5 * 366 * 24 * 3600 * 1000;
        while (d.getTime() <= limit) {
          if (!monSet.has(d.getUTCMonth() + 1)) { d.setUTCMonth(d.getUTCMonth() + 1, 1); d.setUTCHours(0, 0, 0, 0); continue; }
          const dom = d.getUTCDate(), dow = d.getUTCDay();
          const dayOk = (domR && dowR) ? (domSet.has(dom) || dowSet.has(dow)) : domR ? domSet.has(dom) : dowR ? dowSet.has(dow) : true;
          if (!dayOk) { d.setUTCDate(d.getUTCDate() + 1); d.setUTCHours(0, 0, 0, 0); continue; }
          if (!hourSet.has(d.getUTCHours())) { d.setUTCHours(d.getUTCHours() + 1, 0, 0, 0); continue; }
          if (!minSet.has(d.getUTCMinutes())) { d.setUTCMinutes(d.getUTCMinutes() + 1, 0, 0); continue; }
          return new Date(d.getTime());
        }
        return null;
      };
      Bun.cron = Object.assign(function () { throw new Error("Bun.cron scheduling is not implemented yet in mbun"); }, { parse });
    }
)JS";

}  // namespace mbun::jsc::builtins::detail
