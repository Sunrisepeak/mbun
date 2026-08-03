// Bun `$` execution-binding partition. Overrides ShellPromise._run so a compiled
// shell script runs through mbun's own interpreter (modules/shell real fork/exec via
// __mbunShellNative.exec) instead of the host /bin/sh, falling back to the original
// spawn-based _run for any construct the interpreter-backed lowering cannot model.
//
// NOTE: this partition is appended AFTER the master builtins IIFE (opened in
// bootstrap, closed by image_closure) has already run — and after the :shell
// partition has defined Bun.$ — so it wraps itself in a self-contained IIFE that
// re-binds G = globalThis. It must not rely on the outer IIFE's `G` alias.
//
// Ref: bun src/js/builtins/shell.ts (ShellPromise run/quiet/throws/ShellError).
export module mbun.jsc.js_builtins:bunsh;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kBunShJS = R"JS(
(function () {
  const G = globalThis;
  const SHN = G.__mbunShellNative;
  if (!G.Bun || !G.Bun.$ || !SHN || typeof SHN.exec !== "function") return;
  const SP = G.Bun.$.ShellPromise;
  if (!SP || !SP.prototype || SP.prototype.__mbunNativeExec) return;
  SP.prototype.__mbunNativeExec = true;

  // Output view over captured native bytes. The class NAME is part of the
  // Bun-compatible surface: bun reports output.constructor.name === "ShellOutput".
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

  const originalRun = SP.prototype._run;
  SP.prototype._run = function () {
    if (this._hasRun) return;
    if (G.process.platform === "win32") { originalRun.call(this); return; }

    let res;
    try {
      const cwd = this._cwd === undefined ? G.process.cwd() : this._cwd;
      res = SHN.exec(this._script, cwd, this._env);
    } catch (error) {
      this._hasRun = true;
      this._reject(error);
      return;
    }
    // null => the script uses a construct outside the interpreter-backed subset;
    // defer to the original spawn-based implementation (leaves _hasRun untouched).
    if (res == null) { originalRun.call(this); return; }

    this._hasRun = true;
    const stdout = Buffer.from(res.stdout || "", "base64");
    const stderr = Buffer.from(res.stderr || "", "base64");
    const exitCode = res.exitCode == null ? 1 : res.exitCode | 0;
    const output = new ShellOutput(stdout, stderr, exitCode);
    if (!this._quiet) {
      if (stdout.byteLength) G.process.stdout.write(stdout);
      if (stderr.byteLength) G.process.stderr.write(stderr);
    }
    if (this._throws && exitCode !== 0) {
      this._reject(this._potentialError.initialize(output, exitCode));
    } else {
      this._potentialError = undefined;
      this._resolve(output);
    }
  };
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
