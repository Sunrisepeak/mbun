// YAML block and Markdown payload partition; keep raw bytes aligned with js_builtins.cppm lines 4458-6364.
export module mbun.jsc.js_builtins:yaml_block_markdown;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kYamlBlockMarkdownJS = R"JS(  // ---- block mode ----
  // Returns the fully indented text for the node at `level`; scalars inline.
  Emitter.prototype.blockValueFor = function (val, key, level) {
    // value in a mapping: objects go on the following line
    const v = unboxOrSelf(val);
    if (v === null || typeof v !== "object") return { inline: emitScalar(v) };
    const pad = this.space.repeat(level + 1);
    if (this.shared.has(v)) {
      const name = this.nameFor(v, key, false, false);
      if (this.emittedAnchors.has(v)) return { block: pad + "*" + name };
      this.emittedAnchors.add(v);
      return { block: pad + "&" + name + "\n" + this.blockContent(v, level + 1) };
    }
    return { block: this.blockContent(v, level + 1) };
  };

  Emitter.prototype.blockContent = function (val, level) {
    const v = val;
    const pad = this.space.repeat(level);
    if (v instanceof Number || v instanceof Boolean) return pad + emitScalar(v.valueOf());
    if (Array.isArray(v)) {
      const lines = [];
      for (let i = 0; i < v.length; i++) {
        if (!(i in v)) continue;
        const rawItem = v[i];
        if (isSkippable(rawItem)) continue;
        const item = unboxOrSelf(rawItem);
        if (item === null || typeof item !== "object") {
          lines.push(pad + "- " + emitScalar(item));
        } else if (this.shared.has(item)) {
          const name = this.nameFor(item, null, true, false);
          if (this.emittedAnchors.has(item)) {
            lines.push(pad + "- *" + name);
          } else {
            this.emittedAnchors.add(item);
            lines.push(pad + "- &" + name + "\n" + this.blockContent(item, level + 1));
          }
        } else {
          // inline the first line after the dash
          const inner = this.blockContent(item, level + 1);
          const innerPad = this.space.repeat(level + 1);
          lines.push(pad + "- " + inner.slice(innerPad.length));
        }
      }
      if (lines.length === 0) return pad + "[]";
      return lines.join("\n");
    }
    // mapping
    const keys = isMaskedPlatformObject(v) ? [] : ownKeysOf(v);
    const lines = [];
    for (const k of keys) {
      const val2 = v[k];
      if (isSkippable(val2)) continue;
      const r = this.blockValueFor(val2, keyText(k), level);
      if (r.inline !== undefined) lines.push(pad + emitString(keyText(k)) + ": " + r.inline);
      else lines.push(pad + emitString(keyText(k)) + ": \n" + r.block);
    }
    if (lines.length === 0) return pad + "{}";
    return lines.join("\n");
  };

  Emitter.prototype.blockRoot = function (val) {
    const v = unboxOrSelf(val);
    if (v === null || typeof v !== "object") return emitScalar(v);
    if (v instanceof Number || v instanceof Boolean) return emitScalar(v.valueOf());
    if (this.shared.has(v)) {
      const name = this.nameFor(v, null, false, true);
      this.emittedAnchors.add(v);
      return "&" + name + "\n" + this.blockContent(v, 0);
    }
    return this.blockContent(v, 0);
  };

  Emitter.prototype.flowRoot = function (val) {
    const v = unboxOrSelf(val);
    if (v === null || typeof v !== "object") return emitScalar(v);
    if (v instanceof Number || v instanceof Boolean) return emitScalar(v.valueOf());
    if (this.shared.has(v)) {
      const name = this.nameFor(v, null, false, true);
      this.emittedAnchors.add(v);
      return "&" + name + " " + this.flowContent(v);
    }
    return this.flowContent(v);
  };

  function normalizeSpace(space) {
    let sp = space;
    if (sp instanceof Number || sp instanceof String) sp = sp.valueOf();
    if (typeof sp === "number") {
      if (Number.isNaN(sp)) return null;
      if (sp === Infinity) return " ".repeat(10);
      if (sp <= 0 || sp === -Infinity) return null;
      const n = Math.min(10, Math.floor(sp));
      return n > 0 ? " ".repeat(n) : null;
    }
    if (typeof sp === "string") return sp.length > 0 ? sp : null;
    return null;
  }

  function stringify(value, replacer, space) {
    if (typeof value === "bigint") {
      throw new TypeError("YAML.stringify cannot serialize BigInt");
    }
    if (replacer !== undefined && replacer !== null) {
      throw new TypeError("YAML.stringify does not support the replacer argument");
    }
    if (isSkippable(value)) return undefined;
    const unit = normalizeSpace(space);
    const em = new Emitter(unit);
    const seenOnce = new Set();
    collectShared(value, seenOnce, em.shared);
    if (unit === null) return em.flowRoot(value);
    return em.blockRoot(value);
  }

  return { parse, stringify };
})();
      Object.defineProperty(__YAML, Symbol.toStringTag, { value: "YAML", configurable: true });
      Bun.YAML = __YAML;
    }
    // Bun.SQL — connection-option resolution (adapter inference, env-var
    // precedence, URL parsing). Query execution DEFERRED; `.options` is real.
    // Precedence: explicit option > URL component > adapter env > generic env > default.
    if (typeof Bun.SQL === "undefined") {
      const parseDbUrl = (raw) => {
        let s = String(raw), scheme = null;
        const m = s.match(/^([a-zA-Z][a-zA-Z0-9+.-]*):\/\//);
        if (m) { scheme = m[1].toLowerCase(); s = s.slice(m[0].length); }
        if (scheme === "unix") return { scheme, path: "/" + s.replace(/^\/+/, "") };
        if (scheme === "sqlite" || scheme === "file") return { scheme, filename: "/" + s.replace(/^\/+/, "") };
        const out = { scheme };
        const at = s.lastIndexOf("@");
        if (at >= 0) { const cred = s.slice(0, at); s = s.slice(at + 1); const c = cred.indexOf(":"); if (c >= 0) { out.username = cred.slice(0, c); out.password = cred.slice(c + 1); } else if (cred) out.username = cred; }
        const slash = s.indexOf("/");
        if (slash >= 0) { const db = s.slice(slash + 1); if (db) out.database = db; s = s.slice(0, slash); }
        const colon = s.lastIndexOf(":");
        if (colon >= 0) { const p = parseInt(s.slice(colon + 1), 10); if (!isNaN(p)) out.port = p; s = s.slice(0, colon); }
        if (s) out.hostname = s;
        return out;
      };
      const schemeAdapter = (sch) => sch == null ? null : (["postgres", "postgresql", "pg"].includes(sch) ? "postgres" : (["mysql", "mysql2", "mariadb"].includes(sch) ? "mysql" : (["sqlite", "file"].includes(sch) ? "sqlite" : null)));
      class SQL {
        constructor(a, b) {
          const env = G.process.env;
          let url = null, opts = {};
          if (typeof a === "string" || a instanceof URL) { url = String(a); opts = b || {}; }
          else if (a && typeof a === "object") { opts = a; if (typeof opts.url === "string") url = opts.url; }
          let adapter = opts.adapter || null;
          let sslMode = opts.sslMode;
          // env URL sources, each carrying an adapter implied by its NAME (which
          // overrides the URL's protocol) and TLS_* implying sslMode=require(2)
          if (url == null && !("hostname" in opts) && !("host" in opts) && !("filename" in opts) && !("path" in opts)) {
            const SOURCES = [
              ["POSTGRES_URL", "postgres"], ["PGURL", "postgres"], ["PG_URL", "postgres"],
              ["MYSQL_URL", "mysql"], ["MYSQLURL", "mysql"], ["MARIADB_URL", "mysql"], ["MARIADBURL", "mysql"],
              ["SQLITE_URL", "sqlite"], ["SQLITEURL", "sqlite"],
              ["TLS_POSTGRES_DATABASE_URL", "postgres", 2], ["TLS_MYSQL_DATABASE_URL", "mysql", 2], ["TLS_MARIADB_DATABASE_URL", "mysql", 2],
              ["TLS_DATABASE_URL", null, 2], ["DATABASE_URL", null], ["DATABASEURL", null],
            ];
            for (const [name, impliedAdapter, ssl] of SOURCES) {
              if (!env[name]) continue;
              if (adapter && impliedAdapter && impliedAdapter !== adapter) continue;  // explicit adapter only reads its own URLs
              url = env[name];
              if (!adapter) adapter = impliedAdapter;
              if (ssl !== undefined && sslMode === undefined) sslMode = ssl;
              break;
            }
          }
          const u = url != null ? parseDbUrl(url) : {};
          if (!adapter) adapter = schemeAdapter(u.scheme) || "postgres";
          const isPg = adapter === "postgres", isMy = adapter === "mysql";
          const pick = (...vals) => { for (const v of vals) if (v !== undefined && v !== null && v !== "") return v; return undefined; };
          const o = { adapter };
          if (adapter === "sqlite") {
            o.filename = pick(opts.filename, u.filename, u.hostname && "/" + u.hostname, ":memory:");
          } else if (u.scheme === "unix" || ("path" in opts)) {
            o.path = pick(opts.path, u.path);
            o.username = pick(opts.username, opts.user, u.username, env.USER);
            o.password = pick(opts.password, opts.pass, u.password, "");
          } else {
            o.hostname = pick(opts.hostname, opts.host, u.hostname, isPg ? env.PGHOST : undefined, isMy ? env.MYSQL_HOST : undefined, "localhost");
            o.port = Number(pick(opts.port, u.port, isPg ? env.PGPORT : undefined, isMy ? env.MYSQL_PORT : undefined, isMy ? 3306 : 5432));
            o.username = pick(opts.username, opts.user, u.username, isPg ? env.PGUSER : undefined, isMy ? env.MYSQL_USER : undefined, env.USER, env.USERNAME, isMy ? "root" : "postgres");
            o.password = pick(opts.password, opts.pass, u.password, isPg ? env.PGPASSWORD : undefined, isMy ? env.MYSQL_PASSWORD : undefined, "");
            o.database = pick(opts.database, opts.db, u.database, isPg ? env.PGDATABASE : undefined, isMy ? env.MYSQL_DATABASE : undefined, isMy ? "mysql" : o.username);
          }
          o.sslMode = sslMode === undefined ? 0 : sslMode;
          this.options = o;
        }
        connect() { return Promise.reject(new Error("Bun.SQL query execution is not implemented yet in mbun")); }
        close() { return Promise.resolve(); }
        end() { return Promise.resolve(); }
      }
      Bun.SQL = SQL;
    }
    if (typeof Bun.Transpiler === "undefined" && typeof G.__mbun_transpile === "function") {
      Bun.Transpiler = class Transpiler {
        constructor(opts) { this._opts = opts || {}; this._loader = this._opts.loader || "tsx"; }
        transformSync(code, loader) { const l = (typeof loader === "string" ? loader : this._loader); const jsx = l === "tsx" || l === "jsx"; return G.__mbun_transpile(String(code), false, jsx); }
        transform(code, loader) { try { return Promise.resolve(this.transformSync(code, loader)); } catch (e) { return Promise.reject(e); } }
        // import/export scanning is DEFERRED (no metadata pass yet) — return empty.
        scan() { return { imports: [], exports: [] }; }
        scanImports() { return []; }
      };
    }
    if (typeof Bun.concatArrayBuffers === "undefined") Bun.concatArrayBuffers = (list, maxLength, asUint8Array) => { if (typeof maxLength === "boolean") { asUint8Array = maxLength; maxLength = undefined; } let total = 0; const arrs = list.map((b) => b instanceof ArrayBuffer ? new Uint8Array(b) : (ArrayBuffer.isView(b) ? new Uint8Array(b.buffer, b.byteOffset, b.byteLength) : new Uint8Array(b))); for (const a of arrs) total += a.length; if (maxLength != null && total > maxLength) total = maxLength; const out = new Uint8Array(total); let o = 0; for (const a of arrs) { if (o >= total) break; out.set(a.subarray(0, total - o), o); o += a.length; } return asUint8Array ? out : out.buffer; };
    if (typeof Bun.color === "undefined") Bun.color = (input, fmt) => { const s = String(input); if (fmt === "css" || fmt === undefined) return s; if (fmt === "ansi" || fmt === "ansi-16m") return s; return s; };
    if (typeof Bun.stringWidth === "function") { /* native */ } else if (typeof Bun.stringWidth === "undefined") Bun.stringWidth = (s) => String(s).length;
    if (typeof Bun.connect === "undefined") Bun.connect = () => Promise.reject(new Error("Bun.connect: TCP sockets are not implemented yet in mbun"));
    if (typeof Bun.listen === "undefined") Bun.listen = () => { throw new Error("Bun.listen: TCP sockets are not implemented yet in mbun"); };
    if (typeof Bun.udpSocket === "undefined") Bun.udpSocket = () => Promise.reject(new Error("Bun.udpSocket: UDP is not implemented yet in mbun"));
    // Wrap the native Bun.file (raw bytes) into a real Blob carrying .name/.type.
    //
    // bun: `Bun.file(p)` IS a Blob over a file store — `instanceof Blob` is true,
    // `instanceof File` is false, `constructor.name` is "Blob", and it carries a
    // `name` (the path). Everything that consumes a Blob (FormData.append, a
    // Request/Response body, structuredClone) therefore works on it unchanged.
    // It used to be a bare object, so `fd.append(k, Bun.file(p))` stringified to
    // "[object Object]" and `.size` was 0 for any non-UTF-8 file.
    if (Bun.file && !Bun.file.__wrapped) {
      const nativeFile = Bun.file;
      const MIME = { md: "text/markdown", markdown: "text/markdown", css: "text/css;charset=utf-8", html: "text/html;charset=utf-8", htm: "text/html;charset=utf-8", js: "text/javascript;charset=utf-8", mjs: "text/javascript;charset=utf-8", cjs: "text/javascript;charset=utf-8", ts: "text/javascript;charset=utf-8", tsx: "text/javascript;charset=utf-8", mts: "text/javascript;charset=utf-8", cts: "text/javascript;charset=utf-8", json: "application/json;charset=utf-8", txt: "text/plain;charset=utf-8", xml: "text/xml;charset=utf-8", csv: "text/csv;charset=utf-8", svg: "image/svg+xml", png: "image/png", jpg: "image/jpeg", jpeg: "image/jpeg", gif: "image/gif", webp: "image/webp", ico: "image/vnd.microsoft.icon", wasm: "application/wasm", pdf: "application/pdf", zip: "application/zip", woff: "font/woff", woff2: "font/woff2", ttf: "font/ttf", mp3: "audio/mpeg", mp4: "video/mp4", wav: "audio/wav" };
      let __fs;
      const fsModule = () => (__fs || (__fs = require("fs")));
      const FDN = G.__mbunFdNative;
      const __te = new TextEncoder();
      const toU8 = (chunk) => {
        if (typeof chunk === "string") return __te.encode(chunk);
        if (chunk instanceof Uint8Array) return chunk;
        if (ArrayBuffer.isView(chunk)) return new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength);
        if (chunk instanceof ArrayBuffer) return new Uint8Array(chunk);
        return __te.encode(String(chunk));
      };
      // Bun.file(path|fd).writer([opts]) → FileSink. mbun runs the JS on a single
      // thread with synchronous fd I/O, so writes flush to the fd immediately.
      // A partial write to a non-blocking fd (a full socket/pipe kernel buffer)
      // leaves the tail buffered and the write()/flush()/end() call returns a
      // Promise that resolves once the buffer drains — bun's backpressure shape.
      const makeFileSink = (cfg) => {
        const LIVE = (G.__mbunFileSinkLive || (G.__mbunFileSinkLive = { n: 0 }));
        LIVE.n++;
        let finished = false;
        let fd = -1, ownFd = false, opened = false, closed = false, errored = null;
        let hwm = (cfg.opts && cfg.opts.highWaterMark) ? (cfg.opts.highWaterMark | 0) : 65536;
        let buffered = new Uint8Array(0);
        const openIfNeeded = () => {
          if (opened || closed) return;
          if (cfg.fd != null && cfg.fd >= 0) { fd = cfg.fd | 0; ownFd = false; }
          else { fd = FDN.open(cfg.path, "w", 0o666, cfg.isFifo ? true : false); ownFd = true; }
          opened = true;
        };
        const appendBuf = (u8) => {
          if (u8.length === 0) return;
          if (buffered.length === 0) { buffered = u8.slice(); return; }
          const g = new Uint8Array(buffered.length + u8.length);
          g.set(buffered); g.set(u8, buffered.length); buffered = g;
        };
        // Push as much of `buffered` to the fd as it will take this tick.
        const drainOnce = () => {
          if (fd < 0) return 0;
          let wrote = 0;
          while (buffered.length > 0) {
            let n;
            try { n = FDN.write(fd, buffered, 0, buffered.length, -1); }
            catch (e) { errored = e; break; }
            if (n <= 0) break;                       // EAGAIN (-1) or nothing accepted
            wrote += n;
            buffered = buffered.length === n ? new Uint8Array(0) : buffered.slice(n);
          }
          return wrote;
        };
        const finish = () => {
          if (ownFd && fd >= 0) { try { FDN.close(fd); } catch (e) {} }
          fd = -1; closed = true; opened = false;
          if (!finished) { finished = true; LIVE.n--; }
        };
        // Resolve `value` once the buffer fully drains, polling a non-blocking fd.
        // The bounded retry count guarantees the promise always settles (a stalled
        // peer resolves rather than hanging the single JS thread forever).
        const drainToPromise = (value) => new Promise((resolve, reject) => {
          let tries = 0;
          const poll = () => {
            drainOnce();
            if (errored) { reject(errored); return; }
            if (buffered.length === 0 || ++tries > 30000) { resolve(value); return; }
            setTimeout(poll, 1);
          };
          setTimeout(poll, 1);
        });
        return {
          write(chunk) {
            if (closed || errored) return 0;               // post-close/-error: no-op, never throw
            try { openIfNeeded(); } catch (e) { errored = e; return 0; }
            const u8 = toU8(chunk); const len = u8.length;
            appendBuf(u8); drainOnce();
            if (errored || buffered.length === 0) return len;
            return drainToPromise(len);
          },
          flush() {
            if (closed || errored) return 0;
            try { openIfNeeded(); } catch (e) { errored = e; return 0; }
            const wrote = drainOnce();
            if (errored || buffered.length === 0) return wrote;
            return drainToPromise(wrote);
          },
          end() {
            if (closed) return 0;
            try { openIfNeeded(); } catch (e) { errored = e; }
            drainOnce();
            if (errored) { finish(); return Promise.reject(errored); }
            if (buffered.length === 0) { finish(); return 0; }
            return new Promise((resolve, reject) => {
              let tries = 0;
              const poll = () => {
                drainOnce();
                if (errored) { finish(); reject(errored); return; }
                if (buffered.length === 0 || ++tries > 30000) { finish(); resolve(0); return; }
                setTimeout(poll, 1);
              };
              setTimeout(poll, 1);
            });
          },
          start(opts) {
            opts = opts || {};
            if (opts.highWaterMark != null) hwm = opts.highWaterMark | 0;
            if (opts.fd != null && opts.fd >= 0) {
              if (ownFd && fd >= 0) { try { FDN.close(fd); } catch (e) {} }
              fd = opts.fd | 0; ownFd = false; opened = true; closed = false; errored = null;
              cfg.fd = fd; cfg.path = null;
            } else if (closed) {
              closed = false; errored = null; opened = false; fd = -1; buffered = new Uint8Array(0);
            }
            return this;
          },
          ref() { return this; },
          unref() { return this; },
        };
      };
      // Non-blocking pull reader over an fd: retry EAGAIN on a timer, enqueue what
      // arrives, and end on read→0 (peer closed). Defers the very first read one
      // macrotask so a writer created after this stream can open/write first.
      // `ownsFd` closes the fd on end (fifo we opened) vs leaving it (caller's fd).
      const makeFdReadStream = (fd, chunkSize, ownsFd) => {
        const CS = (chunkSize && chunkSize > 0) ? (chunkSize | 0) : 65536;
        let done = false, firstPull = true;
        const endStream = () => { done = true; if (ownsFd && fd >= 0) { try { FDN.close(fd); } catch (e) {} } fd = -1; };
        return new G.ReadableStream({
          pull(controller) {
            return new Promise((resolve) => {
              let tries = 0;
              const attempt = () => {
                if (done) { resolve(); return; }
                const buf = new Uint8Array(CS);
                let n;
                try { n = FDN.read(fd, buf, 0, CS, -1); }
                catch (e) { endStream(); try { controller.error(e); } catch (x) {} resolve(); return; }
                if (n > 0) { try { controller.enqueue(buf.subarray(0, n)); } catch (x) {} resolve(); return; }
                if (n === 0) { endStream(); try { controller.close(); } catch (x) {} resolve(); return; }
                if (++tries > 60000) { endStream(); try { controller.close(); } catch (x) {} resolve(); return; }
                setTimeout(attempt, 1);
              };
              if (firstPull) { firstPull = false; setTimeout(attempt, 0); } else attempt();
            });
          },
          cancel() { endStream(); },
        });
      };
      // Bun.file(fifoPath).stream(chunkSize): open the read end O_RDONLY|O_NONBLOCK
      // (held so a writer's open(2) does not block) and stream it non-blocking.
      const makeFifoReadStream = (path, chunkSize) => makeFdReadStream(FDN.open(path, "r", 0o666, true), chunkSize, true);
      const wrapped = function (path, options) {
        const isFd = typeof path === "number" && Number.isFinite(path);
        const fdArg = isFd ? (path | 0) : -1;
        const p = String(path && path.href ? path.href : path);
        // A BunFile is LAZY in bun: constructing one does no I/O, and `.size`
        // comes from stat(2). mbun used to read the whole file here (through
        // the native, into a std::string, then into a typed array — 2x the file
        // in peak RSS), so merely naming a large file was enough to be
        // OOM-killed: regression/issue/8254 reads back a 2 GiB file it just
        // wrote, and only ever looks at three single bytes of it.
        const fsm = fsModule();
        // `p` is what .name reports (an href stays an href, as before); the
        // syscalls need the filesystem path the native's file_url_to_path used
        // to derive for us.
        let fsPath = p;
        if (fsPath.slice(0, 7) === "file://") {
          const rest = fsPath.slice(7);
          const slash = rest.indexOf("/");            // skip an authority ("localhost")
          try { fsPath = decodeURIComponent(slash < 0 ? "/" + rest : rest.slice(slash)); }
          catch (e) { fsPath = slash < 0 ? "/" + rest : rest.slice(slash); }
        }
        let size = 0;
        let ioerr;
        let isFifo = false;
        if (!isFd) {
          try {
            const st = fsm.statSync(fsPath);
            if (st.isDirectory()) ioerr = "EISDIR";
            else { size = Number(st.size) || 0; if (st.isFIFO && st.isFIFO()) isFifo = true; }
          } catch (e) { ioerr = "ENOENT"; }
        }
        const ext = p.slice(p.lastIndexOf(".") + 1).toLowerCase();
        const type = (options && options.type) || MIME[ext] || "application/octet-stream";
        const f = new G.Blob([], { type });
        // name/lastModified are Blob.prototype accessors backed by these slots
        // (lastModified is getter-only, so it cannot be assigned).
        const slot = (k, v) => Object.defineProperty(f, k, { value: v, writable: true, enumerable: false, configurable: true });
        slot("__size", size);
        slot("__name", p);
        slot("__lastModified", 0);
        if (!ioerr && !isFd) {
          const protoU8 = Object.getOwnPropertyDescriptor(G.Blob.prototype, "_u8");
          let loaded = false;   // has the file's content been pulled into __parts?
          // Shadows Blob.prototype's `_u8`: the bytes are pulled off disk the
          // first time anything actually needs them, then cached as the blob's
          // single part (so a second read is free and `_u8 = …` still works).
          Object.defineProperty(f, "_u8", {
            get() {
              if (!loaded) {
                loaded = true;
                // Raw fd read, NOT fs.readFileSync: readFileSync refuses at the
                // synthetic allocation limit, but ArrayBuffer is exempt from
                // that limit in bun, and text()/bytes()/json() are already
                // capped on `size` by Blob.prototype before they get here.
                const FD = G.__mbunFdNative;
                const fd = FD.open(fsPath, "r", 0o666);
                let u = new Uint8Array(size > 0 ? size : 65536);
                let off = 0;
                try {
                  for (;;) {
                    if (off >= u.length) { const g = new Uint8Array(u.length * 2); g.set(u); u = g; }
                    const n = FD.read(fd, u, off, u.length - off, -1);
                    if (!(n > 0)) break;
                    off += n;
                  }
                } finally { FD.close(fd); }
                if (off !== u.length) u = u.subarray(0, off);
                slot("__parts", [u]);
                slot("__size", off);
                return u;
              }
              return protoU8.get.call(f);
            },
            set(v) { loaded = true; protoU8.set.call(f, v); },
            enumerable: false, configurable: true,
          });
          // Range reads go through pread: slicing a file must not pull the
          // whole file into memory just to hand back a few bytes.
          slot("slice", (start, end, sliceType) => {
            if (loaded) return G.Blob.prototype.slice.call(f, start, end, sliceType);
            const norm = (v, dflt) => {
              if (v === undefined) return dflt;
              let n = Number(v);
              if (Number.isNaN(n)) n = 0;
              n = Math.trunc(n);
              return n < 0 ? Math.max(size + n, 0) : Math.min(n, size);
            };
            const s = norm(start, 0);
            const e = Math.max(norm(end, size), s);
            const out = new Uint8Array(e - s);
            if (out.length > 0) {
              const fd = fsm.openSync(fsPath, "r");
              try {
                let got = 0;
                while (got < out.length) {
                  const n = fsm.readSync(fd, out, got, out.length - got, s + got);
                  if (!(n > 0)) break;
                  got += n;
                }
              } finally { fsm.closeSync(fd); }
            }
            const b = new G.Blob([], { type: sliceType || "" });
            Object.defineProperty(b, "__parts", { value: out.length ? [out] : [], writable: true, enumerable: false, configurable: true });
            Object.defineProperty(b, "__size", { value: out.length, writable: true, enumerable: false, configurable: true });
            return b;
          });
        }
        if (ioerr) {
          // Missing/unreadable file: read methods reject with the errno (bun #26632).
          // These shadow Blob.prototype's resolving versions.
          const mkErr = () => { const e = new Error(ioerr + ": " + (ioerr === "EISDIR" ? "illegal operation on a directory" : "no such file or directory") + ", open '" + p + "'"); e.code = ioerr; e.errno = ioerr === "EISDIR" ? -21 : -2; e.syscall = "open"; e.path = p; return e; };
          slot("text", () => Promise.reject(mkErr()));
          slot("json", () => Promise.reject(mkErr()));
          slot("arrayBuffer", () => Promise.reject(mkErr()));
          slot("bytes", () => Promise.reject(mkErr()));
          slot("stream", () => new G.ReadableStream({ start(c) { c.error(mkErr()); } }));
        }
        // bun: .exists() is true only for a regular file — a directory (or missing
        // path) resolves to false. (text/json/arrayBuffer/bytes/stream/slice all
        // come from Blob.prototype and are already byte-accurate.)
        // bun carries .exists on Blob.prototype, so it is never an own key of a
        // BunFile: keep it off Object.keys()/JSON.stringify().
        slot("exists", () => { try { return Promise.resolve(fsm.statSync(fsPath).isFile()); } catch (e) { return Promise.resolve(false); } });
        // Bun.file(...).writer([opts]) → incremental FileSink over the fd.
        slot("writer", (wopts) => makeFileSink({ path: isFd ? null : fsPath, fd: isFd ? fdArg : -1, isFifo: isFifo, opts: wopts }));
        // A fifo cannot be read by the synchronous Blob loader (open(2)/read(2)
        // would block on the peer); stream it non-blocking instead.
        if (isFifo && !ioerr) slot("stream", (cs) => makeFifoReadStream(fsPath, cs));
        // A raw fd (e.g. one end of a socket pair) has no synchronous Blob bytes;
        // stream it non-blocking straight off the descriptor.
        else if (isFd) slot("stream", (cs) => makeFdReadStream(fdArg, cs, false));
        try { Object.defineProperty(f, "__isBunFile", { value: true, enumerable: false, configurable: true }); } catch (e) {}
        return f;
      };
      wrapped.__wrapped = true;
      Bun.file = wrapped;
    }
    const S = G.__mbunStreams;
    // Fallback for non-mbun ReadableStream shapes (e.g. bunBody duck types).
    const strmFallback = (s, m) => (s && typeof s[m] === "function" ? s[m]() : Promise.resolve(m === "bytes" ? new Uint8Array() : m === "arrayBuffer" ? new ArrayBuffer(0) : ""));
    const useS = (s) => S && S.isReadableStream(s);
    if (typeof Bun.readableStreamToText === "undefined") Bun.readableStreamToText = (s) => useS(s) ? S.text(s) : strmFallback(s, "text");
    if (typeof Bun.readableStreamToJSON === "undefined") Bun.readableStreamToJSON = (s) => useS(s) ? S.json(s) : strmFallback(s, "json");
    if (typeof Bun.readableStreamToArrayBuffer === "undefined") Bun.readableStreamToArrayBuffer = (s) => useS(s) ? S.arrayBuffer(s) : strmFallback(s, "arrayBuffer");
    if (typeof Bun.readableStreamToBytes === "undefined") Bun.readableStreamToBytes = (s) => useS(s) ? S.bytes(s) : strmFallback(s, "bytes");
    if (typeof Bun.readableStreamToArray === "undefined") Bun.readableStreamToArray = (s) => { if (useS(s)) return S.array(s); return (async () => { const out = []; if (s && s.getReader) { const r = s.getReader(); for (;;) { const { value, done } = await r.read(); if (done) break; out.push(value); } } else if (s && s[Symbol.asyncIterator]) { for await (const c of s) out.push(c); } return out; })(); };
    if (typeof Bun.readableStreamToBlob === "undefined") Bun.readableStreamToBlob = (s) => useS(s) ? S.blob(s) : (async () => new G.Blob(await Bun.readableStreamToArray(s)))();
    if (typeof Bun.readableStreamToFormData === "undefined") Bun.readableStreamToFormData = (s, boundary) => {
      const err = useS(s) ? S.usableError(s) : null;
      if (err) return Promise.reject(err);
      return (useS(s) ? S.bytes(s) : Bun.readableStreamToBytes(s)).then((u8) => G.__mbunParseFormData(u8, boundary));
    };
    if (typeof Bun.Cookie === "undefined") {
      class Cookie {
        constructor(name, value, options) {
          // `name` is read-only (bun Cookie.classes.ts: getter, no setter). The no-op
          // setter keeps `cookie.name = x` a silent no-op instead of throwing in strict mode.
          let _name;
          if (typeof name === "string" && value === undefined) {
            const p = Cookie.from(name);
            _name = p.name; this.value = p.value; this.domain = p.domain; this.path = p.path;
            this.secure = p.secure; this.httpOnly = p.httpOnly; this.sameSite = p.sameSite; this.partitioned = p.partitioned;
            this.maxAge = p.maxAge; this.expires = p.expires;
            Object.defineProperty(this, "name", { get() { return _name; }, set(v) {}, enumerable: true, configurable: true });
            return;
          }
          if (name && typeof name === "object") { options = name; name = options.name; value = options.value; }
          _name = String(name); this.value = String(value); options = options || {};
          Object.defineProperty(this, "name", { get() { return _name; }, set(v) {}, enumerable: true, configurable: true });
          // An explicit empty path ("") is preserved so appendTo omits the Path attribute;
          // only an absent path defaults to "/".
          this.domain = options.domain || null; this.path = (options.path === undefined || options.path === null) ? "/" : options.path;
          this.secure = !!options.secure; this.httpOnly = !!options.httpOnly;
          this.sameSite = options.sameSite || "lax"; this.maxAge = options.maxAge; this.partitioned = !!options.partitioned;
          const e = options.expires;
          if (e === undefined || e === null) this.expires = undefined;
          else if (e instanceof Date) { if (isNaN(e.getTime())) throw new Error("expires must be a valid Date (or Number)"); this.expires = e; }
          else if (typeof e === "number") { if (!isFinite(e)) throw new Error("expires must be a valid Number"); this.expires = new Date(e * 1000); }
          else throw new Error("expires must be a valid Date (or Number)");
        }
        isExpired() { if (this.maxAge != null) return this.maxAge <= 0; return this.expires instanceof Date && this.expires.getTime() < Date.now(); }
        serialize() { return this.toString(); }
        toString() {
          // bun appendTo percent-encodes the value (encodeURIComponent). Unpaired
          // surrogates would throw; bun swallows that and keeps the raw value.
          let ev; try { ev = encodeURIComponent(this.value); } catch (e) { ev = this.value; }
          let s = this.name + "=" + ev;
          if (this.domain) s += "; Domain=" + this.domain;
          if (this.path) s += "; Path=" + this.path;
          if (this.expires instanceof Date) s += "; Expires=" + this.expires.toUTCString();
          if (this.maxAge != null) s += "; Max-Age=" + this.maxAge;
          if (this.secure) s += "; Secure";
          if (this.httpOnly) s += "; HttpOnly"; if (this.partitioned) s += "; Partitioned";
          if (this.sameSite) s += "; SameSite=" + (this.sameSite[0].toUpperCase() + this.sameSite.slice(1));
          return s;
        }
        toJSON() { const o = { name: this.name, value: this.value, domain: this.domain == null ? undefined : this.domain, path: this.path, expires: this.expires, secure: this.secure, sameSite: this.sameSite, httpOnly: this.httpOnly, partitioned: this.partitioned }; if (this.maxAge != null) o.maxAge = this.maxAge; return o; }  // bun: omit maxAge when NaN/unset (Cookie.cpp:340)
        static from(name, value, options) {
          if (typeof name === "string" && value === undefined) {
            const parts = name.split(";");
            const first = parts[0]; const i = first.indexOf("=");
            const k = (i < 0 ? first : first.slice(0, i)).trim();
            const v = (i < 0 ? "" : first.slice(i + 1)).trim();
            const opts = {};
            for (let p = 1; p < parts.length; p++) {
              const seg = parts[p].trim(); if (!seg) continue;
              const eq = seg.indexOf("="); const an = (eq < 0 ? seg : seg.slice(0, eq)).trim().toLowerCase(); const av = eq < 0 ? "" : seg.slice(eq + 1).trim();
              if (an === "max-age") { const n = parseInt(av, 10); if (!isNaN(n)) opts.maxAge = n; }
              else if (an === "domain") opts.domain = av;
              else if (an === "path") opts.path = av;
              else if (an === "secure") opts.secure = true;
              else if (an === "httponly") opts.httpOnly = true;
            else if (an === "partitioned") opts.partitioned = true;
              else if (an === "samesite") opts.sameSite = av.toLowerCase();
              else if (an === "expires") { const d = new Date(av); if (!isNaN(d.getTime())) opts.expires = d; }
            }
            return new Cookie(k, v, opts);
          }
          return new Cookie(name, value, options);
        }
        static parse(str) { return Cookie.from(str); }
      }
      Bun.Cookie = Cookie;
      // bun Cookie.cpp validators. Names: [!-:<>-~]+
      // (non-empty). Paths: [ -:=-~]* (may be empty).
      // Domains: [a-z0-9.-]*.
      const isValidCookieName = (s) => { if (s.length === 0) return false; for (let i = 0; i < s.length; i++) { const c = s.charCodeAt(i); if (!((c >= 0x21 && c <= 0x3A) || c === 0x3C || (c >= 0x3E && c <= 0x7E))) return false; } return true; };
      const isValidCookiePath = (s) => { for (let i = 0; i < s.length; i++) { const c = s.charCodeAt(i); if (!((c >= 0x20 && c <= 0x3A) || (c >= 0x3D && c <= 0x7E))) return false; } return true; };
      const isValidCookieDomain = (s) => { for (let i = 0; i < s.length; i++) { const c = s.charCodeAt(i); if (!((c >= 97 && c <= 122) || (c >= 48 && c <= 57) || c === 46 || c === 45)) return false; } return true; };
      // ref: bun src/jsc/bindings/CookieMap.{h,cpp} — two ordered lists, not one map.
      // `_orig` mirrors m_originalCookies (parsed from the request's Cookie header);
      // `_mod` mirrors m_modifiedCookies (only what .set()/.delete() touched).
      // The iterator walks them by absolute index (modified then original), so
      // deleting during iteration shifts entries exactly like bun (FormData-style).
      // Only `_mod` is written back as Set-Cookie (getAllChanges()), so
      // request-borne cookies are never echoed to the client.
      Bun.CookieMap = class CookieMap {
        constructor(init) {
          this._orig = []; this._mod = [];
          if (typeof init === "string") {
            // bun decodeURIComponentSIMD: values are percent-decoded only when the
            // header contains a '%'. Cookie NAMES are never decoded (prefix-rule safety).
            const hasPct = init.indexOf("%") >= 0;
            for (const part of init.split(";")) {
              const i = part.indexOf("="); if (i < 0) continue;
              const name = part.slice(0, i).trim(); if (name === "") continue;
              let value = part.slice(i + 1).trim();
              if (hasPct) { try { value = decodeURIComponent(value); } catch (e) {} }
              this._orig.push([name, value]);
            }
          } else if (Array.isArray(init)) {
            for (const e of init) {
              if (!Array.isArray(e) || e.length !== 2 || typeof e[0] !== "string" || typeof e[1] !== "string")
                throw new TypeError("Expected arrays of exactly two strings");
              this._orig.push([e[0], e[1]]);
            }
          } else if (init && typeof init === "object") {
            for (const k of Object.keys(init)) this._orig.push([k, String(init[k])]);
          }
        }
        // CookieMap::removeInternal — drop the name from BOTH lists.
        _removeInternal(name) { this._orig = this._orig.filter((e) => e[0] !== name); this._mod = this._mod.filter((c) => c.name !== name); }
        // CookieMap::get — modified wins; an empty value means "deleted" → null.
        get(k) {
          const n = String(k);
          for (const c of this._mod) if (c.name === n) return c.value === "" ? null : c.value;
          for (const e of this._orig) if (e[0] === n) return e[1];
          return null;
        }
        // CookieMap::set(Ref<Cookie>) — removeInternal then append to modified.
        set(k, v, options) {
          const c = (k instanceof Cookie) ? k
            : (k && typeof k === "object") ? new Cookie(k)   // Cookie ctor unpacks {name,value,...}
            : new Cookie(String(k), typeof v === "object" && v ? String(v.value) : String(v), options);
          this._removeInternal(c.name);
          this._mod.push(c);
        }
        has(k) { return this.get(k) !== null; }
        // CookieMap::remove — expire it on the client: empty value + epoch Expires.
        delete(k, options) {
          let name, o;
          if (k && typeof k === "object") { o = k; name = k.name; }
          else { name = k; o = (options && typeof options === "object") ? options : {}; }
          if (name === undefined || name === null || name === "") throw new Error("Cookie name is required");
          name = String(name);
          const path = (o.path === undefined || o.path === null) ? "/" : String(o.path);
          const domain = (o.domain === undefined || o.domain === null) ? "" : String(o.domain);
          if (!isValidCookieName(name)) throw new Error("Invalid cookie name: contains invalid characters");
          if (!isValidCookiePath(path)) throw new Error("Invalid cookie path: contains invalid characters");
          if (!isValidCookieDomain(domain)) throw new Error("Invalid cookie domain: contains invalid characters");
          this._removeInternal(name);
          this._mod.push(new Cookie(name, "", { domain: domain || undefined, path, expires: new Date(0), secure: /^__(Secure|Host)-/i.test(name) }));
        }
        // CookieMap::Iterator::next — live index walk of modified (skip deletions)
        // then original. Reads current list lengths each step, so mutation during
        // iteration behaves like bun.
        *_iter() {
          let i = 0;
          for (;;) {
            const modLen = this._mod.length;
            if (i >= modLen + this._orig.length) return;
            if (i >= modLen) { const e = this._orig[i - modLen]; i++; yield [e[0], e[1]]; continue; }
            const c = this._mod[i]; i++;
            if (c.value === "") continue;
            yield [c.name, c.value];
          }
        }
        toSetCookieHeaders() { return this._mod.map((c) => c.toString()); }
        // CookieMap::size — non-deleted modified + all original.
        get size() { let n = 0; for (const c of this._mod) if (c.value !== "") n++; return n + this._orig.length; }
        entries() { return this._iter(); }
        keys() { const it = this._iter(); return (function* () { for (const [k] of it) yield k; })(); }
        values() { const it = this._iter(); return (function* () { for (const [, v] of it) yield v; })(); }
        forEach(fn, thisArg) { for (const [k, v] of this._iter()) fn.call(thisArg, v, k, this); }
        [Symbol.iterator]() { return this._iter(); }
        // CookieMap::toJSON — modified (non-deleted) first, originals only if unseen.
        toJSON() {
          const out = {}; const seen = new Set();
          for (const c of this._mod) if (c.value !== "" && !seen.has(c.name)) { seen.add(c.name); out[c.name] = c.value; }
          for (const e of this._orig) if (!seen.has(e[0])) { seen.add(e[0]); out[e[0]] = e[1]; }
          return out;
        }
      };
    }
    if (typeof Bun.fileURLToPath === "undefined") Bun.fileURLToPath = M["url"].fileURLToPath;
    if (typeof Bun.pathToFileURL === "undefined") Bun.pathToFileURL = M["url"].pathToFileURL;
    if (typeof Bun.resolveSync === "undefined") Bun.resolveSync = (spec, from) => globalThis.__mbun_resolve_native(String(spec), String(from == null ? (globalThis.__dirname || ".") : from));
    if (typeof Bun.resolve === "undefined") Bun.resolve = (spec, from) => { try { return Promise.resolve(Bun.resolveSync(spec, from)); } catch (e) { return Promise.reject(e); } };
    // Bun.build's BuildMessage renders as the failing source line + caret, then
    // `error: <message>`, then the location — *only* through Bun.inspect/console,
    // never through util.inspect (bun 1.3.14: `util.inspect(msg)` is the node-style
    // "BuildMessage {}", `Bun.inspect(msg)` is the block below), so the formatter
    // hangs off Bun.inspect and leaves util.inspect alone.
    // ref: bun-ref/src/jsc/BuildMessage.rs — a native class carrying
    // { message, level, position }, deliberately not an Error (no .stack), whose
    // position object (:139 generate_position_object) supplies lineText/file/
    // line/column. mbun's BuildMessage is the plain record built by
    // modules/jsc/src/runtime/bun_build.inc, so it is matched structurally.
    const inspectBuildMessage = (m) => {
      const p = m.position;
      const head = "error: " + m.message;
      if (!p || typeof p.line !== "number") return head;
      const gutter = p.line + " | ";
      const column = typeof p.column === "number" ? p.column : 1;
      const caret = " ".repeat(gutter.length + Math.max(0, column - 1)) + "^";
      return gutter + (p.lineText == null ? "" : p.lineText) + "\n" + caret + "\n" +
             head + "\n    at " + (p.file == null ? "" : p.file) + ":" + p.line + ":" + column;
    };
    const isBuildMessage = (v) =>
      v !== null && typeof v === "object" && v.name === "BuildMessage" &&
      typeof v.message === "string" && "position" in v && "level" in v;
    // bun accepts BOTH shapes: Bun.inspect(v, { ...options }) and the positional
    // Bun.inspect(v, colors, depth) (ConsoleObject.rs `inspect`). A negative
    // depth is a RangeError, not a silently clamped value.
    if (typeof Bun.inspect === "undefined") Bun.inspect = (v, opts, depthArg) => {
      let o = (opts !== null && typeof opts === "object") ? opts : { colors: opts === true };
      if (depthArg !== undefined && depthArg !== null) o = Object.assign({}, o, { depth: depthArg });
      if (typeof o.depth === "number" && o.depth < 0) throw new RangeError('The "depth" argument must be an integer >= 0');
      return isBuildMessage(v) ? inspectBuildMessage(v) : util.inspect(v, Object.assign({ __bunStyle: true }, o));
    };
    if (typeof Bun.inspect === "function" && Bun.inspect.custom === undefined && util.inspect && util.inspect.custom) Bun.inspect.custom = util.inspect.custom;
    if (typeof Bun.deepEquals === "undefined") Bun.deepEquals = (a, b, strict) => {
      const eq = (x, y, s, seen) => {
        if (Object.is(x, y)) return true;
        if (typeof x !== "object" || typeof y !== "object" || x === null || y === null) return false;
        if (x instanceof Date || y instanceof Date) return x instanceof Date && y instanceof Date && x.getTime() === y.getTime();
        if (x instanceof RegExp || y instanceof RegExp) return x instanceof RegExp && y instanceof RegExp && x.source === y.source && x.flags === y.flags;
        if (Array.isArray(x) !== Array.isArray(y)) return false;
        let xv = seen.get(x); if (xv && xv.has(y)) return true;
        if (!xv) { xv = new Set(); seen.set(x, xv); } xv.add(y);
        try {
          if (x instanceof Map || y instanceof Map) {
            if (!(x instanceof Map && y instanceof Map) || x.size !== y.size) return false;
            for (const [k, v] of x) { if (y.has(k)) { if (!eq(v, y.get(k), s, seen)) return false; continue; } let f = false; for (const [k2, v2] of y) if (eq(k, k2, s, seen) && eq(v, v2, s, seen)) { f = true; break; } if (!f) return false; }
            return true;
          }
          if (x instanceof Set || y instanceof Set) {
            if (!(x instanceof Set && y instanceof Set) || x.size !== y.size) return false;
            for (const v of x) { if (y.has(v)) continue; let f = false; for (const v2 of y) if (eq(v, v2, s, seen)) { f = true; break; } if (!f) return false; }
            return true;
          }
          if (ArrayBuffer.isView(x) || ArrayBuffer.isView(y)) {
            if (!(ArrayBuffer.isView(x) && ArrayBuffer.isView(y)) || x.byteLength !== y.byteLength) return false;
            const ux = new Uint8Array(x.buffer, x.byteOffset, x.byteLength), uy = new Uint8Array(y.buffer, y.byteOffset, y.byteLength);
            for (let i = 0; i < ux.length; i++) if (ux[i] !== uy[i]) return false;
            return true;
          }
          if (s && Object.getPrototypeOf(x) !== Object.getPrototypeOf(y)) return false;
          const keep = (o) => (k) => s || o[k] !== undefined;
          const xk = Object.keys(x).filter(keep(x)), yk = Object.keys(y).filter(keep(y));
          if (xk.length !== yk.length) return false;
          for (const k of xk) { if (!Object.prototype.hasOwnProperty.call(y, k)) return false; if (!eq(x[k], y[k], s, seen)) return false; }
          return true;
        } finally { xv.delete(y); }
      };
      return eq(a, b, !!strict, new Map());
    };
    if (typeof Bun.escapeHTML === "undefined") Bun.escapeHTML = (s) => ("" + s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;").replace(/'/g, "&#x27;");
    // Bun.peek (blueprint src/js/builtins/Peek.ts): settled → value, else the
    // promise/value itself. Backed by native promise-slot readers.
    {
      const pstat = G.__mbun_peek_status, pres = G.__mbun_peek_result;
      Bun.peek = (v) => { const s = pstat(v); return (s === 1 || s === 2) ? pres(v) : v; };
      Bun.peek.status = (v) => { const s = pstat(v); return s === 0 ? "pending" : s === 2 ? "rejected" : "fulfilled"; };
    }
    // randomUUIDv7: time-ordered UUID (48-bit ms timestamp + version 7 + random).
    if (typeof Bun.randomUUIDv7 === "undefined") Bun.randomUUIDv7 = (function () {
      // Monotonic 12-bit counter (bun jsc/uuid.rs UUID7::init): within one
      // millisecond timestamp the counter increments so successive UUIDs sort
      // in generation order; it resets to 0 when the timestamp changes.
      let lastTs = -1, counter = 0;
      return (enc, ts) => {
      // bun rejects an unknown encoding label with ERR_UNKNOWN_ENCODING
      // (JSBufferEncodingType.cpp:91), it does not fall back to hex.
      if (enc !== undefined && enc !== null && enc !== "hex" && enc !== "base64" && enc !== "base64url" && enc !== "buffer") {
        const e = new TypeError("Invalid encoding"); e.code = "ERR_UNKNOWN_ENCODING"; throw e;
      }
      let t = ts instanceof Date ? ts.getTime() : (typeof ts === "number" ? ts : (G.Date ? Date.now() : 0));
      t = t < 0 ? 0 : Math.floor(t);
      if (t !== lastTs) { lastTs = t; counter = 0; }
      const count = (counter++) % 4096;
      const b = new Uint8Array(16); (G.crypto || {}).getRandomValues ? crypto.getRandomValues(b) : b.forEach((_, i) => (b[i] = Math.floor(Math.random() * 256)));
      b[0] = (t / 0x10000000000) & 0xff; b[1] = (t / 0x100000000) & 0xff; b[2] = (t / 0x1000000) & 0xff; b[3] = (t / 0x10000) & 0xff; b[4] = (t / 0x100) & 0xff; b[5] = t & 0xff;
      b[6] = 0x70 | ((count >> 8) & 0x0f); b[7] = count & 0xff; b[8] = (b[8] & 0x3f) | 0x80;
      if (enc === "buffer") return Buffer.from(b);
      const h = Array.from(b, (x) => x.toString(16).padStart(2, "0")).join("");
      const s = h.slice(0, 8) + "-" + h.slice(8, 12) + "-" + h.slice(12, 16) + "-" + h.slice(16, 20) + "-" + h.slice(20);
      return enc === "base64" ? (G.btoa ? btoa(String.fromCharCode(...b)) : s) : s;
      };
    })();
    if (typeof Bun.nanoseconds === "function") { /* native */ } else Bun.nanoseconds = () => 0;
    // Bun.JSONC: JSON with `//`/`/* */` comments + trailing commas. Strip the JSONC
    // extras (string-aware, so `,]`/`//` inside strings are preserved) then defer to
    // JSON.parse — this keeps exact JSON.parse semantics on values/errors.
    // Bun.JSONL: JSON Lines — one JSON value per newline-separated line (JSON
    // strings never contain a literal newline, so splitting on \n is safe).
    if (typeof Bun.JSONL === "undefined") {
      // Semantics (per bun): parse line-by-line; on the first malformed line,
      // return the values collected so far (partial) — unless none were collected,
      // in which case the parse error propagates. Non-string input → TypeError.
      const JSONL = {};
      const inputError = (input) => new TypeError("The \"input\" argument must be of type string or an instance of TypedArray. Received " + (input === null ? "null" : typeof input));
      const incompleteJSON = (text) => {
        let depth = 0, quote = 0, escaped = false;
        for (let i = 0; i < text.length; i++) {
          const c = text.charCodeAt(i);
          if (quote) {
            if (escaped) escaped = false;
            else if (c === 92) escaped = true;
            else if (c === quote) quote = 0;
          } else if (c === 34) quote = c;
          else if (c === 123 || c === 91) depth++;
          else if (c === 125 || c === 93) depth--;
        }
        return quote !== 0 || depth > 0 || text.endsWith(":") || text.endsWith(",") || ["t", "tr", "tru", "f", "fa", "fal", "fals", "n", "nu", "nul"].includes(text);
      };
      JSONL.parseChunk = (input, start, end) => {
        const bytes = input && ArrayBuffer.isView(input);
        if (typeof input !== "string" && !bytes) throw inputError(input);
        if (bytes) G.__mbunCheckAllocLimit(input.byteLength, "text");
        const length = bytes ? input.byteLength : input.length;
        const offset = (value, fallback, negative) => {
          value = value === undefined ? fallback : Number(value);
          return Number.isNaN(value) || value < 0 ? negative : Math.min(length, Number.isFinite(value) ? Math.floor(value) : length);
        };
        let begin = offset(start, 0, 0), finish = offset(end, length, length);
        if (begin > finish) begin = finish;
        const raw = bytes ? new Uint8Array(input.buffer, input.byteOffset + begin, finish - begin) : null;
        let text = bytes ? new G.TextDecoder().decode(raw) : input.slice(begin, finish);
        let bom = 0;
        if (bytes && begin === 0 && raw.length >= 3 && raw[0] === 0xef && raw[1] === 0xbb && raw[2] === 0xbf) bom = 3;
        if (!bytes && begin === 0 && text.charCodeAt(0) === 0xfeff) text = text.slice(1);
        const toOffset = (index) => bytes ? begin + bom + new G.TextEncoder().encode(text.slice(0, index)).byteLength : begin + index + (input.charCodeAt(0) === 0xfeff && begin === 0 ? 1 : 0);
        const values = [];
        let read = begin, pos = 0, error = null, done = true;
        while (pos <= text.length) {
          const newline = text.indexOf("\n", pos);
          const hasNewline = newline !== -1;
          const lineEnd = hasNewline ? newline : text.length;
          const line = text.slice(pos, lineEnd);
          const valueText = line.trim();
          if (valueText) {
            try {
              values.push(JSON.parse(valueText));
              read = toOffset(pos + line.length - line.trimStart().length + valueText.length);
            } catch (e) {
              if (hasNewline || !incompleteJSON(valueText)) error = e;
              else done = false;
              break;
            }
          }
          if (!hasNewline) break;
          pos = newline + 1;
        }
        if (error) done = false;
        return { values, read, done, error };
      };
      JSONL.parse = (input) => {
        if (input === null || input === undefined) throw inputError(input);
        const source = typeof input === "string" || ArrayBuffer.isView(input) ? input : String(input);
        const result = JSONL.parseChunk(source);
        if (result.error && result.values.length === 0) throw result.error;
        return result.values;
      };
      Object.defineProperty(JSONL, Symbol.toStringTag, { value: "JSONL", configurable: true });
      Bun.JSONL = JSONL;
    }
    if (typeof Bun.JSONC === "undefined") {
      // Recursive-descent JSONC parser aligned with bun's lenient tsconfig-style
      // parser (pinned by test/js/bun/jsonc/*). Beyond strict JSON it accepts:
      // `//`+`/* */` comments, trailing commas, single-quoted strings/keys,
      // `\xNN` escapes, hex (0x2A) and legacy-octal (012 -> 10) integers,
      // numbers like `2.`, `.5e1`, `- 1`, exotic whitespace (NBSP/BOM/U+2028...),
      // trailing garbage after the top-level value, and `{}` for empty input.
      // It still rejects: leading/double commas, missing commas/colons, unquoted
      // keys, invalid escapes, raw control chars in strings, and malformed numbers.
      const parseJSONC = (input) => {
        const s = String(input);
        const n = s.length;
        if (n === 0) return {};
        let i = 0;
        const err = (m) => { throw new SyntaxError("JSON Parse error: " + m); };
        const isWS = (cc) => (cc >= 9 && cc <= 13) || cc === 32 || cc === 0xa0 || cc === 0xfeff
          || cc === 0x1680 || (cc >= 0x2000 && cc <= 0x200a) || cc === 0x2028 || cc === 0x2029
          || cc === 0x202f || cc === 0x205f || cc === 0x3000;
        const skipWS = () => {
          while (i < n) {
            const cc = s.charCodeAt(i);
            if (isWS(cc)) { i++; continue; }
            if (cc === 47 /* / */) {
              const d = i + 1 < n ? s.charCodeAt(i + 1) : 0;
              if (d === 47) { i += 2; while (i < n && s.charCodeAt(i) !== 10) i++; continue; }
              if (d === 42) { const e = s.indexOf("*/", i + 2); if (e === -1) err("Unterminated comment"); i = e + 2; continue; }
              err("Unexpected token '/'");
            }
            break;
          }
        };
        // A number/keyword must be immediately followed by a delimiter (bun's
        // lexer errors on `2@`, `123\0`, `[123x]` while `1]`/`1,`/`1/*c*/` are ok).
        const checkLiteralEnd = () => {
          if (i >= n) return;
          const cc = s.charCodeAt(i);
          if (cc === 44 || cc === 93 || cc === 125 || cc === 58 || cc === 47 || isWS(cc)) return;
          err("Unexpected character after literal");
        };
        const isHex = (cc) => (cc >= 48 && cc <= 57) || ((cc | 32) >= 97 && (cc | 32) <= 102);
        const parseNumber = () => {
          const start = i;
          let cc = s.charCodeAt(i);
          if (cc === 48 && i + 1 < n && (s.charCodeAt(i + 1) | 32) === 120) {
            i += 2;
            const h0 = i;
            while (i < n && isHex(s.charCodeAt(i))) i++;
            if (i === h0) err("No hexadecimal digits after '0x'");
            checkLiteralEnd();
            return parseInt(s.slice(h0, i), 16);
          }
          const i0 = i;
          while (i < n && (cc = s.charCodeAt(i)) >= 48 && cc <= 57) i++;
          const intDigits = i - i0;
          let fracDigits = 0;
          if (i < n && s.charCodeAt(i) === 46) {
            i++;
            const f0 = i;
            while (i < n && (cc = s.charCodeAt(i)) >= 48 && cc <= 57) i++;
            fracDigits = i - f0;
          }
          if (intDigits === 0 && fracDigits === 0) err("Invalid number");
          if (i < n && (s.charCodeAt(i) | 32) === 101) {
            i++;
            if (i < n && (s.charCodeAt(i) === 43 || s.charCodeAt(i) === 45)) i++;
            const e0 = i;
            while (i < n && (cc = s.charCodeAt(i)) >= 48 && cc <= 57) i++;
            if (i === e0) err("Exponent has no digits");
          }
          checkLiteralEnd();
          const text = s.slice(start, i);
          // Legacy octal: 0-prefixed all-digit integer (bun: [012] -> [10]).
          if (intDigits > 1 && text.length === intDigits && s.charCodeAt(start) === 48) {
            let octal = true;
            for (let k = 0; k < text.length; k++) { const d = text.charCodeAt(k); if (d < 48 || d > 55) { octal = false; break; } }
            if (octal) return parseInt(text, 8);
          }
          return Number(text);
        };
        const parseString = () => {
          const quote = s.charCodeAt(i);
          i++;
          let out = "";
          let chunk = i;
          for (;;) {
            if (i >= n) err("Unterminated string");
            const cc = s.charCodeAt(i);
            if (cc === quote) { out += s.slice(chunk, i); i++; return out; }
            if (cc === 92 /* \ */) {
              out += s.slice(chunk, i);
              i++;
              if (i >= n) err("Unterminated string");
              const e = s.charCodeAt(i);
              if (e === 34 || e === 39 || e === 92 || e === 47) { out += s[i]; i++; }
              else if (e === 98) { out += "\b"; i++; }
              else if (e === 102) { out += "\f"; i++; }
              else if (e === 110) { out += "\n"; i++; }
              else if (e === 114) { out += "\r"; i++; }
              else if (e === 116) { out += "\t"; i++; }
              else if (e === 117) { // \uXXXX
                if (i + 5 > n) err("Invalid unicode escape");
                let v = 0;
                for (let k = i + 1; k < i + 5; k++) { const h = s.charCodeAt(k); if (!isHex(h)) err("Invalid unicode escape"); v = v * 16 + parseInt(s[k], 16); }
                out += String.fromCharCode(v);
                i += 5;
              } else if (e === 120) { // \xXX
                if (i + 3 > n) err("Invalid hex escape");
                const h1 = s.charCodeAt(i + 1), h2 = s.charCodeAt(i + 2);
                if (!isHex(h1) || !isHex(h2)) err("Invalid hex escape");
                out += String.fromCharCode(parseInt(s.slice(i + 1, i + 3), 16));
                i += 3;
              } else err("Invalid escape character " + s[i]);
              chunk = i;
              continue;
            }
            if (cc < 0x20) err("Unescaped control character in string");
            i++;
          }
        };
        const parseObject = () => {
          i++; // {
          const obj = {};
          for (;;) {
            skipWS();
            if (i >= n) err("Unexpected EOF");
            let cc = s.charCodeAt(i);
            if (cc === 125 /* } */) { i++; return obj; }
            if (cc !== 34 && cc !== 39) err("Property name must be a string literal");
            const key = parseString();
            skipWS();
            if (i >= n || s.charCodeAt(i) !== 58) err("Expected ':' before value in object property definition");
            i++;
            const value = parseValue();
            if (key === "__proto__") Object.defineProperty(obj, key, { value, writable: true, enumerable: true, configurable: true });
            else obj[key] = value;
            skipWS();
            if (i >= n) err("Unexpected EOF");
            cc = s.charCodeAt(i);
            if (cc === 44 /* , */) { i++; continue; }
            if (cc === 125 /* } */) { i++; return obj; }
            err("Expected '}'");
          }
        };
        const parseArray = () => {
          i++; // [
          const arr = [];
          for (;;) {
            skipWS();
            if (i >= n) err("Unexpected EOF");
            let cc = s.charCodeAt(i);
            if (cc === 93 /* ] */) { i++; return arr; }
            if (cc === 44 /* , */) err("Unexpected comma in array literal");
            arr.push(parseValue());
            skipWS();
            if (i >= n) err("Unexpected EOF");
            cc = s.charCodeAt(i);
            if (cc === 44) { i++; continue; }
            if (cc === 93) { i++; return arr; }
            err("Expected ']'");
          }
        };
        const parseValue = () => {
          skipWS();
          if (i >= n) err("Unexpected EOF");
          const cc = s.charCodeAt(i);
          if (cc === 123) return parseObject();
          if (cc === 91) return parseArray();
          if (cc === 34 || cc === 39) return parseString();
          if (cc === 45 /* - */) {
            i++;
            skipWS(); // bun accepts `- 1`
            if (i >= n) err("Unexpected EOF");
            const d = s.charCodeAt(i);
            if (!((d >= 48 && d <= 57) || d === 46)) err("Invalid number");
            return -parseNumber();
          }
          if ((cc >= 48 && cc <= 57) || cc === 46) return parseNumber();
          if (cc === 116 && s.startsWith("true", i)) { i += 4; checkLiteralEnd(); return true; }
          if (cc === 102 && s.startsWith("false", i)) { i += 5; checkLiteralEnd(); return false; }
          if (cc === 110 && s.startsWith("null", i)) { i += 4; checkLiteralEnd(); return null; }
          err("Unrecognized token '" + s[i] + "'");
        };
        const value = parseValue();
        // Trailing data after the top-level value is ignored (bun recovers on
        // `[1]x`, `{}}`, `""x`, `{"a":"b"}#`...), but a dangling `/` or an
        // unterminated block comment still errors.
        skipWS();
        return value;
      };
      Bun.JSONC = { parse: parseJSONC };
    }
    if (typeof Bun.JSON5 === "undefined") {
      // Recursive-descent JSON5 parser (json5.org / bun ast/loader.rs Loader::Json5,
      // which serializes to the same JSON AST as json/toml/yaml). Beyond strict JSON
      // it accepts: `//`+`/* */` comments, trailing commas, single-quoted
      // strings/keys, UNQUOTED (ECMAScript IdentifierName) object keys, string line
      // continuations (`\`+newline), leading `+`, `Infinity`/`-Infinity`/`NaN`, hex
      // (0x2A) integers, and numbers like `2.`/`.5`. The empty document is invalid
      // (JSON5 requires exactly one value) — bun surfaces that as a parse error.
      const parseJSON5 = (input) => {
        const s = String(input);
        const n = s.length;
        let i = 0;
        const err = (m) => { throw new SyntaxError("JSON5 Parse error: " + m); };
        const isWS = (cc) => (cc >= 9 && cc <= 13) || cc === 32 || cc === 0xa0 || cc === 0xfeff
          || cc === 0x1680 || (cc >= 0x2000 && cc <= 0x200a) || cc === 0x2028 || cc === 0x2029
          || cc === 0x202f || cc === 0x205f || cc === 0x3000;
        const skipWS = () => {
          while (i < n) {
            const cc = s.charCodeAt(i);
            if (isWS(cc)) { i++; continue; }
            if (cc === 47 /* / */) {
              const d = i + 1 < n ? s.charCodeAt(i + 1) : 0;
              if (d === 47) { i += 2; while (i < n && s.charCodeAt(i) !== 10) i++; continue; }
              if (d === 42) { const e = s.indexOf("*/", i + 2); if (e === -1) err("Unterminated comment"); i = e + 2; continue; }
              err("Unexpected token '/'");
            }
            break;
          }
        };
        const isHex = (cc) => (cc >= 48 && cc <= 57) || ((cc | 32) >= 97 && (cc | 32) <= 102);
        // ECMAScript IdentifierName (ASCII fast path + any non-ASCII codepoint,
        // which covers the common unicode-letter identifier cases).
        const isIdStart = (cc) => (cc >= 65 && cc <= 90) || (cc >= 97 && cc <= 122)
          || cc === 36 || cc === 95 || cc >= 0x80;
        const isIdPart = (cc) => isIdStart(cc) || (cc >= 48 && cc <= 57);
        const parseNumberBody = () => {
          const start = i;
          let cc = s.charCodeAt(i);
          if (cc === 48 && i + 1 < n && (s.charCodeAt(i + 1) | 32) === 120) {
            i += 2;
            const h0 = i;
            while (i < n && isHex(s.charCodeAt(i))) i++;
            if (i === h0) err("No hexadecimal digits after '0x'");
            return parseInt(s.slice(h0, i), 16);
          }
          const i0 = i;
          while (i < n && (cc = s.charCodeAt(i)) >= 48 && cc <= 57) i++;
          const intDigits = i - i0;
          let fracDigits = 0;
          if (i < n && s.charCodeAt(i) === 46) {
            i++;
            const f0 = i;
            while (i < n && (cc = s.charCodeAt(i)) >= 48 && cc <= 57) i++;
            fracDigits = i - f0;
          }
          if (intDigits === 0 && fracDigits === 0) err("Invalid number");
          if (i < n && (s.charCodeAt(i) | 32) === 101) {
            i++;
            if (i < n && (s.charCodeAt(i) === 43 || s.charCodeAt(i) === 45)) i++;
            const e0 = i;
            while (i < n && (cc = s.charCodeAt(i)) >= 48 && cc <= 57) i++;
            if (i === e0) err("Exponent has no digits");
          }
          return Number(s.slice(start, i));
        };
        const parseString = () => {
          const quote = s.charCodeAt(i);
          i++;
          let out = "";
          let chunk = i;
          for (;;) {
            if (i >= n) err("Unterminated string");
            const cc = s.charCodeAt(i);
            if (cc === quote) { out += s.slice(chunk, i); i++; return out; }
            if (cc === 92 /* \ */) {
              out += s.slice(chunk, i);
              i++;
              if (i >= n) err("Unterminated string");
              const e = s.charCodeAt(i);
              // JSON5 line continuation: backslash + line terminator is removed.
              if (e === 10) { i++; }
              else if (e === 13) { i++; if (i < n && s.charCodeAt(i) === 10) i++; }
              else if (e === 0x2028 || e === 0x2029) { i++; }
              else if (e === 34 || e === 39 || e === 92 || e === 47) { out += s[i]; i++; }
              else if (e === 98) { out += "\b"; i++; }
              else if (e === 102) { out += "\f"; i++; }
              else if (e === 110) { out += "\n"; i++; }
              else if (e === 114) { out += "\r"; i++; }
              else if (e === 116) { out += "\t"; i++; }
              else if (e === 118) { out += "\v"; i++; }
              else if (e === 48 && !(i + 1 < n && s.charCodeAt(i + 1) >= 48 && s.charCodeAt(i + 1) <= 57)) { out += "\0"; i++; }
              else if (e === 117) { // \uXXXX
                if (i + 5 > n) err("Invalid unicode escape");
                let v = 0;
                for (let k = i + 1; k < i + 5; k++) { const h = s.charCodeAt(k); if (!isHex(h)) err("Invalid unicode escape"); v = v * 16 + parseInt(s[k], 16); }
                out += String.fromCharCode(v);
                i += 5;
              } else if (e === 120) { // \xXX
                if (i + 3 > n) err("Invalid hex escape");
                const h1 = s.charCodeAt(i + 1), h2 = s.charCodeAt(i + 2);
                if (!isHex(h1) || !isHex(h2)) err("Invalid hex escape");
                out += String.fromCharCode(parseInt(s.slice(i + 1, i + 3), 16));
                i += 3;
              } else err("Invalid escape character " + s[i]);
              chunk = i;
              continue;
            }
            if (cc < 0x20) err("Unescaped control character in string");
            i++;
          }
        };
        const parseKey = () => {
          const cc = s.charCodeAt(i);
          if (cc === 34 || cc === 39) return parseString();
          if (isIdStart(cc)) { const start = i; i++; while (i < n && isIdPart(s.charCodeAt(i))) i++; return s.slice(start, i); }
          err("Property name must be a string literal or an identifier");
        };
        const parseObject = () => {
          i++; // {
          const obj = {};
          for (;;) {
            skipWS();
            if (i >= n) err("Unexpected EOF");
            let cc = s.charCodeAt(i);
            if (cc === 125 /* } */) { i++; return obj; }
            const key = parseKey();
            skipWS();
            if (i >= n || s.charCodeAt(i) !== 58) err("Expected ':' before value in object property definition");
            i++;
            const value = parseValue();
            if (key === "__proto__") Object.defineProperty(obj, key, { value, writable: true, enumerable: true, configurable: true });
            else obj[key] = value;
            skipWS();
            if (i >= n) err("Unexpected EOF");
            cc = s.charCodeAt(i);
            if (cc === 44 /* , */) { i++; continue; }
            if (cc === 125 /* } */) { i++; return obj; }
            err("Expected '}'");
          }
        };
        const parseArray = () => {
          i++; // [
          const arr = [];
          for (;;) {
            skipWS();
            if (i >= n) err("Unexpected EOF");
            let cc = s.charCodeAt(i);
            if (cc === 93 /* ] */) { i++; return arr; }
            arr.push(parseValue());
            skipWS();
            if (i >= n) err("Unexpected EOF");
            cc = s.charCodeAt(i);
            if (cc === 44) { i++; continue; }
            if (cc === 93) { i++; return arr; }
            err("Expected ']'");
          }
        };
        const parseValue = () => {
          skipWS();
          if (i >= n) err("Unexpected EOF");
          const cc = s.charCodeAt(i);
          if (cc === 123) return parseObject();
          if (cc === 91) return parseArray();
          if (cc === 34 || cc === 39) return parseString();
          if (cc === 45 /* - */) {
            i++;
            if (s.startsWith("Infinity", i)) { i += 8; return -Infinity; }
            const d = i < n ? s.charCodeAt(i) : 0;
            if (!((d >= 48 && d <= 57) || d === 46)) err("Invalid number");
            return -parseNumberBody();
          }
          if (cc === 43 /* + */) {
            i++;
            if (s.startsWith("Infinity", i)) { i += 8; return Infinity; }
            const d = i < n ? s.charCodeAt(i) : 0;
            if (!((d >= 48 && d <= 57) || d === 46)) err("Invalid number");
            return parseNumberBody();
          }
          if ((cc >= 48 && cc <= 57) || cc === 46) return parseNumberBody();
          if (cc === 116 && s.startsWith("true", i)) { i += 4; return true; }
          if (cc === 102 && s.startsWith("false", i)) { i += 5; return false; }
          if (cc === 110 && s.startsWith("null", i)) { i += 4; return null; }
          if (cc === 73 && s.startsWith("Infinity", i)) { i += 8; return Infinity; }
          if (cc === 78 && s.startsWith("NaN", i)) { i += 3; return NaN; }
          err("Unrecognized token '" + s[i] + "'");
        };
        skipWS();
        if (i >= n) err("Unexpected end of JSON5 input");
        const value = parseValue();
        skipWS();
        if (i < n) err("Unexpected token after top-level value");
        return value;
      };
      // json5@2.x style serializer. JSON.stringify cannot be used as a
      // post-pass because JSON5 retains Infinity/NaN, accepts unquoted keys,
      // uses single-quoted strings, and writes trailing commas when indented.
      const stringifyJSON5 = (input, replacer, space) => {
        if (replacer !== undefined && replacer !== null) {
          throw new TypeError("JSON5.stringify does not support the replacer argument");
        }
        let gap = "";
        if (typeof space === "number" || space instanceof Number) {
          const width = Number(space);
          gap = " ".repeat(Math.max(0, Math.min(10, Number.isFinite(width) ? Math.floor(width) : width > 0 ? 10 : 0)));
        } else if (typeof space === "string" || space instanceof String) {
          gap = String(space).slice(0, 10);
        }
        const isIdentifier = (key) => /^[A-Za-z_$][A-Za-z0-9_$]*$/.test(key);
        const quote = (value) => "'" + value.replace(/[\\'\b\f\n\r\t\v\u0000-\u001f\u2028\u2029]/g, (c) => {
          if (c === "'") return "\\'";
          if (c === "\\") return "\\\\";
          if (c === "\b") return "\\b";
          if (c === "\f") return "\\f";
          if (c === "\n") return "\\n";
          if (c === "\r") return "\\r";
          if (c === "\t") return "\\t";
          if (c === "\v") return "\\v";
          const code = c.charCodeAt(0).toString(16).padStart(4, "0");
          return "\\u" + code;
        }) + "'";
        const stack = new Set();
        const serialize = (value, indent, inArray) => {
          if (value === null) return "null";
          switch (typeof value) {
            case "boolean": return value ? "true" : "false";
            case "number":
              if (Number.isNaN(value)) return "NaN";
              if (value === Infinity) return "Infinity";
              if (value === -Infinity) return "-Infinity";
              return String(value);
            case "string": return quote(value);
            case "undefined":
            case "function":
            case "symbol": return inArray ? "null" : undefined;
            case "bigint": throw new TypeError("Do not know how to serialize a BigInt");
          }
          if (stack.has(value)) throw new TypeError("Converting circular structure to JSON");
          stack.add(value);
          const nextIndent = indent + gap;
          let result;
          if (Array.isArray(value)) {
            const entries = value.map((item) => serialize(item, nextIndent, true));
            result = entries.length === 0 ? "[]" : gap === "" ? "[" + entries.join(",") + "]"
              : "[\n" + nextIndent + entries.join(",\n" + nextIndent) + ",\n" + indent + "]";
          } else {
            const entries = [];
            for (const key of Object.keys(value)) {
              const item = serialize(value[key], nextIndent, false);
              if (item !== undefined) entries.push((isIdentifier(key) ? key : quote(key)) + (gap === "" ? ":" : ": ") + item);
            }
            result = entries.length === 0 ? "{}" : gap === "" ? "{" + entries.join(",") + "}"
              : "{\n" + nextIndent + entries.join(",\n" + nextIndent) + ",\n" + indent + "}";
          }
          stack.delete(value);
          return result;
        };
        return serialize(input, "", false);
      };
      Bun.JSON5 = { parse: parseJSON5, stringify: stringifyJSON5 };
    }

    // ---- Bun.markdown.ansi: markdown -> ANSI (port of bun src/md/ansi_renderer) ----
// Markdown -> ANSI renderer, port of bun src/md/ansi_renderer.rs + a CommonMark-ish
// parser. Exposed as globalThis.__renderMarkdownAnsi(src, opts).
(function () {
  "use strict";
  const W = (s) => (globalThis.Bun && Bun.stringWidth ? Bun.stringWidth(s) : s.length);
  // Byte index (JS string index) of longest prefix of s with visible width <= max.
  function visIndexAt(s, max) {
    let w = 0;
    let i = 0;
    while (i < s.length) {
      if (s[i] === "\x1b") {
        if (s[i + 1] === "[") { let j = i + 2; while (j < s.length) { const c = s.charCodeAt(j); if (c >= 0x40 && c <= 0x7e) { j++; break; } j++; } i = j; continue; }
        if (s[i + 1] === "]") { let j = i + 2; while (j < s.length) { if (s[j] === "\x07") { j++; break; } if (s[j] === "\x1b" && s[j + 1] === "\\") { j += 2; break; } j++; } i = j; continue; }
        i++; continue;
      }
      const cp = s.codePointAt(i);
      const clen = cp > 0xffff ? 2 : 1;
      const cw = W(s.slice(i, i + clen));
      if (w + cw > max) break;
      w += cw;
      i += clen;
    }
    return i;
  }

  // ---------------- Parser ----------------
  const RE_ATX = /^ {0,3}(#{1,6})(?:[ \t]+(.*?))?(?:[ \t]+#+)?[ \t]*$/;
  const RE_FENCE = /^( {0,3})(`{3,}|~{3,})[ \t]*([^`]*?)[ \t]*$/;
  const RE_HR = /^ {0,3}([-*_])[ \t]*(?:\1[ \t]*){2,}$/;
  const RE_BQ = /^ {0,3}> ?/;
  const RE_LIST = /^( {0,3})([-+*]|\d{1,9}[.)])([ \t]+|$)(.*)$/;
  const RE_BLANK = /^[ \t]*$/;

  function parseDocument(src) {
    let lines = src.split("\n");
    // A trailing newline yields a final "" element; drop one trailing empty.
    if (lines.length && lines[lines.length - 1] === "") lines.pop();
    return { type: "document", children: parseBlocks(lines) };
  }

  function isTableDelim(line) {
    return /^ {0,3}\|?[ \t]*:?-+:?[ \t]*(\|[ \t]*:?-+:?[ \t]*)*\|?[ \t]*$/.test(line) && line.includes("-");
  }
  function splitRow(line) {
    let s = line.trim();
    if (s.startsWith("|")) s = s.slice(1);
    if (s.endsWith("|")) s = s.slice(0, -1);
    const cells = [];
    let cur = "";
    for (let i = 0; i < s.length; i++) {
      if (s[i] === "\\" && i + 1 < s.length) { cur += s[i] + s[i + 1]; i++; continue; }
      if (s[i] === "|") { cells.push(cur); cur = ""; continue; }
      cur += s[i];
    }
    cells.push(cur);
    return cells.map((c) => c.trim());
  }

  function parseBlocks(lines) {
    const nodes = [];
    let i = 0;
    const n = lines.length;
    while (i < n) {
      const line = lines[i];
      if (RE_BLANK.test(line)) { i++; continue; }
      // Blockquote
      if (RE_BQ.test(line)) {
        const inner = [];
        while (i < n && (RE_BQ.test(lines[i]) || (!RE_BLANK.test(lines[i]) && inner.length && !isBlockStart(lines[i])))) {
          if (RE_BLANK.test(lines[i])) break;
          inner.push(lines[i].replace(RE_BQ, ""));
          i++;
        }
        nodes.push({ type: "blockquote", children: parseBlocks(inner) });
        continue;
      }
      // ATX heading
      let m = RE_ATX.exec(line);
      if (m) {
        nodes.push({ type: "heading", level: m[1].length, text: (m[2] || "").trim() });
        i++;
        continue;
      }
      // Fenced code
      m = RE_FENCE.exec(line);
      if (m) {
        const fence = m[2][0];
        const flen = m[2].length;
        const indent = m[1].length;
        const lang = m[3].split(/[ \t]/)[0] || "";
        const body = [];
        i++;
        while (i < n) {
          const cl = lines[i];
          const cm = new RegExp("^ {0,3}" + fence + "{" + flen + ",}[ \\t]*$").exec(cl);
          if (cm) { i++; break; }
          body.push(indent > 0 ? cl.replace(new RegExp("^ {0," + indent + "}"), "") : cl);
          i++;
        }
        nodes.push({ type: "code", lang, literal: body.join("\n") });
        continue;
      }
      // HR
      if (RE_HR.test(line)) { nodes.push({ type: "hr" }); i++; continue; }
      // Table
      if (line.includes("|") && i + 1 < n && isTableDelim(lines[i + 1])) {
        const header = splitRow(line);
        const delim = splitRow(lines[i + 1]);
        const align = delim.map((d) => {
          const l = d.startsWith(":"), r = d.endsWith(":");
          if (l && r) return "center";
          if (r) return "right";
          if (l) return "left";
          return "default";
        });
        i += 2;
        const rows = [];
        while (i < n && !RE_BLANK.test(lines[i]) && lines[i].includes("|") && !isBlockStart(lines[i])) {
          rows.push(splitRow(lines[i]));
          i++;
        }
        nodes.push({ type: "table", align, header, rows });
        continue;
      }
      // List
      m = RE_LIST.exec(line);
      if (m) {
        const parsed = parseList(lines, i);
        nodes.push(parsed.node);
        i = parsed.next;
        continue;
      }
      // Paragraph
      const para = [];
      while (i < n && !RE_BLANK.test(lines[i]) && !isBlockStart(lines[i])) {
        para.push(lines[i]);
        i++;
      }
      nodes.push({ type: "paragraph", text: para.join("\n") });
    }
    return nodes;
  }

  function isBlockStart(line) {
    return RE_ATX.test(line) || RE_FENCE.test(line) || RE_HR.test(line) || RE_BQ.test(line) || RE_LIST.test(line);
  }

  function markerKind(marker) {
    return /\d/.test(marker) ? "ol" : marker;
  }

  function parseList(lines, start) {
    const n = lines.length;
    let i = start;
    const first = RE_LIST.exec(lines[i]);
    const ordered = /\d/.test(first[2]);
    const kind = markerKind(first[2]);
    const startNum = ordered ? parseInt(first[2], 10) : 0;
    const items = [];
    let loose = false;
    while (i < n) {
      const m = RE_LIST.exec(lines[i]);
      if (!m || markerKind(m[2]) !== kind) break;
      const markerWidth = m[1].length + m[2].length + (m[3].length || 1);
      const itemLines = [];
      // first line remainder
      itemLines.push(m[4]);
      i++;
      // continuation lines: blanks or indented >= markerWidth
      while (i < n) {
        if (RE_BLANK.test(lines[i])) {
          // Only fold blank lines into the item if a following line is
          // indented enough to continue it; otherwise leave them for the
          // outer loose-list detection.
          let j = i;
          while (j < n && RE_BLANK.test(lines[j])) j++;
          if (j < n && /^( *)/.exec(lines[j])[1].length >= markerWidth) {
            while (i < j) { itemLines.push(""); i++; }
            continue;
          }
          break;
        }
        const indentMatch = /^( *)/.exec(lines[i])[1].length;
        if (indentMatch >= markerWidth) {
          itemLines.push(lines[i].slice(markerWidth));
          i++;
        } else {
          break;
        }
      }
      // trim trailing blank lines from item, note if present (loose)
      while (itemLines.length && RE_BLANK.test(itemLines[itemLines.length - 1])) {
        itemLines.pop();
      }
      // task marker
      let task = null;
      let firstText = itemLines[0] || "";
      const tm = /^\[([ xX])\][ \t]+(.*)$/.exec(firstText);
      if (tm) {
        task = tm[1] === " " ? "unchecked" : "checked";
        itemLines[0] = tm[2];
      }
      items.push({ children: parseBlocks(itemLines), task });
      // Check blank separator before next item -> loose
      if (i < n && RE_BLANK.test(lines[i])) {
        let j = i;
        while (j < n && RE_BLANK.test(lines[j])) j++;
        if (j < n) {
          const nm = RE_LIST.exec(lines[j]);
          if (nm && markerKind(nm[2]) === kind) { loose = true; i = j; continue; }
        }
        break;
      }
    }
    return { node: { type: "list", ordered, start: startNum, tight: !loose, items, kind }, next: i };
  }

  // ---------------- Inline parser ----------------
  function parseInline(s) {
    // Returns array of inline nodes.
    const nodes = [];
    let i = 0;
    let buf = "";
    const flush = () => { if (buf) { pushText(nodes, buf); buf = ""; } };
    while (i < s.length) {
      const c = s[i];
      if (c === "\\" && i + 1 < s.length && /[!-/:-@[-`{-~]/.test(s[i + 1])) {
        buf += s[i + 1]; i += 2; continue;
      }
      if (c === "`") {
        let n2 = 1;
        while (s[i + n2] === "`") n2++;
        const close = s.indexOf("`".repeat(n2), i + n2);
        // ensure exact run length
        let ci = i + n2;
        let found = -1;
        while (ci <= s.length - n2) {
          if (s.slice(ci, ci + n2) === "`".repeat(n2) && s[ci + n2] !== "`" && s[ci - 1] !== "`") { found = ci; break; }
          ci++;
        }
        if (found !== -1) {
          flush();
          let content = s.slice(i + n2, found);
          content = content.replace(/\n/g, " ");
          if (content.length > 2 && content[0] === " " && content[content.length - 1] === " " && content.trim() !== "") {
            content = content.slice(1, -1);
          }
          nodes.push({ type: "code", value: content });
          i = found + n2;
          continue;
        }
        buf += c; i++; continue;
      }
      if (c === "!" && s[i + 1] === "[") {
        const link = parseLink(s, i + 1, true);
        if (link) { flush(); nodes.push(link.node); i = link.next; continue; }
        buf += c; i++; continue;
      }
      if (c === "[") {
        if (s[i + 1] === "[") {
          const end = s.indexOf("]]", i + 2);
          if (end !== -1) {
            flush();
            nodes.push({ type: "wikilink", target: s.slice(i + 2, end) });
            i = end + 2;
            continue;
          }
          // never closes: literal, emit one '[' and continue
          buf += "[";
          i++;
          continue;
        }
        const link = parseLink(s, i, false);
        if (link) { flush(); nodes.push(link.node); i = link.next; continue; }
        buf += c; i++; continue;
      }
      if (c === "<") {
        const auto = parseAutolink(s, i);
        if (auto) { flush(); nodes.push(auto.node); i = auto.next; continue; }
        buf += c; i++; continue;
      }
      if (c === "*" || c === "_" || c === "~") {
        flush();
        let n2 = 1;
        while (s[i + n2] === c) n2++;
        nodes.push({ type: "delim", ch: c, count: n2, before: i > 0 ? s[i - 1] : "", after: i + n2 < s.length ? s[i + n2] : "", raw: c.repeat(n2) });
        i += n2;
        continue;
      }
      buf += c;
      i++;
    }
    flush();
    resolveEmphasis(nodes);
    return nodes;
  }

  // Split plain text into text + bare-URL autolinks (GFM).
  function pushText(nodes, text) {
    const re = /(https?:\/\/[^\s<]+|www\.[^\s<]+|[A-Za-z0-9.!#$%&'*+/=?^_`{|}~-]+@[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?(?:\.[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?)+)/g;
    let last = 0;
    let m;
    while ((m = re.exec(text))) {
      // GFM does not link `texthttp://…`: a bare autolink needs a text
      // boundary before its scheme/www form (the email alternative owns its
      // complete local part, so it is unaffected by this check).
      if (m.index > 0 && /[A-Za-z0-9]/.test(text[m.index - 1])) continue;
      if (m.index > last) nodes.push({ type: "text", value: text.slice(last, m.index) });
      let url = m[1];
      // Strip trailing punctuation with a single index walk. Paren counts are
      // computed once and decremented as ')' are trimmed, so a URL ending in a
      // long run of ')' stays linear instead of O(n^2) (rescanning per char).
      let end = url.length;
      let opens = -1, closes = -1;
      while (end > 0) {
        const ch = url[end - 1];
        if (ch === ")") {
          if (opens === -1) {
            opens = 0; closes = 0;
            for (let k = 0; k < end; k++) { const kc = url[k]; if (kc === "(") opens++; else if (kc === ")") closes++; }
          }
          if (closes <= opens) break;
          closes--; end--;
        } else if (ch === "!" || ch === "." || ch === "," || ch === ";" || ch === ":" || ch === "?") {
          end--;
        } else break;
      }
      // A complete HTML entity suffix belongs to the following text, not the
      // GFM autolink (`...?q=commonmark&hl;` → link + `&amp;hl;`).
      const entity = /&[A-Za-z][A-Za-z0-9]*;$/.exec(url.slice(0, end));
      if (entity) end -= entity[0].length;
      const trail = url.slice(end);
      url = url.slice(0, end);
      const email = url.includes("@") && !url.includes("://");
      nodes.push({ type: "autolink", href: email ? url : (url.startsWith("www.") ? "http://" + url : url), kind: email ? "email" : "url", text: url, bare: true });
      if (trail) nodes.push({ type: "text", value: trail });
      last = m.index + m[1].length;
    }
    if (last < text.length) nodes.push({ type: "text", value: text.slice(last) });
  }

  function parseLink(s, start, isImage) {
    // s[start] === '['
    let i = start + 1;
    let depth = 1;
    let text = "";
    while (i < s.length && depth > 0) {
      if (s[i] === "\\" && i + 1 < s.length) { text += s[i] + s[i + 1]; i += 2; continue; }
      if (s[i] === "[") depth++;
      else if (s[i] === "]") { depth--; if (depth === 0) break; }
      text += s[i];
      i++;
    }
    if (depth !== 0) return null;
    i++; // past ]
    if (s[i] !== "(") return null;
    i++;
    // dest
    let href = "";
    let title = "";
    while (i < s.length && /[ \t]/.test(s[i])) i++;
    if (s[i] === "<") {
      i++;
      // An angle destination ends at '>' and may not contain an unescaped
      // '<' or line ending (cmark parity). The '<' bound also keeps a flood
      // of "[a](<b" repeats linear instead of O(n^2).
      while (i < s.length && s[i] !== ">" && s[i] !== "\n" && s[i] !== "<") {
        if (s[i] === "\\" && i + 1 < s.length) { href += s[i + 1]; i += 2; continue; }
        href += s[i]; i++;
      }
      if (s[i] !== ">") return null;
      i++;
    } else {
      let pdepth = 0;
      while (i < s.length && !/[ \t]/.test(s[i]) && !(s[i] === ")" && pdepth === 0)) {
        // Parenthesis nesting is capped at 32 (cmark parity); past the cap the
        // candidate is not a link. This also keeps a flood of "[a](b" repeats
        // linear instead of O(n^2).
        if (s[i] === "(") { if (++pdepth > 32) return null; }
        else if (s[i] === ")") pdepth--;
        href += s[i];
        i++;
      }
    }
    while (i < s.length && /[ \t]/.test(s[i])) i++;
    if (s[i] === '"' || s[i] === "'" || s[i] === "(") {
      const openCh = s[i];
      const close = openCh === "(" ? ")" : openCh;
      i++;
      while (i < s.length && s[i] !== close) {
        // A '('-delimited title may not contain an unescaped '(' (cmark
        // parity); this also bounds a "[ (](" flood.
        if (openCh === "(" && s[i] === "(") return null;
        title += s[i]; i++;
      }
      if (s[i] === close) i++;
    }
    while (i < s.length && /[ \t]/.test(s[i])) i++;
    if (s[i] !== ")") return null;
    i++;
    if (isImage) {
      return { node: { type: "image", href, title, children: parseInline(text) }, next: i };
    }
    return { node: { type: "link", href, title, children: parseInline(text) }, next: i };
  }

  function parseAutolink(s, start) {
    // Autolink content has no whitespace or unescaped '<', so bail at the
    // first such char instead of scanning to EOF for a '>' that may never
    // appear. This keeps a flood of unterminated "<!--" openers linear.
    let end = -1;
    for (let j = start + 1; j < s.length; j++) {
      const ch = s[j];
      if (ch === ">") { end = j; break; }
      if (ch === "<" || ch === " " || ch === "\t" || ch === "\n") return null;
    }
    if (end === -1) return null;
    const inner = s.slice(start + 1, end);
    if (/^[a-zA-Z][a-zA-Z0-9+.-]*:[^\s<>]*$/.test(inner)) {
      return { node: { type: "autolink", href: inner, kind: "url", text: inner }, next: end + 1 };
    }
    if (/^[^\s<>@]+@[^\s<>@]+\.[^\s<>@]+$/.test(inner)) {
      return { node: { type: "autolink", href: inner, kind: "email", text: inner }, next: end + 1 };
    }
    return null;
  }

  // Delimiter-run emphasis resolution over the flat node list (in place).
  function resolveEmphasis(nodes) {
    function isPunct(ch) { return ch !== "" && /[!-/:-@[-`{-~]/.test(ch); }
    function isWs(ch) { return ch === "" || /\s/.test(ch); }
    // compute canOpen/canClose per delim
    for (const nd of nodes) {
      if (nd.type !== "delim") continue;
      const beforeWs = isWs(nd.before), afterWs = isWs(nd.after);
      const beforePunct = isPunct(nd.before), afterPunct = isPunct(nd.after);
      const leftFlank = !afterWs && (!afterPunct || beforeWs || beforePunct);
      const rightFlank = !beforeWs && (!beforePunct || afterWs || afterPunct);
      if (nd.ch === "_") {
        nd.canOpen = leftFlank && (!rightFlank || beforePunct);
        nd.canClose = rightFlank && (!leftFlank || afterPunct);
      } else {
        nd.canOpen = leftFlank;
        nd.canClose = rightFlank;
      }
      nd.remaining = nd.count;
    }
    // process: scan for closer, match nearest opener
    let idx = 0;
    while (idx < nodes.length) {
      const closer = nodes[idx];
      if (!closer || closer.type !== "delim" || !closer.canClose || closer.remaining === 0) { idx++; continue; }
      // find opener
      let openIdx = -1;
      for (let j = idx - 1; j >= 0; j--) {
        const o = nodes[j];
        if (!o || o.type !== "delim") continue;
        if (o.ch !== closer.ch || !o.canOpen || o.remaining === 0) continue;
        openIdx = j;
        break;
      }
      if (openIdx === -1) { idx++; continue; }
      const opener = nodes[openIdx];
      const use = closer.ch === "~" ? 2 : Math.min(2, Math.min(opener.remaining, closer.remaining));
      if (closer.ch === "~" && (opener.remaining < 2 || closer.remaining < 2)) { idx++; continue; }
      const type = use === 2 ? (closer.ch === "~" ? "del" : "strong") : "em";
      const inner = nodes.slice(openIdx + 1, idx).filter((x) => !(x.type === "delim" && x.remaining === 0 && !x._kept));
      const innerNodes = nodes.slice(openIdx + 1, idx);
      resolveEmphasis(innerNodes);
      const node = { type, children: innerNodes.filter((x) => x.type !== "delim" || x.remaining > 0 ? x.type !== "delim" : false) };
      node.children = cleanDelims(innerNodes);
      opener.remaining -= use;
      closer.remaining -= use;
      // replace range (openIdx+1 .. idx) plus consumed delim chars
      const before = [];
      if (opener.remaining > 0) opener.raw = opener.ch.repeat(opener.remaining);
      const replacement = [node];
      nodes.splice(openIdx + 1, idx - openIdx, ...replacement);
      // opener now at openIdx; if consumed fully mark
      idx = openIdx;
      // continue scanning from opener
    }
  }

  function cleanDelims(list) {
    const out = [];
    for (const nd of list) {
      if (nd.type === "delim") {
        if (nd.remaining > 0) out.push({ type: "text", value: nd.ch.repeat(nd.remaining) });
        continue;
      }
      out.push(nd);
    }
    return out;
  }

  function finalizeInline(nodes) {
    return cleanDelims(nodes);
  }

  // ---------------- Renderer ----------------
  const ANSI = {
    BOLD: "\x1b[1m", ITALIC: "\x1b[3m", UNDERLINE: "\x1b[4m", STRIKE: "\x1b[9m",
    DIM: "\x1b[2m", RESET: "\x1b[0m",
    RED: "\x1b[31m", GREEN: "\x1b[32m", YELLOW: "\x1b[33m", BLUE: "\x1b[34m",
    MAGENTA: "\x1b[35m", CYAN: "\x1b[36m", WHITE: "\x1b[37m",
  };
  const SPAN_EM = 1, SPAN_STRONG = 2, SPAN_DEL = 4, SPAN_U = 8, SPAN_CODE = 16;
  const MAX_INDENT_COLS = 128;

  function headingColor(l) {
    return [ANSI.MAGENTA, ANSI.MAGENTA, ANSI.CYAN, ANSI.YELLOW, ANSI.GREEN, ANSI.BLUE, ANSI.WHITE][l] || ANSI.WHITE;
  }
  function codeSpanOpen(light) { return light ? "\x1b[48;5;254m\x1b[38;5;124m" : "\x1b[48;5;236m\x1b[38;5;215m"; }

  class Renderer {
    constructor(theme) {
      this.theme = theme;
      this.out = [];
      this.quoteDepth = 0;
      this.listIndentCols = 0;
      this.blockStack = [];
      this.spanFlags = 0;
      this.linkHref = null;
      this.linkDepth = 0;
      this.imageDepth = 0;
      this.imageAlt = "";
      this.imageSrc = null;
      this.imageTitle = null;
      this.col = 0;
      this.inCode = false;
      this.codeLang = "";
      this.codeFenced = false;
      this.codeBuf = "";
      this.headingLevel = 0;
      this.headingBuf = "";
      this.tableCells = [];
      this.tableRows = [];
      this.tableCellBuf = "";
      this.inThead = false;
      this.inCell = false;
      this.cellAlign = "default";
      this.lastWasNewline = true;
      this.blankEmitted = false;
    }
    output() { return this.out.join(""); }
    w(s) { if (s) this.out.push(s); }

    // ---- block ----
    enterBlock(t, data, flags) {
      const c = this.theme.colors;
      switch (t) {
        case "quote": this.ensureBlankLine(); this.pushBlock({ kind: "quote", indent: 2 }); break;
        case "ul": this.ensureNewline(); this.pushBlock({ kind: "ul", data, index: 0, indent: 2 }); break;
        case "ol": this.ensureNewline(); this.pushBlock({ kind: "ol", data, index: 0, indent: 3 }); break;
        case "li": {
          this.ensureNewline();
          this.writeIndent();
          const entry = { kind: "li", index: 0, indent: 0 };
          const parentIdx = this.findParentList();
          const task = data && data.task;
          if (parentIdx !== -1) { entry.index = this.blockStack[parentIdx].index; this.blockStack[parentIdx].index++; }
          let glyph, color;
          if (task) {
            const checked = task === "checked";
            glyph = c ? (checked ? "☒ " : "☐ ") : (checked ? "[x] " : "[ ] ");
            color = checked ? ANSI.GREEN : ANSI.DIM;
          } else if (parentIdx !== -1 && this.blockStack[parentIdx].kind === "ol") {
            const num = this.blockStack[parentIdx].data + entry.index;
            glyph = num + ". ";
            color = ANSI.CYAN;
          } else {
            glyph = c ? "• " : "* ";
            color = ANSI.CYAN;
          }
          this.writeStyled(color, glyph);
          this.writeStyled(ANSI.RESET, "");
          entry.indent = W(glyph);
          this.pushBlock(entry);
          break;
        }
        case "hr": {
          this.ensureBlankLine();
          this.writeIndent();
          const indentCols = this.currentIndent();
          let width = this.theme.columns === 0 ? Math.max(0, 60 - indentCols) : Math.max(0, Math.min(this.theme.columns, 60) - indentCols);
          const dash = c ? "─" : "-";
          this.writeStyled(ANSI.DIM, "");
          for (let k = 0; k < width; k++) this.wRaw(dash);
          this.writeStyled(ANSI.RESET, "");
          this.wRaw("\n"); this.lastWasNewline = true; this.col = 0;
          break;
        }
        case "h": this.ensureBlankLine(); this.headingLevel = data; this.headingBuf = ""; break;
        case "code":
          this.ensureBlankLine();
          this.inCode = true;
          this.codeFenced = !!(flags && flags.fenced);
          this.codeBuf = "";
          this.codeLang = flags && flags.lang ? flags.lang : "";
          break;
        case "p": {
          const top = this.blockStack.length ? this.blockStack[this.blockStack.length - 1].kind : null;
          if (top === "li" && this.col > 0) { /* same line */ } else { this.ensureBlankLine(); this.writeIndent(); }
          break;
        }
        case "table": this.ensureBlankLine(); this.inThead = false; this.tableRows = []; this.tableCells = []; break;
        case "thead": this.inThead = true; break;
        case "tbody": this.inThead = false; break;
        case "tr": this.tableCells = []; break;
        case "th": case "td": this.inCell = true; this.cellAlign = data && data.align || "default"; this.tableCellBuf = ""; break;
      }
    }
    leaveBlock(t) {
      switch (t) {
        case "quote": case "ul": case "ol": case "li": this.popBlock(); this.ensureNewline(); break;
        case "h": this.flushHeading(); this.headingLevel = 0; break;
        case "code": this.flushCode(); this.inCode = false; this.codeLang = ""; break;
        case "p": this.writeStyled(ANSI.RESET, ""); this.ensureNewline(); this.col = 0; break;
        case "table": this.flushTable(); this.ensureNewline(); break;
        case "tr": {
          this.tableRows.push({ cells: this.tableCells.slice(), isHeader: this.inThead });
          this.tableCells = [];
          break;
        }
        case "th": case "td":
          this.inCell = false;
          this.tableCells.push({ content: this.tableCellBuf, alignment: this.cellAlign });
          break;
      }
    }

    // ---- span ----
    enterSpan(t, detail) {
      switch (t) {
        case "em": this.spanFlags |= SPAN_EM; this.writeStyled(ANSI.ITALIC, ""); break;
        case "strong": this.spanFlags |= SPAN_STRONG; this.writeStyled(ANSI.BOLD, ""); break;
        case "u": this.spanFlags |= SPAN_U; this.writeStyled(ANSI.UNDERLINE, ""); break;
        case "del": this.spanFlags |= SPAN_DEL; this.writeStyled(ANSI.STRIKE, ""); break;
        case "code": this.spanFlags |= SPAN_CODE; this.writeStyled(codeSpanOpen(this.theme.light), ""); break;
        case "a": {
          this.linkDepth++;
          if (this.linkDepth === 1) {
            this.linkHref = detail.href;
            if (this.theme.colors && this.theme.hyperlinks && this.linkHref) {
              this.wRawNoColor("\x1b]8;;"); this.wRawNoColor(this.linkHref); this.wRawNoColor("\x1b\\");
            }
            this.writeStyled(ANSI.BLUE, ""); this.writeStyled(ANSI.UNDERLINE, "");
          }
          break;
        }
        case "img": {
          this.imageDepth++;
          if (this.imageDepth === 1) { this.imageSrc = detail.href; this.imageTitle = detail.title || ""; this.imageAlt = ""; }
          break;
        }
        case "wikilink": this.writeStyled(ANSI.BLUE, "[["); break;
      }
    }
    leaveSpan(t) {
      switch (t) {
        case "em": this.spanFlags &= ~SPAN_EM; this.writeStyled("\x1b[23m", ""); if (this.headingLevel > 0) this.reapplyStyles(); break;
        case "strong": this.spanFlags &= ~SPAN_STRONG; this.writeStyled("\x1b[22m", ""); if (this.headingLevel > 0) this.reapplyStyles(); break;
        case "u": this.spanFlags &= ~SPAN_U; this.writeStyled("\x1b[24m", ""); if (this.headingLevel > 0) this.reapplyStyles(); break;
        case "del": this.spanFlags &= ~SPAN_DEL; this.writeStyled("\x1b[29m", ""); if (this.headingLevel > 0) this.reapplyStyles(); break;
        case "code": this.spanFlags &= ~SPAN_CODE; this.writeStyled("\x1b[39m\x1b[49m", ""); this.reapplyStyles(); break;
        case "a": {
          if (this.linkDepth === 1) {
            this.linkDepth = 0;
            const hadHref = this.linkHref != null;
            this.writeStyled("\x1b[24m\x1b[39m", "");
            this.reapplyStyles();
            if (this.theme.colors && this.theme.hyperlinks) {
              if (hadHref) this.wRawNoColor("\x1b]8;;\x1b\\");
            } else if (this.linkHref) {
              const href = this.linkHref;
              if (href && this.imageDepth === 0) {
                this.writeStyled(ANSI.DIM, " (");
                this.writeStyled("", href);
                this.writeStyled(ANSI.DIM, ")");
                this.writeStyled("\x1b[39m\x1b[22m", "");
                this.reapplyStyles();
              }
            }
            this.linkHref = null;
          } else if (this.linkDepth > 0) this.linkDepth--;
          break;
        }
        case "img": {
          if (this.imageDepth === 1) { this.emitImage(); this.imageSrc = null; this.imageTitle = null; this.imageAlt = ""; }
          if (this.imageDepth > 0) this.imageDepth--;
          break;
        }
        case "wikilink": this.writeNoWrap("]]"); this.writeStyled("\x1b[39m", ""); this.reapplyStyles(); break;
      }
    }

    // ---- text ----
    text(tt, content) {
      switch (tt) {
        case "br": this.writeContent("\n"); break;
        case "softbr": this.writeContent(" "); break;
        case "code": this.writeStyled("", content); break;
        default: this.writeContent(content);
      }
    }

    writeContent(data) {
      if (this.imageDepth > 0) { this.imageAlt += data; return; }
      if (this.inCode) { this.codeBuf += data; return; }
      if (this.headingLevel > 0) { this.headingBuf += data; return; }
      if (this.inCell) { this.tableCellBuf += data; return; }
      this.writeWrapped(data);
    }

    writeWrapped(data) {
      if (this.theme.columns === 0) {
        let start = 0;
        for (let i = 0; i < data.length; i++) {
          if (data[i] === "\n") { this.wRaw(data.slice(start, i + 1)); this.col = 0; this.lastWasNewline = true; this.writeIndent(); start = i + 1; }
        }
        if (start < data.length) { this.wRaw(data.slice(start)); this.updateColFromText(data.slice(start)); }
        return;
      }
      const indent = this.currentIndent();
      const max = this.theme.columns;
      let i = 0;
      while (i < data.length) {
        const c = data[i];
        if (c === "\n") { this.wRaw("\n"); this.lastWasNewline = true; this.col = 0; i++; this.writeIndent(); continue; }
        if (c === " " && this.col >= max) { this.wRaw("\n"); this.lastWasNewline = true; this.col = 0; this.writeIndent(); i++; while (i < data.length && data[i] === " ") i++; continue; }
        let j = i;
        while (j < data.length && data[j] !== " " && data[j] !== "\n") j++;
        const word = data.slice(i, j);
        const wordWidth = W(word);
        const avail = Math.max(0, max - indent);
        if (avail > 0 && wordWidth > avail) {
          let rest = word;
          while (rest.length) {
            const r = Math.max(0, max - this.col);
            if (r === 0) { this.wrapBreak(); continue; }
            let cut = visIndexAt(rest, r);
            if (cut === 0) { const cp = rest.codePointAt(0); cut = cp > 0xffff ? 2 : 1; }
            this.wRaw(rest.slice(0, cut)); this.col += W(rest.slice(0, cut)); this.lastWasNewline = false;
            rest = rest.slice(cut);
            if (rest.length) this.wrapBreak();
          }
        } else {
          if (this.col !== 0 && this.col + wordWidth > max && this.col > indent) this.wrapBreak();
          this.wRaw(word); this.col += wordWidth; this.lastWasNewline = word.length === 0;
        }
        i = j;
        if (i < data.length && data[i] === " ") {
          let k = i;
          while (k < data.length && data[k] === " ") k++;
          let m2 = k;
          while (m2 < data.length && data[m2] !== " " && data[m2] !== "\n") m2++;
          const nextWordWidth = W(data.slice(k, m2));
          const nextAvail = Math.max(0, max - indent);
          if (this.col !== 0 && this.col + 1 + nextWordWidth > max && this.col > indent && nextWordWidth <= nextAvail) {
            this.wRaw("\n"); this.lastWasNewline = true; this.col = 0; this.writeIndent();
          } else { this.wRaw(" "); this.col += 1; }
          i = k;
        }
      }
    }

    emitInline(bytes) {
      if (!bytes) return;
      if (this.imageDepth > 0) { this.imageAlt += stripAnsi(bytes); return; }
      if (this.inCell) { this.tableCellBuf += bytes; return; }
      if (this.headingLevel > 0) { this.headingBuf += bytes; return; }
      this.w(bytes);
    }

    writeStyled(prefix, text) {
      const inMain = !this.inCell && this.headingLevel === 0 && !this.inCode && this.imageDepth === 0;
      if (inMain && this.theme.columns > 0 && text) {
        const tw = W(text);
        if (tw > 0) {
          const max = this.theme.columns, indent = this.currentIndent();
          if (this.col > indent && this.col + tw > max) this.wrapBreak();
        }
      }
      if (this.theme.colors && prefix) this.emitInline(prefix);
      if (!text) return;
      if (!inMain) { this.emitInline(text); return; }
      const max = this.theme.columns;
      if (max === 0) { this.emitInline(text); this.col += W(text); this.lastWasNewline = false; return; }
      let rest = text;
      while (rest.length) {
        const room = Math.max(0, max - this.col);
        if (room === 0) {
          if (this.col <= this.currentIndent()) { this.emitInline(rest); this.col += W(rest); this.lastWasNewline = false; return; }
          this.wrapBreak(); continue;
        }
        const cut = visIndexAt(rest, room);
        if (cut === rest.length) { this.emitInline(rest); this.col += W(rest); this.lastWasNewline = false; return; }
        if (cut === 0) {
          if (this.col <= this.currentIndent()) {
            let one = visIndexAt(rest, 2);
            if (one === 0) { const cp = rest.codePointAt(0); one = cp > 0xffff ? 2 : 1; }
            this.emitInline(rest.slice(0, one)); this.col += W(rest.slice(0, one)); this.lastWasNewline = false;
            rest = rest.slice(one);
            if (rest.length) this.wrapBreak();
            continue;
          }
          this.wrapBreak(); continue;
        }
        this.emitInline(rest.slice(0, cut)); this.col += W(rest.slice(0, cut)); this.lastWasNewline = false;
        rest = rest.slice(cut);
        this.wrapBreak();
      }
    }

    wrapBreak() {
      const hasStyle = this.spanFlags !== 0 || this.linkDepth > 0;
      if (this.theme.colors && hasStyle) this.w("\x1b[39m\x1b[49m");
      this.w("\n"); this.lastWasNewline = true; this.col = 0;
      this.writeIndent();
      if (hasStyle) this.reapplyStyles();
    }

    wRaw(data) { if (!data) return; this.emitInline(data); this.lastWasNewline = data[data.length - 1] === "\n"; }
    writeNoWrap(text) {
      if (!text) return;
      this.emitInline(text);
      if (!this.inCell && this.headingLevel === 0 && !this.inCode && this.imageDepth === 0) { this.col += W(text); this.lastWasNewline = false; }
    }
    wRawNoColor(data) {
      if (!this.theme.colors || !data) return;
      if (this.imageDepth > 0) return;
      if (this.inCell) { this.tableCellBuf += data; return; }
      if (this.headingLevel > 0) { this.headingBuf += data; return; }
      this.w(data);
    }

    reapplyStyles() {
      if (!this.theme.colors) return;
      if (this.headingLevel > 0) { this.emitInline(ANSI.BOLD); this.emitInline(headingColor(this.headingLevel)); }
      if (this.spanFlags & SPAN_STRONG) this.emitInline(ANSI.BOLD);
      if (this.spanFlags & SPAN_EM) this.emitInline(ANSI.ITALIC);
      if (this.spanFlags & SPAN_U) this.emitInline(ANSI.UNDERLINE);
      if (this.spanFlags & SPAN_DEL) this.emitInline(ANSI.STRIKE);
      if (this.spanFlags & SPAN_CODE) this.emitInline(codeSpanOpen(this.theme.light));
      if (this.linkDepth > 0) { this.emitInline(ANSI.BLUE); this.emitInline(ANSI.UNDERLINE); }
    }

    pushBlock(entry) {
      if (entry.kind === "quote") this.quoteDepth++;
      else this.listIndentCols += entry.indent || 0;
      this.blockStack.push(entry);
    }
    popBlock() {
      const e = this.blockStack.pop();
      if (!e) return;
      if (e.kind === "quote") this.quoteDepth--;
      else this.listIndentCols -= e.indent || 0;
    }
    indentCounts() {
      const quoteBars = Math.min(this.quoteDepth, MAX_INDENT_COLS / 2);
      const other = Math.min(this.listIndentCols, MAX_INDENT_COLS - quoteBars * 2);
      return [quoteBars, other];
    }
    writeIndent() {
      this.blankEmitted = false;
      const [quoteBars, other] = this.indentCounts();
      const bar = this.theme.colors ? "│ " : "| ";
      if (this.theme.colors && quoteBars > 0) this.w("\x1b[38;5;242m");
      for (let i = 0; i < quoteBars; i++) { this.w(bar); this.col += 2; }
      if (this.theme.colors && quoteBars > 0) { this.w("\x1b[39m"); this.reapplyStyles(); }
      for (let j = 0; j < other; j++) { this.w(" "); this.col += 1; }
    }
    currentIndent() { const [q, o] = this.indentCounts(); return q * 2 + o; }
    updateColFromText(data) {
      let start = 0;
      for (let i = 0; i < data.length; i++) if (data[i] === "\n") { this.col = 0; this.lastWasNewline = true; start = i + 1; }
      if (start < data.length) { this.col += W(data.slice(start)); this.lastWasNewline = false; }
    }
    writeQuoteBars() {
      const [quoteBars] = this.indentCounts();
      if (quoteBars === 0) return;
      const bar = this.theme.colors ? "│" : "|";
      if (this.theme.colors) this.w("\x1b[38;5;242m");
      for (let i = 0; i < quoteBars; i++) { this.w(bar); this.col += 1; }
      if (this.theme.colors) this.w("\x1b[39m");
    }
    ensureNewline() { if (!this.lastWasNewline) { this.w("\n"); this.col = 0; this.lastWasNewline = true; } }
    ensureBlankLine() {
      this.ensureNewline();
      if (this.blankEmitted) return;
      if (this.out.length) {
        const joined = this.out;
        // Check last two output chars
        const s = this.tail(2);
        if (s.length >= 2 && s[s.length - 1] === "\n" && s[s.length - 2] !== "\n") {
          this.writeQuoteBars(); this.w("\n"); this.col = 0; this.blankEmitted = true;
        } else if (this.totalLen() === 1 && s === "\n") {
          // single newline, skip
        } else if (s.length >= 1 && s[s.length - 1] !== "\n") {
          this.writeQuoteBars(); this.w("\n"); this.col = 0; this.blankEmitted = true;
        }
      }
    }
    tail(n) { let s = ""; for (let i = this.out.length - 1; i >= 0 && s.length < n + 4; i--) s = this.out[i] + s; return s.slice(-n); }
    totalLen() { let t = 0; for (const p of this.out) t += p.length; return t; }
    findParentList() {
      for (let i = this.blockStack.length - 1; i >= 0; i--) { const k = this.blockStack[i].kind; if (k === "ul" || k === "ol") return i; }
      return -1;
    }

    flushHeading() {
      const level = this.headingLevel;
      this.headingLevel = 0;
      const content = this.headingBuf;
      this.headingBuf = "";
      this.writeIndent();
      if (this.theme.colors) { this.w(ANSI.BOLD); this.w(headingColor(level)); }
      this.w(content);
      if (this.theme.colors) this.w(ANSI.RESET);
      this.w("\n"); this.lastWasNewline = true; this.col = 0;
      if (level === 1 || level === 2) {
        this.writeIndent();
        const textW = Math.max(W(content), 3);
        const indentCols = this.currentIndent();
        const width = this.theme.columns === 0 ? textW : Math.min(textW, Math.max(0, this.theme.columns - indentCols));
        if (this.theme.colors) this.w(ANSI.DIM);
        const ch = this.theme.colors ? (level === 1 ? "═" : "─") : (level === 1 ? "=" : "-");
        for (let i = 0; i < width; i++) this.w(ch);
        if (this.theme.colors) this.w(ANSI.RESET);
        this.w("\n"); this.lastWasNewline = true; this.col = 0;
      }
      this.headingLevel = level;
    }

    flushCode() {
      const src = this.codeBuf;
      const body = src.endsWith("\n") ? src.slice(0, -1) : src;
      const c = this.theme.colors;
      const topBorder = c ? "┌─ " : "+- ";
      const topBare = c ? "┌─" : "+-";
      const side = c ? "│ " : "| ";
      const bottom = c ? "└─" : "+-";
      if (c) this.w(ANSI.DIM);
      this.writeIndent();
      const badge = this.codeLang || "";
      if (badge) {
        this.w(topBorder);
        if (c) this.w(ANSI.RESET);
        if (c) this.w("\x1b[2m\x1b[3m");
        this.w(badge);
        if (c) this.w(ANSI.RESET);
      } else {
        if (c) this.w(ANSI.DIM);
        this.w(topBare);
        if (c) this.w(ANSI.RESET);
      }
      this.w("\n"); this.lastWasNewline = true;
      const isJs = isJsLang(this.codeLang);
      const lines = body.split("\n");
      // body of "" -> one empty line? Rust iterates i<=len, so a trailing produces. Match: split gives [""] for "".
      for (const line of lines) {
        this.writeIndent();
        if (c) this.w(ANSI.DIM);
        this.w(side);
        if (c) this.w(ANSI.RESET);
        if (isJs && c) this.w(highlightJs(line));
        else this.w(line);
        this.w("\n"); this.lastWasNewline = true;
      }
      this.writeIndent();
      if (c) this.w(ANSI.DIM);
      this.w(bottom);
      if (c) this.w(ANSI.RESET);
      this.w("\n"); this.col = 0; this.lastWasNewline = true;
    }

    flushTable() {
      if (!this.tableRows.length) return;
      let colCount = 0;
      for (const r of this.tableRows) colCount = Math.max(colCount, r.cells.length);
      if (colCount === 0) return;
      const widths = new Array(colCount).fill(3);
      const aligns = new Array(colCount).fill("default");
      for (const r of this.tableRows) {
        r.cells.forEach((cell, i) => {
          widths[i] = Math.max(widths[i], W(cell.content));
          if (aligns[i] === "default") aligns[i] = cell.alignment;
        });
      }
      if (this.theme.columns > 0) {
        const indent = this.currentIndent();
        let total = indent + 1;
        for (const wv of widths) total += wv + 3;
        const budget = this.theme.columns;
        while (total > budget) {
          let widest = 0;
          for (let i = 0; i < widths.length; i++) if (widths[i] > widths[widest]) widest = i;
          if (widths[widest] <= 3) break;
          widths[widest]--; total--;
        }
      }
      const ch = this.boxChars();
      const c = this.theme.colors;
      // top
      this.writeIndent();
      if (c) this.w(ANSI.DIM);
      this.w(ch.tl);
      widths.forEach((wv, i) => { for (let j = 0; j < wv + 2; j++) this.w(ch.h); this.w(i === widths.length - 1 ? ch.tr : ch.t); });
      if (c) this.w(ANSI.RESET);
      this.w("\n"); this.lastWasNewline = true;
      let hasSep = false;
      const rows = this.tableRows;
      this.tableRows = [];
      for (const row of rows) {
        this.writeRowCells(row, widths, aligns);
        if (row.isHeader && !hasSep) { this.writeTableSeparator(widths); hasSep = true; }
      }
      this.writeIndent();
      if (c) this.w(ANSI.DIM);
      this.w(ch.bl);
      widths.forEach((wv, i) => { for (let j = 0; j < wv + 2; j++) this.w(ch.h); this.w(i === widths.length - 1 ? ch.br : ch.b); });
      if (c) this.w(ANSI.RESET);
      this.w("\n"); this.lastWasNewline = true; this.col = 0;
    }

    writeRowCells(row, widths, aligns) {
      const ch = this.boxChars();
      const c = this.theme.colors;
      const segments = widths.map(() => []);
      const stateAt = widths.map(() => []);
      let lines = 1;
      for (let i = 0; i < widths.length; i++) {
        const wv = widths[i];
        const content = i < row.cells.length ? row.cells[i].content : "";
        let rest = content;
        let state = newCellState();
        while (rest.length) {
          let cut = visIndexAt(rest, wv);
          if (cut < rest.length) {
            const sp = lastWordBreakOutsideEscapes(rest.slice(0, cut));
            if (sp !== -1 && sp > 0) cut = sp;
          }
          if (cut === 0) { const cp = rest.codePointAt(0); cut = cp > 0xffff ? 2 : 1; }
          stateAt[i].push(cloneState(state));
          segments[i].push(rest.slice(0, cut));
          scanState(state, rest.slice(0, cut));
          rest = rest.slice(cut);
          let skipped = 0;
          while (skipped < rest.length && rest[skipped] === " ") skipped++;
          if (skipped > 0) { scanState(state, rest.slice(0, skipped)); rest = rest.slice(skipped); }
        }
        lines = Math.max(lines, segments[i].length);
      }
      for (let line = 0; line < lines; line++) {
        this.writeIndent();
        if (c) this.w(ANSI.DIM);
        this.w(ch.v);
        if (c) this.w(ANSI.RESET);
        for (let i = 0; i < widths.length; i++) {
          const wv = widths[i];
          const seg = line < segments[i].length ? segments[i][line] : "";
          const opens = line < stateAt[i].length ? stateAt[i][line] : newCellState();
          this.w(" ");
          if (row.isHeader && c) this.w(ANSI.BOLD);
          if (c && line > 0) this.w(emitOpens(opens));
          const cw = W(seg);
          const cellAlign = i < row.cells.length ? row.cells[i].alignment : "default";
          const alignment = cellAlign !== "default" ? cellAlign : aligns[i];
          const pad = Math.max(0, wv - cw);
          let left, right;
          if (alignment === "right") { left = pad; right = 0; }
          else if (alignment === "center") { left = pad >> 1; right = pad - (pad >> 1); }
          else { left = 0; right = pad; }
          for (let p = 0; p < left; p++) this.w(" ");
          this.w(seg);
          if (c) {
            const end = cloneState(opens);
            scanState(end, seg);
            this.w(emitCloses(end));
            if (row.isHeader) this.w(ANSI.RESET);
          }
          for (let p = 0; p < right; p++) this.w(" ");
          this.w(" ");
          if (c) this.w(ANSI.DIM);
          this.w(ch.v);
          if (c) this.w(ANSI.RESET);
        }
        this.w("\n");
      }
      this.lastWasNewline = true;
    }

    writeTableSeparator(widths) {
      const ch = this.boxChars();
      const c = this.theme.colors;
      this.writeIndent();
      if (c) this.w(ANSI.DIM);
      this.w(ch.ml);
      widths.forEach((wv, i) => { for (let j = 0; j < wv + 2; j++) this.w(ch.h); this.w(i === widths.length - 1 ? ch.mr : ch.x); });
      if (c) this.w(ANSI.RESET);
      this.w("\n"); this.lastWasNewline = true;
    }

    boxChars() {
      if (this.theme.colors)
        return { h: "─", v: "│", tl: "┌", tr: "┐", bl: "└", br: "┘", t: "┬", b: "┴", ml: "├", mr: "┤", x: "┼" };
      return { h: "-", v: "|", tl: "+", tr: "+", bl: "+", br: "+", t: "+", b: "+", ml: "+", mr: "+", x: "+" };
    }

    emitImage() {
      const alt = this.imageAlt;
      const src = this.imageSrc;
      const title = this.imageTitle;
      const savedDepth = this.imageDepth;
      this.imageDepth = 0;
      const hasSrc = src && src.length > 0;
      const linkOk = this.theme.colors && this.theme.hyperlinks && hasSrc && this.linkDepth === 0 && !src.startsWith("data:");
      if (linkOk) { this.wRawNoColor("\x1b]8;;"); this.wRawNoColor(src); this.wRawNoColor("\x1b\\"); }
      const marker = this.theme.colors ? "📷 " : "[img] ";
      this.writeStyled(ANSI.MAGENTA, marker);
      if (alt) this.writeContent(alt);
      else if (title) this.writeContent(title);
      else this.writeContent("(image)");
      this.writeStyled(ANSI.RESET, "");
      this.reapplyStyles();
      if (linkOk) this.wRawNoColor("\x1b]8;;\x1b\\");
      this.imageDepth = savedDepth;
    }
  }

  function stripAnsi(s) { return s.replace(/\x1b\[[0-9;]*[A-Za-z]/g, "").replace(/\x1b\]8;;[^\x1b\x07]*(\x1b\\|\x07)/g, ""); }
  function isJsLang(l) { return ["js", "javascript", "jsx", "mjs", "cjs", "ts", "typescript", "tsx", "mts", "cts"].includes(String(l).toLowerCase()); }

  // ---- cell ANSI state ----
  const CELL = { BOLD: 1, ITALIC: 2, UNDERLINE: 4, STRIKE: 8, DIM: 16 };
  function newCellState() { return { flags: 0, fg: null, bg: null, link: null }; }
  function cloneState(s) { return { flags: s.flags, fg: s.fg, bg: s.bg, link: s.link }; }
  function hasAny(s) { return s.flags !== 0 || s.fg || s.bg || s.link; }
  function emitOpens(s) {
    let o = "";
    if (s.flags & CELL.BOLD) o += "\x1b[1m";
    if (s.flags & CELL.DIM) o += "\x1b[2m";
    if (s.flags & CELL.ITALIC) o += "\x1b[3m";
    if (s.flags & CELL.UNDERLINE) o += "\x1b[4m";
    if (s.flags & CELL.STRIKE) o += "\x1b[9m";
    if (s.fg) o += s.fg;
    if (s.bg) o += s.bg;
    if (s.link) o += s.link;
    return o;
  }
  function emitCloses(s) { let o = ""; if (hasAny(s)) o += "\x1b[0m"; if (s.link) o += "\x1b]8;;\x1b\\"; return o; }
  function scanState(st, bytes) {
    let i = 0;
    while (i < bytes.length) {
      if (bytes[i] !== "\x1b") { i++; continue; }
      if (i + 1 >= bytes.length) return;
      if (bytes[i + 1] === "[") {
        let j = i + 2;
        while (j < bytes.length) { const c = bytes.charCodeAt(j); if (c >= 0x40 && c <= 0x7e) break; j++; }
        if (j >= bytes.length) return;
        if (bytes[j] === "m") applySgr(st, bytes.slice(i, j + 1), bytes.slice(i + 2, j));
        i = j + 1; continue;
      }
      if (bytes[i + 1] === "]") {
        let j = i + 2;
        while (j < bytes.length) {
          if (bytes[j] === "\x07") { j++; break; }
          if (bytes[j] === "\x1b" && bytes[j + 1] === "\\") { j += 2; break; }
          j++;
        }
        const seq = bytes.slice(i, j);
        if (seq.length >= 5 && seq.startsWith("\x1b]8;")) {
          let body = seq.slice(4);
          if (body.endsWith("\x1b\\")) body = body.slice(0, -2);
          else if (body.endsWith("\x07")) body = body.slice(0, -1);
          const semi = body.indexOf(";");
          if (semi !== -1) { const url = body.slice(semi + 1); st.link = url ? seq : null; }
        }
        i = j; continue;
      }
      i++;
    }
  }
  function applySgr(st, seq, params) {
    if (params === "") { st.flags = 0; st.fg = null; st.bg = null; return; }
    const parts = params.split(";");
    for (let idx = 0; idx < parts.length; idx++) {
      const n = parseInt(parts[idx], 10);
      if (isNaN(n)) continue;
      if (n === 0) { st.flags = 0; st.fg = null; st.bg = null; }
      else if (n === 1) st.flags |= CELL.BOLD;
      else if (n === 2) st.flags |= CELL.DIM;
      else if (n === 3) st.flags |= CELL.ITALIC;
      else if (n === 4) st.flags |= CELL.UNDERLINE;
      else if (n === 9) st.flags |= CELL.STRIKE;
      else if (n === 22) st.flags &= ~(CELL.BOLD | CELL.DIM);
      else if (n === 23) st.flags &= ~CELL.ITALIC;
      else if (n === 24) st.flags &= ~CELL.UNDERLINE;
      else if (n === 29) st.flags &= ~CELL.STRIKE;
      else if ((n >= 30 && n <= 37) || (n >= 90 && n <= 97)) st.fg = seq;
      else if (n === 38) { st.fg = seq; return; }
      else if (n === 39) st.fg = null;
      else if ((n >= 40 && n <= 47) || (n >= 100 && n <= 107)) st.bg = seq;
      else if (n === 48) { st.bg = seq; return; }
      else if (n === 49) st.bg = null;
    }
  }
  function lastWordBreakOutsideEscapes(s) {
    let last = -1, i = 0;
    while (i < s.length) {
      const c = s[i];
      if (c === "\x1b" && i + 1 < s.length) {
        const nx = s[i + 1];
        if (nx === "[") { i += 2; while (i < s.length) { const cc = s.charCodeAt(i); if (cc >= 0x40 && cc <= 0x7e) { i++; break; } i++; } continue; }
        if (nx === "]") { i += 2; while (i < s.length) { if (s[i] === "\x07") { i++; break; } if (s[i] === "\x1b" && s[i + 1] === "\\") { i += 2; break; } i++; } continue; }
        i += 2; continue;
      }
      if (c === " ") last = i;
      i++;
    }
    return last;
  }

  // ---- JS syntax highlighter (port of QuickAndDirty) ----
  const KW = {};
  (function () {
    const magenta = "\x1b[35m", blue = "\x1b[34m", orange = "\x1b[33m", red = "\x1b[31m";
    const m = {
      abstract: blue, as: blue, async: magenta, await: magenta, case: magenta, catch: magenta, class: magenta,
      const: magenta, continue: magenta, debugger: magenta, default: magenta, delete: red, do: magenta, else: magenta,
      break: magenta, undefined: orange, enum: blue, export: magenta, extends: magenta, false: orange, finally: magenta,
      for: magenta, function: magenta, if: magenta, implements: blue, import: magenta, in: magenta, instanceof: magenta,
      interface: blue, let: magenta, new: magenta, null: orange, package: magenta, private: blue, protected: blue,
      public: blue, return: magenta, static: magenta, super: magenta, switch: magenta, this: orange, throw: magenta,
      true: orange, try: magenta, type: blue, typeof: magenta, var: magenta, void: magenta, while: magenta, with: magenta,
      yield: magenta, string: blue, number: blue, boolean: blue, symbol: blue, any: blue, object: blue, unknown: blue,
      never: blue, namespace: blue, declare: blue, readonly: blue,
    };
    for (const k in m) KW[k] = m[k];
  })();
  function isIdentStart(c) { return /[A-Za-z_$]/.test(c); }
  function isIdentCont(c) { return /[A-Za-z0-9_$]/.test(c); }
  // Redact string values assigned to a sensitive-looking key (token/auth/password/
  // secret/api key/email) before highlighting — used by the redacting highlighter
  // that prints bunfig/error source lines. Only matches terminated string literals,
  // so unterminated `${` tails are left untouched (no OOB).
  function redactSecrets(text) {
    return String(text).replace(
      /(\b\w*(?:token|auth|password|passwd|secret|apikey|api[_-]?key|email)\w*\s*[:=]\s*)(["'`])((?:\\.|[^\\])*?)\2/gi,
      (m, pre, q, val) => pre + q + "*".repeat(Math.max(1, Math.min(val.length, 8))) + q);
  }
  // Expose the highlighter to bun:internal-for-testing (M + these fns are closure vars).
  if (M["bun:internal-for-testing"]) {
    M["bun:internal-for-testing"].highlightJavaScript = (x) => highlightJs(String(x));
    M["bun:internal-for-testing"].highlightJavaScriptRedacted = (x) => highlightJs(redactSecrets(String(x)));
    // createSocketPair() → [readFd, writeFd]: a connected non-blocking AF_UNIX
    // stream pair (native), for the FileSink backpressure/leak tests.
    M["bun:internal-for-testing"].createSocketPair = () => {
      if (typeof G.__mbunCreateSocketPair !== "function") throw new Error("createSocketPair is not supported on this platform");
      return G.__mbunCreateSocketPair();
    };
    // fileSinkInternals.liveCount(): live FileSink count (JS-tracked on a global
    // by the Bun.file(...).writer() factory).
    M["bun:internal-for-testing"].fileSinkInternals = { liveCount: () => (G.__mbunFileSinkLive ? G.__mbunFileSinkLive.n : 0) };
  }
  function highlightJs(text) {
    if (text.length > 2048 || text.length === 0 || /[^\x00-\x7f]/.test(text)) return text;
    const RESET = "\x1b[0m";
    let out = "";
    let prevKw = null;
    let i = 0;
    const len = text.length;
    while (i < len) {
      const c = text[i];
      if (isIdentStart(c)) {
        let j = i + 1;
        while (j < len && isIdentCont(text[j])) j++;
        const word = text.slice(i, j);
        if (KW[word] !== undefined) {
          if (word !== "as") prevKw = word;
          out += RESET + KW[word] + word + RESET;
        } else {
          let handled = false;
          if (prevKw) {
            if (prevKw === "new") { prevKw = null; if (j < len && text[j] === "(") { out += RESET + "\x1b[1m" + word + RESET; handled = true; } }
            else if (["abstract", "namespace", "declare", "type", "interface"].includes(prevKw)) { out += RESET + "\x1b[1m\x1b[34m" + word + RESET; prevKw = null; handled = true; }
            else if (prevKw === "import" && word === "from") { out += RESET + "\x1b[35m" + word + RESET; prevKw = null; handled = true; }
          }
          if (!handled) out += word;
        }
        i = j; continue;
      }
      const cc = text[i];
      if (cc >= "0" && cc <= "9") {
        prevKw = null;
        let j = i + 1;
        if (i + 1 < len && cc === "0" && text[i + 1] === "x") { j = i + 2; while (j < len && /[0-9a-fA-F]/.test(text[j])) j++; }
        else while (j < len && /[0-9.eExXbBoO]/.test(text[j])) j++;
        out += RESET + "\x1b[33m" + text.slice(i, j) + RESET;
        i = j; continue;
      }
      if (cc === "`" || cc === '"' || cc === "'") {
        prevKw = null;
        let j = i + 1;
        let seg = "";
        let broke = false;
        while (j < len && text[j] !== cc) {
          if (cc === "`" && text[j] === "$" && text[j + 1] === "{") {
            const curlyStart = j;
            j += 2;
            while (j < len && text[j] !== "}") { if (text[j] === "\\") j++; j++; }
            out += RESET + "\x1b[32m" + text.slice(i, curlyStart) + RESET;
            out += "${";
            const inner = text.slice(curlyStart + 2, j);
            if (inner) out += highlightJsInner(inner);
            if (j < len && text[j] === "}") { out += "}"; j++; }
            // reset i to j start
            i = j;
            if (i < len && text[i] === cc) { out += RESET + "\x1b[32m" + "`" + RESET; i++; }
            broke = true;
            break;
          }
          if (text[j] === "\\" && j + 1 < len) j++;
          j++;
        }
        if (broke) continue;
        if (j < len) j++; // include closing quote
        out += RESET + "\x1b[32m" + text.slice(i, j) + RESET;
        i = j; continue;
      }
      if (cc === "/") {
        prevKw = null;
        if (text[i + 1] === "/") {
          let j = i;
          while (j < len && text[j] !== "\n") j++;
          out += RESET + "\x1b[2m" + text.slice(i, j) + RESET;
          i = j; continue;
        }
        if (text[i + 1] === "*") {
          let j = i + 2;
          while (j + 2 < len && text.slice(j, j + 2) !== "*/") j++;
          if (j + 2 < len && text.slice(j, j + 2) === "*/") { j += 2; out += RESET + "\x1b[2m" + text.slice(i, j) + RESET; i = j; continue; }
        }
        out += cc; i++; continue;
      }
      if (cc === "}" || cc === "{") {
        if (prevKw !== "import") prevKw = null;
        out += cc; i++; continue;
      }
      if (cc === "[" || cc === "]") { prevKw = null; out += cc; i++; continue; }
      if (cc === ";") { prevKw = null; out += RESET + "\x1b[2m;" + RESET; i++; continue; }
      if (cc === ".") {
        prevKw = null;
        if (i + 1 < len && (isIdentStart(text[i + 1]) || text[i + 1] === "#")) {
          let j = i + 2;
          while (j < len && isIdentCont(text[j])) j++;
          if (j < len && text[j] === "(") { out += RESET + "\x1b[3m\x1b[1m" + text.slice(i, j) + RESET; i = j; continue; }
        }
        out += cc; i++; continue;
      }
      if (cc === "<") { out += RESET + "<" + RESET; i++; continue; }
      out += cc; i++;
    }
    return out;
  }
  function highlightJsInner(text) {
    // like highlightJs but without the ascii/length guard (nested interp)
    const saved = highlightJs;
    return highlightJsNoGuard(text);
  }
  function highlightJsNoGuard(text) {
    // reuse by temporarily bypassing guard: replicate with guard removed
    return highlightJs2(text);
  }
  function highlightJs2(text) {
    // Same as highlightJs but skip the leading guard.
    const RESET = "\x1b[0m";
    let out = "", prevKw = null, i = 0; const len = text.length;
    while (i < len) {
      const c = text[i];
      if (isIdentStart(c)) {
        let j = i + 1; while (j < len && isIdentCont(text[j])) j++;
        const word = text.slice(i, j);
        if (KW[word] !== undefined) { if (word !== "as") prevKw = word; out += RESET + KW[word] + word + RESET; }
        else out += word;
        i = j; continue;
      }
      const cc = text[i];
      if (cc >= "0" && cc <= "9") { let j = i + 1; while (j < len && /[0-9.eExXbBoO]/.test(text[j])) j++; out += RESET + "\x1b[33m" + text.slice(i, j) + RESET; i = j; continue; }
      if (cc === "`" || cc === '"' || cc === "'") { let j = i + 1; while (j < len && text[j] !== cc) { if (text[j] === "\\") j++; j++; } if (j < len) j++; out += RESET + "\x1b[32m" + text.slice(i, j) + RESET; i = j; continue; }
      if (cc === ";") { out += RESET + "\x1b[2m;" + RESET; i++; continue; }
      out += cc; i++;
    }
    return out;
  }

)JS";

}  // namespace mbun::jsc::builtins::detail
