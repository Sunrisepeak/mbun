// Bun `$` shell template-tag builtin. Kept as a dedicated payload partition so
// ShellPromise semantics do not grow runtime.cppm or the bootstrap partition.
// Ref: bun src/js/builtins/shell.ts.
export module mbun.jsc.js_builtins:shell;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kShellJS = R"JS(
  // ---- Bun.$ shell tagged template (ShellPromise over the native shell compiler) ----
  if (G.Bun && G.__mbunShellNative && typeof G.Bun.$ === "undefined") {
    const SHN = G.__mbunShellNative;
    const snapshotEnv = (source) => {
      const snapshot = {};
      for (const key of Object.keys(source || {})) {
        const value = source[key];
        if (value !== undefined) snapshot[key] = String(value);
      }
      return snapshot;
    };

    class ShellOutput {
      constructor(stdout, stderr, exitCode) {
        this.stdout = stdout;
        this.stderr = stderr;
        this.exitCode = exitCode;
      }
      text(encoding) { return this.stdout.toString(encoding); }
      json() { return JSON.parse(this.stdout.toString()); }
      arrayBuffer() {
        const bytes = this.stdout;
        return bytes.buffer.slice(bytes.byteOffset || 0, (bytes.byteOffset || 0) + bytes.byteLength);
      }
      bytes() { return new Uint8Array(this.arrayBuffer()); }
      blob() { return new G.Blob([this.stdout]); }
    }

    class ShellError extends Error {
      constructor() { super(""); this.name = "ShellError"; }
      initialize(output, code) {
        this.message = "Failed with exit code " + code;
        this.exitCode = code;
        this.stdout = output.stdout;
        this.stderr = output.stderr;
        Object.defineProperty(this, "info", {
          value: { exitCode: code, stdout: output.stdout, stderr: output.stderr },
          writable: true, configurable: true, enumerable: false,
        });
        this._output = output;
        return this;
      }
      text(encoding) { return this._output.text(encoding); }
      json() { return this._output.json(); }
      arrayBuffer() { return this._output.arrayBuffer(); }
      bytes() { return this._output.bytes(); }
      blob() { return this._output.blob(); }
    }

    class ShellPromise extends Promise {
      constructor(script, throws, cwd, env) {
        let resolve, reject;
        super((res, rej) => { resolve = res; reject = rej; });
        this._script = script;
        this._throws = throws;
        this._cwd = cwd;
        this._env = snapshotEnv(env);
        this._quiet = false;
        this._hasRun = false;
        this._resolve = resolve;
        this._reject = reject;
        this._potentialError = new ShellError();
      }
      _throwIfRunning() {
        if (this._hasRun) throw new Error("Shell is already running");
      }
      cwd(newCwd) {
        this._throwIfRunning();
        if (newCwd === undefined || newCwd === "." || newCwd === "" || newCwd === "./") {
          newCwd = G.process.cwd();
        }
        if (typeof newCwd !== "string") throw new TypeError("cwd must be a string or undefined");
        this._cwd = newCwd;
        return this;
      }
      env(newEnv) {
        this._throwIfRunning();
        if (newEnv === undefined) newEnv = G.process.env;
        if (!newEnv || typeof newEnv !== "object") throw new TypeError("env must be an object or undefined");
        this._env = snapshotEnv(newEnv);
        return this;
      }
      quiet(isQuiet) { this._throwIfRunning(); this._quiet = isQuiet === undefined ? true : !!isQuiet; return this; }
      nothrow() { this._throws = false; return this; }
      throws(doThrow) { this._throws = !!doThrow; return this; }
      run() { this._run(); return this; }
      _run() {
        if (this._hasRun) return;
        this._hasRun = true;
        if (G.process.platform === "win32") {
          this._reject(new Error("Bun.$ shell execution is unsupported on Windows in this build"));
          return;
        }
        let process;
        try {
          process = G.Bun.spawn({
            cmd: ["/bin/sh", "-c", this._script],
            cwd: this._cwd,
            env: this._env,
            stdin: "ignore",
            stdout: "pipe",
            stderr: "pipe",
          });
        } catch (error) {
          this._reject(error);
          return;
        }
        Promise.all([process.stdout.bytes(), process.stderr.bytes(), process.exited]).then(
          ([stdoutBytes, stderrBytes, code]) => {
            const stdout = Buffer.from(stdoutBytes);
            const stderr = Buffer.from(stderrBytes);
            const output = new ShellOutput(stdout, stderr, code == null ? 1 : code);
            if (!this._quiet) {
              if (stdout.byteLength) G.process.stdout.write(stdout);
              if (stderr.byteLength) G.process.stderr.write(stderr);
            }
            if (this._throws && output.exitCode !== 0) {
              this._reject(this._potentialError.initialize(output, output.exitCode));
            } else {
              this._potentialError = undefined;
              this._resolve(output);
            }
          },
          (error) => this._reject(error),
        );
      }
      then(onfulfilled, onrejected) { this._run(); return super.then(onfulfilled, onrejected); }
      async text(encoding) { const output = await this.quiet(true); return output.text(encoding); }
      async json() { const output = await this.quiet(true); return output.json(); }
      async *lines() {
        const output = await this.quiet(true);
        const text = output.stdout.toString();
        yield* (G.process.platform === "win32" ? text.split(/\r?\n/) : text.split("\n"));
      }
      async arrayBuffer() { const output = await this.quiet(true); return output.arrayBuffer(); }
      async bytes() { const output = await this.quiet(true); return output.bytes(); }
      async blob() { const output = await this.quiet(true); return output.blob(); }
      static get [Symbol.species]() { return Promise; }
    }

    const originalDefaultEnv = G.process.env || {};
    const makeTag = (name) => {
      const state = { cwd: undefined, env: originalDefaultEnv, throws: true };
      const tag = function(first, ...values) {
        if (!first || first.raw === undefined) {
          throw new Error("Please use '$' as a tagged template function: $`cmd arg1 arg2`");
        }
        const script = SHN.compile(first.raw, values);
        return new ShellPromise(script, state.throws, state.cwd, state.env);
      };
      Object.defineProperty(tag, "name", { value: name, configurable: true });
      tag.env = (newEnv) => {
        if (newEnv === undefined || newEnv === originalDefaultEnv) state.env = originalDefaultEnv;
        else if (newEnv && typeof newEnv === "object") state.env = snapshotEnv(newEnv);
        else throw new TypeError("env must be an object or undefined");
        return tag;
      };
      tag.cwd = (newCwd) => {
        if (newCwd === "." || newCwd === "" || newCwd === "./") newCwd = G.process.cwd();
        if (newCwd !== undefined && typeof newCwd !== "string") throw new TypeError("cwd must be a string or undefined");
        state.cwd = newCwd;
        return tag;
      };
      tag.nothrow = () => { state.throws = false; return tag; };
      tag.throws = (doThrow) => { state.throws = !!doThrow; return tag; };
      return tag;
    };

    class Shell {
      constructor() { return makeTag("Shell"); }
    }

    const BunShell = makeTag("BunShell");
    Object.defineProperties(BunShell, {
      // bun exposes escape on `$` itself (BunObject.cpp: putDirectNativeFunction
      // on the shell object), not on Shell instances.
      escape: { value: (str) => SHN.escape(str), enumerable: true },
      Shell: { value: Shell, enumerable: true },
      ShellPromise: { value: ShellPromise, enumerable: true },
      ShellError: { value: ShellError, enumerable: true },
    });
    G.Bun.$ = BunShell;
  }
)JS";

}  // namespace mbun::jsc::builtins::detail
