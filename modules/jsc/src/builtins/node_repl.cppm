// node:repl payload partition — a real REPLServer.
//
// bootstrap.cppm registered a placeholder (`start: () => new EventEmitter()`),
// so every node repl test died on `replServer.write is not a function`. This is
// node lib/repl.js translated onto the pieces mbun already has: REPLServer
// extends node:readline's Interface (the 1:1 bun port in node_readline), the
// evaluator is node:vm, and the default context is a contextified sandbox
// carrying the host globals, `module`, `require` and a Console bound to the
// REPL's own output — the same construction node performs in createContext().
//
// Kept faithful: defaultEval (object-literal wrapping, REPL_MODE_STRICT, the
// recoverable/multiline protocol, RegExp.$1..$9 save/restore), _handleError's
// "Uncaught " decoration, `_`/`_error` assignment tracking, .break/.clear/
// .exit/.help/.save/.load/.editor, editor mode and its _ttyWrite overlay,
// defineCommand, resetContext + the "reset" event, and the completer.
//
// Substituted, because node's versions live in internals mbun does not have:
//   * isRecoverableError / isValidSyntax — node runs acorn with a recoverable
//     parser subclass; here JSC's own parse verdict is combined with a scan of
//     the lexical state at end-of-input (template / block comment / continued
//     string), which is the same three-case rule acorn is asked for.
//   * processTopLevelAwait — node rewrites the statement list through acorn;
//     here the command is wrapped in an async IIFE returning { value }, which
//     covers the expression and statement forms the REPL protocol needs.
//   * the multiline continuation is node's `| ` prompt driven from the buffered
//     command, not readline's kAddNewLineOnTTY (mbun's readline has no
//     multiline line editor).
// DEFERRED: input preview, reverse-i-search, and the persistent-history
// manager's async flushing.
//
// NOTE: appended AFTER the master builtins IIFE has closed (see image_closure),
// so this is a self-contained IIFE that re-binds G = globalThis and replaces
// the registry's earlier `repl` stub.
export module mbun.jsc.js_builtins:node_repl;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kNodeReplJS = R"JS(
(function () {
  const G = globalThis;
  const M = G.__mbunNativeModules;
  if (!M) return;
  const req = (id) => {
    const n = String(id).replace(/^node:/, "");
    return M["node:" + n] || M[n];
  };
  const process = G.process;
  const util = req("util");
  const inspect = util.inspect;
  const vm = req("vm");
  const path = req("path");
  const fs = req("fs");
  const { Interface } = req("readline");
  const { Console } = req("console");
  const moduleMod = req("module");
  const CJSModule = moduleMod.Module || moduleMod;

  // node internal/errors.js: an E() error carries kIsNodeError, so both
  // defaultPrepareStackTrace and NodeError#toString render the code into the
  // header — `TypeError [ERR_X]: msg`. That header is what assert.throws(/ERR_X/)
  // matches (it tests String(err)) and what this REPL's own writer() prints.
  // mbun's `.stack` is pure JSC frames with no header line at all, so the header
  // has to be PREPENDED; splicing would eat the first frame.
  const ERR = (code, Ctor, msg) => {
    const e = new Ctor(msg);
    e.code = code;
    const header = `${e.name} [${code}]: ${e.message}`;
    try {
      const st = e.stack;
      e.stack = typeof st === "string" && st.length ? `${header}\n${st}` : header;
    } catch { /* a throwing/readonly `stack` accessor is not fatal */ }
    try {
      Object.defineProperty(e, "toString", {
        value: () => header, writable: true, configurable: true, enumerable: false,
      });
    } catch { /* frozen error: the stack header still carries the code */ }
    return e;
  };
  const ERR_MISSING_ARGS = (name) =>
    ERR("ERR_MISSING_ARGS", TypeError, `The "${name}" argument must be specified`);
  const ERR_INVALID_ARG_VALUE = (name, value, reason) =>
    ERR("ERR_INVALID_ARG_VALUE", TypeError,
        `The property '${name}' ${reason} Received ${inspect(value)}`);
  const ERR_INVALID_REPL_EVAL_CONFIG = () =>
    ERR("ERR_INVALID_REPL_EVAL_CONFIG", TypeError,
        'Cannot specify both "breakEvalOnSigint" and "eval" for REPL');
  const ERR_INVALID_REPL_INPUT = (msg) => ERR("ERR_INVALID_REPL_INPUT", TypeError, msg);
  const ERR_INVALID_STATE = (msg) => ERR("ERR_INVALID_STATE", Error, `Invalid state: ${msg}`);

  const isNativeError = (util.types && util.types.isNativeError) || (() => false);
  const isError = (e) => e instanceof Error || isNativeError(e);

  // ── persistent history ────────────────────────────────────────────────────
  // node lib/internal/repl/history.js ReplHistory, ported onto mbun's readline.
  //
  // The entries themselves stay in the Interface's own `this.history` array
  // (newest first, exactly node's layout), so the line editor's up/down
  // navigation is untouched and this class owns only what node's manager owns:
  // the file handle, the 15ms write debounce that guards against pasted input,
  // the `flushHistory` event a closing REPL waits on, and the `_historyPrev`
  // override that explains why history is not being persisted.
  //
  // node's version drives fs.promises + FileHandle; mbun's file handles are
  // the sync fd API, so each await point is kept but the I/O under it is
  // synchronous. The observable protocol — pause/resume around init, isFlushing
  // true from `line` until the debounced write lands, flushHistory emitted only
  // when no timer remains — is node's.
  const kDebounceHistoryMS = 15;
  const kDefaultHistorySize = 30;

  const replHistoryDisabledMessage =
    "\nPersistent history support disabled. " +
    "Set the NODE_REPL_HISTORY environment\nvariable to " +
    "a valid, user-writable path to enable.\n";

  class ReplHistory {
    constructor(context, options) {
      options = options || {};
      if (options.history !== undefined && !Array.isArray(options.history)) {
        throw ERR("ERR_INVALID_ARG_TYPE", TypeError,
                  'The "history" argument must be an instance of Array. ' +
                  `Received ${inspect(options.history)}`);
      }
      if (options.size !== undefined) {
        if (typeof options.size !== "number" || Number.isNaN(options.size)) {
          throw ERR("ERR_INVALID_ARG_TYPE", TypeError,
                    'The "size" argument must be of type number. ' +
                    `Received ${inspect(options.size)}`);
        }
        if (options.size < 0) {
          throw ERR("ERR_OUT_OF_RANGE", RangeError,
                    'The value of "size" is out of range. It must be >= 0. ' +
                    `Received ${options.size}`);
        }
      }
      let filePath = options.filePath;
      if (typeof filePath === "string") filePath = filePath.trim();
      this._path = filePath;
      this._context = context;
      this._timer = null;
      this._writing = false;
      this._pending = false;
      this._fd = null;
      this._isFlushing = false;
      this._size = options.size !== undefined && options.size !== null
        ? options.size
        : (context.historySize !== undefined ? context.historySize : kDefaultHistorySize);
      if (options.history) context.history = options.history;
      this._removeDuplicates = !!options.removeHistoryDuplicates;
      this.historyPrev = undefined;
    }

    get size() { return this._size; }
    get isFlushing() { return this._isFlushing; }
    get history() { return this._context.history; }
    set history(value) { this._context.history = value; }
    get index() { return this._context.historyIndex; }
    set index(value) { this._context.historyIndex = value; }

    // node writes through the Interface so the message lands above the prompt
    // and the prompt is redrawn under it.
    _writeToOutput(message) {
      const ctx = this._context;
      if (typeof ctx._writeToOutput === "function") {
        ctx._writeToOutput(message);
        if (typeof ctx._refreshLine === "function") ctx._refreshLine();
      }
    }

    // Installed as `_historyPrev` whenever persistence could not be set up: the
    // first `up` explains it, then the real navigation is restored.
    _replHistoryMessage() {
      if (!this._context.history || this._context.history.length === 0) {
        this._writeToOutput(replHistoryDisabledMessage);
      }
      this._context._historyPrev = this.historyPrev;
      return this._context._historyPrev();
    }

    _disable(onReadyCallback) {
      this.historyPrev = this._context._historyPrev;
      this._context._historyPrev = () => this._replHistoryMessage();
      return onReadyCallback(null, this._context);
    }

    _resolveHistoryPath() {
      if (!this._path) {
        try {
          this._path = path.join(req("os").homedir(), ".node_repl_history");
          return this._path;
        } catch {
          return null;
        }
      }
      return this._path;
    }

    initialize(onReadyCallback) {
      // An empty string disables persistent history outright.
      if (this._path === "") return this._disable(onReadyCallback);

      if (!this._resolveHistoryPath()) {
        this._writeToOutput("\nError: Could not get the home directory.\n" +
                            "REPL session history will not be persisted.\n");
        return this._disable(onReadyCallback);
      }

      this._context.pause();
      Promise.resolve()
        .then(() => this._initializeHistory(onReadyCallback))
        .catch((err) => this._handleInitError(err, onReadyCallback));
    }

    async _initializeHistory(onReadyCallback) {
      try {
        // Touch the file first so it exists; history files are conventionally
        // owner-only.
        fs.closeSync(fs.openSync(this._path, "a+", 0o600));

        let data;
        try {
          data = fs.readFileSync(this._path, "utf8");
        } catch (err) {
          return this._handleInitError(err, onReadyCallback);
        }

        this._context.history = data
          ? data.split(/\r?\n+/).slice(0, this._size)
          : [];

        const fd = fs.openSync(this._path, "r+");
        this._fd = fd;
        fs.ftruncateSync(fd, 0);

        this._onLineBound = () => this._onLine();
        this._context.on("line", this._onLineBound);
        this._context.once("exit", () => this._onExit());

        this._context.once("flushHistory", () => {
          if (!this._context.closed) {
            this._context.resume();
            onReadyCallback(null, this._context);
          }
        });

        await this._flushHistory();
      } catch (err) {
        this._closeHandle();
        return this._handleInitError(err, onReadyCallback);
      }
    }

    _handleInitError(err, onReadyCallback) {
      // Cannot open the history file. Don't crash — just don't persist.
      this._writeToOutput("\nError: Could not open history file.\n" +
                          "REPL session history will not be persisted.\n");
      this._context.resume();
      return this._disable(onReadyCallback);
    }

    _onLine() {
      this._isFlushing = true;
      if (this._timer) clearTimeout(this._timer);
      this._timer = setTimeout(() => { this._flushHistory(); }, kDebounceHistoryMS);
    }

    async _flushHistory() {
      this._timer = null;
      if (this._writing) {
        this._pending = true;
        return;
      }
      this._writing = true;
      const historyData = (this._context.history || []).join("\n");
      try {
        if (this._fd !== null) {
          fs.writeSync(this._fd, historyData, 0, "utf8");
          fs.ftruncateSync(this._fd, Buffer.byteLength(historyData, "utf8"));
        }
        this._writing = false;
        if (this._pending) {
          this._pending = false;
          this._onLine();
        } else {
          this._isFlushing = !!this._timer;
          if (!this._isFlushing) this._context.emit("flushHistory");
        }
      } catch {
        this._writing = false;
      }
    }

    _onExit() {
      if (this._isFlushing) {
        this._context.once("flushHistory", () => this._onExit());
        return;
      }
      if (this._onLineBound) this._context.off("line", this._onLineBound);
      this._closeHandle();
    }

    _closeHandle() {
      if (this._fd !== null && this._fd !== undefined) {
        const fd = this._fd;
        this._fd = null;
        try { fs.closeSync(fd); } catch { /* ignore */ }
      }
    }

    closeHandle() {
      this._closeHandle();
      return Promise.resolve();
    }
  }

  // ── recoverability ────────────────────────────────────────────────────────
  // node uses acorn (internal/repl/utils.isRecoverableError): an error is
  // recoverable when the parse failed *at* end-of-input, or the input ends
  // inside a template / block comment / backslash-continued string. JSC has no
  // acorn, but it reports the same three situations distinguishably: the
  // scanner state at EOF is recovered here and combined with JSC's message.
  function scanEofState(code) {
    let i = 0;
    const n = code.length;
    let lastQuoteContinued = false;
    while (i < n) {
      const c = code[i];
      if (c === "/" && code[i + 1] === "/") {
        while (i < n && code[i] !== "\n") i++;
        continue;
      }
      if (c === "/" && code[i + 1] === "*") {
        i += 2;
        while (i < n && !(code[i] === "*" && code[i + 1] === "/")) i++;
        if (i >= n) return "comment";
        i += 2;
        continue;
      }
      if (c === '"' || c === "'") {
        const quote = c;
        i++;
        lastQuoteContinued = false;
        while (i < n) {
          if (code[i] === "\\") {
            if (/[\r\n\u2028\u2029]/.test(code[i + 1] || "")) lastQuoteContinued = true;
            i += 2;
            continue;
          }
          if (code[i] === quote) break;
          if (code[i] === "\n") break; // unterminated on this line
          i++;
        }
        if (i >= n) return lastQuoteContinued ? "string-continued" : "string";
        i++;
        continue;
      }
      if (c === "`") {
        i++;
        let depth = 0;
        while (i < n) {
          if (code[i] === "\\") { i += 2; continue; }
          if (depth === 0 && code[i] === "`") break;
          if (code[i] === "$" && code[i + 1] === "{") { depth++; i += 2; continue; }
          if (depth > 0 && code[i] === "}") { depth--; i++; continue; }
          i++;
        }
        if (i >= n) return "template";
        i++;
        continue;
      }
      i++;
    }
    return "code";
  }

  // True when the input ends with brackets still open and no mismatch on the
  // way, i.e. acorn would have failed exactly at end-of-input. This has to be
  // tracked separately because parseThrows uses `new Function(code)`, which
  // appends a synthetic `}`: JSC then blames that token
  // ("Unexpected token '}'. Expected ')' to end a compound expression") rather
  // than reporting end-of-script, so `("a"` and `[1,2` look unrecoverable.
  function unbalancedAtEof(code) {
    const stack = [];
    const pairs = { ")": "(", "]": "[", "}": "{" };
    let i = 0;
    const n = code.length;
    while (i < n) {
      const c = code[i];
      if (c === "/" && code[i + 1] === "/") {
        while (i < n && code[i] !== "\n") i++;
        continue;
      }
      if (c === "/" && code[i + 1] === "*") {
        i += 2;
        while (i < n && !(code[i] === "*" && code[i + 1] === "/")) i++;
        if (i >= n) return false; // an open comment is already handled above
        i += 2;
        continue;
      }
      if (c === '"' || c === "'") {
        const quote = c;
        i++;
        while (i < n) {
          if (code[i] === "\\") { i += 2; continue; }
          if (code[i] === quote || code[i] === "\n") break;
          i++;
        }
        if (i >= n) return false;
        i++;
        continue;
      }
      if (c === "`") {
        // Template literals are reported by scanEofState; skip the whole thing.
        i++;
        let depth = 0;
        while (i < n) {
          if (code[i] === "\\") { i += 2; continue; }
          if (depth === 0 && code[i] === "`") break;
          if (code[i] === "$" && code[i + 1] === "{") { depth++; i += 2; continue; }
          if (depth > 0 && code[i] === "}") { depth--; i++; continue; }
          i++;
        }
        if (i >= n) return false;
        i++;
        continue;
      }
      if (c === "(" || c === "[" || c === "{") stack.push(c);
      else if (c === ")" || c === "]" || c === "}") {
        // A mismatch or a stray closer is a real error, never recoverable.
        if (stack.pop() !== pairs[c]) return false;
      }
      i++;
    }
    return stack.length > 0;
  }

  function parseThrows(code) {
    try {
      new Function(code);
      return null;
    } catch (e) {
      return e;
    }
  }

  function isRecoverableError(e, code) {
    // Wrap a leading `{` in parentheses first, exactly as node does, so an
    // incomplete object literal counts as recoverable.
    if (/^\s*\{/.test(code) && isRecoverableError(e, `(${code}`)) return true;

    const err = parseThrows(code);
    if (err === null) return false;
    if (err.name !== "SyntaxError") return false;

    const state = scanEofState(code);
    if (state === "template" || state === "comment" || state === "string-continued") return true;
    if (state === "string") return false;

    const msg = String(err.message || "");
    if (msg.includes("Unexpected end of script") || msg.includes("Unexpected EOF")) {
      return true;
    }
    // Brackets still open at end-of-input: acorn would have failed exactly
    // there, so node reports this as recoverable and prompts with `| `.
    return unbalancedAtEof(code);
  }

  function isValidSyntax(input) {
    if (parseThrows(input) === null) return true;
    return parseThrows(`_=${input}`) === null;
  }

  const startsWithBraceRegExp = /^\s*{/;
  const endsWithSemicolonRegExp = /;\s*$/;
  function isObjectLiteral(code) {
    return startsWithBraceRegExp.test(code) && !endsWithSemicolonRegExp.test(code);
  }

  let nextREPLResourceNumber = 1;
  function getREPLResourceName() {
    return `REPL${nextREPLResourceNumber++}`;
  }

  // Lazy: node's internal/repl/utils computes this at module load, but this
  // partition is part of the builtins image, so doing it eagerly would spin up
  // a whole JSC global context on EVERY process start just in case a REPL is
  // ever created.
  let globalBuiltinsCache;
  const globalBuiltinNames = () => {
    if (globalBuiltinsCache === undefined) {
      globalBuiltinsCache = new Set(
        vm.runInNewContext("Object.getOwnPropertyNames(globalThis)"));
    }
    return globalBuiltinsCache;
  };

  const builtinLibsFilter = (e) => e[0] !== "_" && !e.startsWith("node:");
  let _builtinLibs;
  const getReplBuiltinLibs = () => {
    if (_builtinLibs === undefined) {
      _builtinLibs = (CJSModule.builtinModules || []).filter(builtinLibsFilter);
    }
    return _builtinLibs;
  };
  const setReplBuiltinLibs = (v) => { _builtinLibs = v; };

  // ── writer ────────────────────────────────────────────────────────────────
  const writer = (obj) => inspect(obj, writer.options);
  writer.options = {
    showHidden: false, depth: 2, colors: false, customInspect: true,
    showProxy: true, maxArrayLength: 100, maxStringLength: 10000,
    // node: writer.options = { ...inspect.defaultOptions, showProxy: true },
    // and inspectDefaultOptions.breakLength is 80 (internal/util/inspect.js).
    // _handleError picks `Uncaught ` vs `Uncaught:\n` by comparing the error
    // line against this number, so 128 collapsed two-line output into one.
    breakLength: 80, compact: 3, sorted: false, getters: false,
    numericSeparator: false,
  };

  class Recoverable extends SyntaxError {
    constructor(err) {
      super();
      this.err = err;
    }
  }

  const kBufferedCommandSymbol = Symbol("bufferedCommand");
  const kLoadingSymbol = Symbol("loading");
  const kContextId = Symbol("contextId");
  const kMultilinePrompt = "| ";

  const REPL_MODE_SLOPPY = Symbol("repl-sloppy");
  const REPL_MODE_STRICT = Symbol("repl-strict");

  let processNewListenerUseCount = 0;
  let activeReplServer = null;
  // The most recent REPLServer to run a command — the owner of any async throw
  // its commands scheduled, once every live server has closed.
  let lastEvaluatingServer = null;
  // Live (not yet closed) REPL servers, newest last. node routes an async
  // uncaught exception to the REPL that owns the current async context via
  // AsyncLocalStorage + addUncaughtExceptionCaptureCallback; the innermost live
  // server is the same answer for every shape the tests drive.
  const liveServers = [];
  let installingCapture = false;
  let captureInstalled = false;
  let captureHandler = null;
  function setupExceptionCapture() {
    if (captureInstalled) return;
    captureInstalled = true;
    installingCapture = true;
    try {
      captureHandler = (err) => {
        // A REPL that has evaluated something owns the async throws its
        // commands scheduled, even after it closed: node reaches the same
        // answer through the AsyncLocalStorage store captured when the command
        // ran, which a closed REPL still carries. Ending stdin closes the REPL
        // before a setImmediate scheduled by the last command fires, and
        // rethrowing there killed the process instead of reporting through the
        // REPL's output (test-repl-uncaught-exception-after-input-ended).
        const server = liveServers[liveServers.length - 1] || lastEvaluatingServer;
        if (server === undefined || server === null) throw err;
        server._handleError(err);
      };
      process.on("uncaughtException", captureHandler);
    } finally {
      installingCapture = false;
    }
  }
  // node registers this capture through process.addUncaughtExceptionCaptureCallback
  // (repl.js:195), which is NOT an 'uncaughtException' listener. mbun's runtime
  // only offers the event, so the REPL's own handler has to be discounted
  // wherever node counts listeners: a standalone REPL otherwise reads its own
  // capture as a user handler, re-emits every eval error into itself instead of
  // printing it, and `mbun -i` reported nothing at all for a throw.
  function userUncaughtExceptionListeners() {
    let n = 0;
    // Indexed loop, not for-of: this runs on every REPL eval error, including the
    // one raised by user code that just deleted Array.prototype[Symbol.iterator]
    // (test-repl-unsafe-array-iteration), and a for-of would re-read it here and
    // replace the user's TypeError with a crash in the error reporter itself.
    const ls = process.listeners("uncaughtException");
    for (let i = 0; i < ls.length; i++) {
      if (ls[i] !== captureHandler) n++;
    }
    return n;
  }
  function processNewListener(event) {
    if (event === "uncaughtException" && activeReplServer !== null && !installingCapture) {
      throw ERR_INVALID_REPL_INPUT(
        "Listeners for `uncaughtException` cannot be used in the REPL");
    }
  }
  function addProcessNewListener() {
    if (processNewListenerUseCount++ === 0) {
      process.prependListener("newListener", processNewListener);
    }
  }
  function removeProcessNewListener() {
    if (--processNewListenerUseCount === 0) {
      process.removeListener("newListener", processNewListener);
    }
  }

  // ── completion (port of the classic node repl completer) ──────────────────
  const simpleExpressionRE =
    /(?:[\w$]+|[\w$]+\.(?:[\w$]+\.)*[\w$]*|\[[^\]]*\](?:\.[\w$]*)*)$/;
  const requireRE = /\brequire\s*\(\s*['"`](([\w@./:-]+\/)?(?:[\w@./:-]*))(?![^'"`])$/;
  const fsAutoCompleteRE = /fs(?:\.promises)?\.\s*[a-z][a-zA-Z]+\(\s*["'](.*)/;
  const importRE = /\bimport\s*\(\s*['"`](([\w@./:-]+\/)?(?:[\w@./:-]*))(?![^'"`])$/;

  function isIdentifier(str) {
    if (str === "") return false;
    const first = str.codePointAt(0);
    if (!((first >= 65 && first <= 90) || (first >= 97 && first <= 122) ||
          first === 36 || first === 95)) return false;
    for (let i = 1; i < str.length; i++) {
      const c = str.codePointAt(i);
      if (!((c >= 48 && c <= 57) || (c >= 65 && c <= 90) ||
            (c >= 97 && c <= 122) || c === 36 || c === 95)) return false;
    }
    return true;
  }

  function isNotLegacyObjectPrototypeMethod(str) {
    return isIdentifier(str) &&
      str !== "__defineGetter__" &&
      str !== "__defineSetter__" &&
      str !== "__lookupGetter__" &&
      str !== "__lookupSetter__";
  }

  function filteredOwnPropertyNames(obj) {
    if (!obj) return [];
    // `Object.prototype` is the only non-contrived object that fulfills
    // `Object.getPrototypeOf(X) === null &&
    //  Object.getPrototypeOf(Object.getPrototypeOf(X.constructor)) === X`.
    // Only on that object does node hide the legacy __define*__/__lookup*__
    // accessors from completion.
    let isObjectPrototype = false;
    try {
      if (Object.getPrototypeOf(obj) === null) {
        const ctorDescriptor = Object.getOwnPropertyDescriptor(obj, "constructor");
        if (ctorDescriptor && ctorDescriptor.value) {
          const ctorProto = Object.getPrototypeOf(ctorDescriptor.value);
          isObjectPrototype = !!ctorProto && Object.getPrototypeOf(ctorProto) === obj;
        }
      }
    } catch { /* fall through as a plain object */ }
    let names;
    try {
      names = Object.getOwnPropertyNames(obj);
    } catch { return []; }
    // node uses getOwnNonIndexProperties(obj, ALL_PROPERTIES | SKIP_SYMBOLS);
    // getOwnPropertyNames already skips symbols, and isIdentifier rejects the
    // array index keys, so the surviving set is the same.
    return names.filter(
      isObjectPrototype ? isNotLegacyObjectPrototypeMethod : isIdentifier);
  }

  function getGlobalLexicalScopeNames() { return []; }

  // node's addCommonWords: only words which do not yet exist as a global
  // property. Pushed as its own group, and only when there is a filter.
  const COMMON_WORDS = [
    "async", "await", "break", "case", "catch", "const", "continue",
    "debugger", "default", "delete", "do", "else", "export", "false",
    "finally", "for", "function", "if", "import", "in", "instanceof", "let",
    "new", "null", "return", "switch", "this", "throw", "true", "try",
    "typeof", "var", "void", "while", "with", "yield",
  ];
  function addCommonWords(completionGroups) {
    completionGroups.push(COMMON_WORDS.slice());
  }

  // node runs acorn over the line and walks the AST to find the trailing
  // sub-expression that tab completion should evaluate
  // (internal/repl/completion.js findExpressionCompleteTarget). The acorn copy
  // node uses lives in deps/, which the `internal/*` -> lib/internal/* module
  // mapping can never reach, so this is a reverse scanner over the same
  // grammar: from the end of the line, walk left over an identifier fragment
  // and then over as many `.`/`?.`-joined members as the text supports,
  // matching (), [] and quotes backwards so computed keys such as
  // `obj[lookupObj["a" + " b"]].toFi` stay part of the target.
  const DECLARATION_KEYWORD_RE = /(?:^|[^\w$])(?:let|const|var)\s+$/;
  function findExpressionCompleteTarget(code) {
    if (!code) return null;

    // A trailing `.` or `?.` cannot terminate an expression, so strip it, find
    // the target of the rest, and put it back.
    if (code.endsWith(".")) {
      if (code.length >= 2 && code[code.length - 2] === "?") {
        const inner = findExpressionCompleteTarget(code.slice(0, -2));
        return !inner ? inner : `${inner}?.`;
      }
      const inner = findExpressionCompleteTarget(code.slice(0, -1));
      return !inner ? inner : `${inner}.`;
    }

    // Walk left over the trailing identifier fragment being completed.
    let i = code.length;
    while (i > 0 && /[\w$]/.test(code[i - 1])) i--;
    const identStart = i;

    // `let a` / `const foo` / `var x`: a declaration with no initialiser has
    // nothing to complete on, so node's AST walk returns null here.
    if (DECLARATION_KEYWORD_RE.test(code.slice(0, identStart))) return null;

    // Walk left over member accesses. A `.`/`?.` joiner is optional, because
    // bracket accesses chain directly (`obj["a"]["b"]`).
    let start = identStart;
    for (;;) {
      let j = start;
      let afterDot = false;
      if (j > 0 && code[j - 1] === ".") {
        j--;
        if (j > 0 && code[j - 1] === "?") j--;
        afterDot = true;
      } else if (!(j > 0 && (code[j - 1] === "]" || code[j - 1] === ")"))) {
        break;
      }
      const k = consumeAtomBackwards(code, j);
      if (k < 0 || k >= j) {
        // A `.` whose left-hand side is not a completable base means the line
        // is not a member expression at all — `{}.a` is a block followed by
        // junk, which acorn rejects and node completes nothing for.
        if (afterDot) return null;
        break;
      }
      start = k;
    }

    const target = code.slice(start);
    if (target === "" || target === "." || target === "?.") return null;

    // node only evaluates a base that bottoms out at an identifier with
    // literal property keys (see includesProxiesOrGetters). Anything that
    // could run user code — a call, an assignment, an increment — makes the
    // whole target ineligible, which is what keeps tab completion free of
    // side effects for `incCounter().`, `a=(counter+=1).foo.` and
    // `arr[incCounter()].b`. Grouping parens around a literal such as
    // `("").a` carry no call and stay eligible.
    const base = code.slice(start, identStart);
    if (/[\w$\])]\s*\(/.test(base)) return null;
    if (/=|\+\+|--|;/.test(base)) return null;

    return target;
  }

  const isProxyValue = (util.types && util.types.isProxy) || (() => false);

  // node's includesProxiesOrGetters, over the target string instead of an AST:
  // split the base into its root identifier plus one step per property access,
  // then walk it checking each step for an own getter or a Proxy. `true` means
  // "do not evaluate this, completing it could run user code".
  function includesProxiesOrGetters(expr, evalInRepl) {
    const steps = splitMemberPath(expr);
    if (!steps) return false;
    let obj;
    try { obj = evalInRepl(steps.root); } catch { return false; }
    // The root itself may already be a Proxy (`proxyObj.<TAB>`), in which case
    // enumerating it would run the handler's traps.
    if (isProxyValue(obj)) return true;
    for (const step of steps.props) {
      if (obj === null || obj === undefined) return false;
      let key = step.name;
      if (step.computed) {
        // A computed key is itself an expression; only evaluate it when it is
        // literal enough to be side-effect free (findExpressionCompleteTarget
        // has already rejected calls and assignments).
        try { key = evalInRepl(step.name); } catch { return false; }
        if (typeof key !== "string" && typeof key !== "number") return false;
      }
      // Check for a getter BEFORE reading the value, so that a property which
      // does have one is never triggered by this very check.
      let desc;
      try { desc = Object.getOwnPropertyDescriptor(obj, key); } catch { return false; }
      if (desc && typeof desc.get === "function") return true;
      let value;
      try { value = obj[key]; } catch { return false; }
      if (isProxyValue(value)) return true;
      obj = value;
    }
    return false;
  }

  // Split a member expression such as `a.b["c"]` into { root: "a", props: [...] }.
  // Returns null when the expression is not a plain identifier-rooted chain.
  function splitMemberPath(expr) {
    let i = 0;
    while (i < expr.length && /[\w$]/.test(expr[i])) i++;
    if (i === 0) return null;
    const root = expr.slice(0, i);
    const props = [];
    while (i < expr.length) {
      if (expr[i] === "?" && expr[i + 1] === ".") i += 2;
      else if (expr[i] === ".") i += 1;
      else if (expr[i] === "[") {
        const close = matchForwards(expr, i, "[", "]");
        if (close < 0) return null;
        props.push({ name: expr.slice(i + 1, close), computed: true });
        i = close + 1;
        continue;
      } else return null;
      const start = i;
      while (i < expr.length && /[\w$]/.test(expr[i])) i++;
      if (i === start) return null;
      props.push({ name: expr.slice(start, i), computed: false });
    }
    return { root, props };
  }

  // Given code[start] === open, return the index of the matching close, or -1.
  function matchForwards(code, start, open, close) {
    let depth = 0;
    for (let i = start; i < code.length; i++) {
      const c = code[i];
      if (c === "\"" || c === "'" || c === "`") {
        let j = i + 1;
        for (; j < code.length; j++) {
          if (code[j] === "\\") { j++; continue; }
          if (code[j] === c) break;
        }
        if (j >= code.length) return -1;
        i = j;
        continue;
      }
      if (c === open) depth++;
      else if (c === close) {
        depth--;
        if (depth === 0) return i;
      }
    }
    return -1;
  }

  // Consume one member-access "atom" ending just before index `end` and return
  // the index it starts at, or -1 if there is no atom there. A bracket or paren
  // group also swallows the identifier naming it, so `obj["k"]` is one atom.
  function consumeAtomBackwards(code, end) {
    let k = end;
    const prev = k > 0 ? code[k - 1] : "";
    if (prev === ")" || prev === "]") {
      const open = prev === ")" ? "(" : "[";
      k = matchBackwards(code, k - 1, open, prev);
      if (k < 0) return -1;
      while (k > 0 && /[\w$]/.test(code[k - 1])) k--;
      return k;
    }
    if (prev === "\"" || prev === "'" || prev === "`") {
      return matchQuoteBackwards(code, k - 1, prev);
    }
    if (/[\w$]/.test(prev)) {
      while (k > 0 && /[\w$]/.test(code[k - 1])) k--;
      return k;
    }
    return -1;
  }

  // Given code[end] === close, return the index of the matching open bracket,
  // or -1. Skips over nested brackets and quoted strings.
  function matchBackwards(code, end, open, close) {
    let depth = 0;
    for (let i = end; i >= 0; i--) {
      const c = code[i];
      if (c === "\"" || c === "'" || c === "`") {
        i = matchQuoteBackwards(code, i, c);
        if (i < 0) return -1;
        continue;
      }
      if (c === close) depth++;
      else if (c === open) {
        depth--;
        if (depth === 0) return i;
      }
    }
    return -1;
  }

  // Given code[end] === quote, return the index of the opening quote, or -1.
  function matchQuoteBackwards(code, end, quote) {
    for (let i = end - 1; i >= 0; i--) {
      if (code[i] !== quote) continue;
      // Count preceding backslashes to tell an escaped quote from a real one.
      let bs = 0;
      let j = i - 1;
      while (j >= 0 && code[j] === "\\") { bs++; j--; }
      if (bs % 2 === 0) return i;
    }
    return -1;
  }

  function commonPrefix(strings) {
    if (!strings || strings.length === 0) return "";
    if (strings.length === 1) return strings[0];
    const sorted = strings.slice().sort();
    const first = sorted[0];
    const last = sorted[sorted.length - 1];
    for (let i = 0; i < first.length; i++) {
      if (first[i] !== last[i]) return first.slice(0, i);
    }
    return first;
  }

  function complete(line, callback) {
    // List of completion lists, one for each inheritance "level"
    let completionGroups = [];
    let completeOn, group;

    // node internal/repl/completion.js drops the leading indentation before it
    // looks at anything, so a line that is only whitespace is treated as an
    // empty line and still yields the full global completion (with completeOn
    // "" rather than undefined).
    line = line.trimStart();

    // REPL commands (e.g. ".break").
    let filter = "";
    let match = /^\s*\.(\w*)$/.exec(line);
    if (match) {
      completionGroups.push(Object.keys(this.commands));
      completeOn = match[1];
      if (completeOn.length) filter = completeOn;
    } else if ((match = requireRE.exec(line)) !== null) {
      completeOn = match[1];
      filter = completeOn;
      const subdir = match[2] || "";
      completionGroups.push(getReplBuiltinLibs());
      completionGroups.push(getReplBuiltinLibs().map((lib) => `node:${lib}`));
      if (subdir === "") completionGroups.push([]);
      if (this.allowBlockingCompletions) {
        const extensions = Object.keys(CJSModule._extensions || { ".js": 1 });
        const indexes = extensions.map((extension) => `index${extension}`);
        indexes.push("package.json", "index");
        const replModule = this.context && this.context.module;
        const paths = ((replModule && replModule.paths) || []).concat(CJSModule.globalPaths || []);
        const group2 = [];
        for (let dir of paths) {
          dir = path.resolve(dir, subdir);
          let dirents;
          try { dirents = fs.readdirSync(dir, { withFileTypes: true }); } catch { continue; }
          for (const dirent of dirents) {
            if (extensions.includes(path.extname(dirent.name)) || indexes.includes(dirent.name)) {
              continue;
            }
            const extension = path.extname(dirent.name);
            const base = dirent.name.slice(0, -extension.length);
            if (!dirent.isDirectory()) {
              if (extensions.includes(extension) && (!subdir || base !== "index")) {
                group2.push(`${subdir}${base}`);
              }
              continue;
            }
            group2.push(`${subdir}${dirent.name}/`);
            let files;
            try { files = fs.readdirSync(path.join(dir, dirent.name)); } catch { continue; }
            for (const file of files) {
              if (indexes.includes(file)) {
                group2.push(`${subdir}${dirent.name}`);
                break;
              }
            }
          }
        }
        if (group2.length) completionGroups.push(group2);
      }
    } else if ((match = importRE.exec(line)) !== null) {
      completeOn = match[1];
      filter = completeOn;
      completionGroups.push(getReplBuiltinLibs().map((lib) => `node:${lib}`));
      completionGroups.push(getReplBuiltinLibs());
    } else if (line.length === 0 || /\w|\.|\$/.test(line[line.length - 1])) {
      const completeTarget =
        line.length === 0 ? line : findExpressionCompleteTarget(line);
      if (line.length !== 0 && !completeTarget) {
        // No completable target (e.g. `let a`, or `{ a: true }`): node returns
        // no completions at all, and leaves completeOn undefined.
        return completionGroupsLoaded();
      }
      {
        let expr = "";
        completeOn = completeTarget;
        if (line.length !== 0) {
          const lastIndex = completeOn.lastIndexOf(".");
          if (lastIndex > -1) {
            expr = completeOn.slice(0, lastIndex);
            filter = completeOn.slice(lastIndex + 1);
          } else {
            filter = completeOn;
          }
        }
        // Optional chaining: the split above leaves the `?` on the expression
        // (`console?.lo` -> expr `console?`), so peel it off and remember that
        // the member joiner is `?.` rather than `.`.
        let chaining = ".";
        if (expr.endsWith("?")) {
          expr = expr.slice(0, -1);
          chaining = "?.";
        }
        if (!expr) {
          // One group per inheritance level, exactly as node does: the
          // prototype chain first (walked away from the context), then the
          // context's own names, then the keywords. completionGroupsLoaded
          // unshifts, so this array order comes out reversed — keywords
          // nearest the cursor, the far end of the prototype chain last.
          completionGroups.push(getGlobalLexicalScopeNames());
          let contextProto = this.context;
          while ((contextProto = Object.getPrototypeOf(contextProto)) !== null) {
            completionGroups.push(filteredOwnPropertyNames(contextProto));
          }
          const contextOwnNames = filteredOwnPropertyNames(this.context);
          if (!this.useGlobal) {
            // When the context is not `global`, builtins are not own
            // properties of it, so they have to be added back by name.
            for (const name of globalBuiltinNames()) contextOwnNames.push(name);
          }
          completionGroups.push(contextOwnNames);
          if (filter !== "") addCommonWords(completionGroups);
        } else {
          const evalInRepl = (src) => this.useGlobal
            ? (0, eval)(src)
            : vm.runInContext(src, this.context, { displayErrors: false });
          // node walks the member chain first and bails out entirely if any
          // step reads through a getter or a Proxy, so that merely pressing
          // TAB cannot trigger user code (internal/repl/completion.js
          // includesProxiesOrGetters).
          if (includesProxiesOrGetters(expr, evalInRepl)) {
            return completionGroupsLoaded();
          }
          let obj;
          try {
            obj = evalInRepl(expr);
          } catch { obj = undefined; }
          // node builds one group per inheritance level (memberGroups) so that
          // own properties shadow the ones further up the chain instead of
          // being merged into a single sorted list.
          const memberGroups = [];
          if (obj != null) {
            try {
              let p;
              if (typeof obj === "object" || typeof obj === "function") {
                memberGroups.push(filteredOwnPropertyNames(obj));
                p = Object.getPrototypeOf(obj);
              } else {
                p = obj.constructor ? obj.constructor.prototype : null;
              }
              // Circular refs possible? Let's guard against that.
              let sentinel = 5;
              while (p !== null && p !== undefined && sentinel-- !== 0) {
                memberGroups.push(filteredOwnPropertyNames(p));
                p = Object.getPrototypeOf(p);
              }
            } catch {
              // Maybe a Proxy object without `getOwnPropertyNames` trap.
              // We simply ignore it here, as we don't want to break the
              // autocompletion.
            }
          }
          if (memberGroups.length) {
            expr += chaining;
            for (const g of memberGroups) {
              completionGroups.push(g.map((member) => `${expr}${member}`));
            }
            if (filter !== "") filter = `${expr}${filter}`;
          }
        }
      }
    }

    return completionGroupsLoaded();

    function completionGroupsLoaded() {
      // Filter, sort (within each group), uniq and merge the completion groups.
      if (completionGroups.length && filter !== "") {
        const newCompletionGroups = [];
        // node: "Filter is always case-insensitive following chromium
        // autocomplete behavior." So `foo.b` offers `foo.BARbuz` too.
        const lowerCaseFilter = filter.toLocaleLowerCase();
        for (const group3 of completionGroups) {
          const filtered = group3.filter(
            (elem) => elem.toLocaleLowerCase().startsWith(lowerCaseFilter));
          if (filtered.length) newCompletionGroups.push(filtered);
        }
        completionGroups = newCompletionGroups;
      }
      const completions = [];
      // Unique completions across all groups. node seeds the set with "" so an
      // empty entry inside a group can never be mistaken for a separator.
      const uniqueSet = new Set();
      uniqueSet.add("");
      // Completion group 0 is the "closest" (least far up the inheritance
      // chain) so its completions go LAST, to sit nearest the cursor in the
      // REPL. That is why entries and separators are unshifted, not pushed.
      for (const group4 of completionGroups) {
        group4.sort((a, b) => (b > a ? 1 : -1));
        const setSize = uniqueSet.size;
        for (const entry of group4) {
          if (!uniqueSet.has(entry)) {
            completions.unshift(entry);
            uniqueSet.add(entry);
          }
        }
        // Add a separator between groups.
        if (uniqueSet.size !== setSize) completions.unshift("");
      }
      // Remove obsolete group entry, if present.
      if (completions[0] === "") completions.shift();
      callback(null, [completions, completeOn]);
    }
  }

  const KEYWORDS = [
    "await", "break", "case", "catch", "class", "const", "continue", "debugger",
    "default", "delete", "do", "else", "export", "extends", "false", "finally",
    "for", "function", "if", "import", "in", "instanceof", "let", "new", "null",
    "return", "super", "switch", "this", "throw", "true", "try", "typeof",
    "var", "void", "while", "with", "yield",
  ];

  // Top-level-await support: node uses acorn to rewrite the statement list. We
  // wrap the whole command in an async IIFE that yields { value } and let JSC's
  // own parser reject anything that is not valid inside an async function.
  function processTopLevelAwait(src) {
    const wrapPrefix = "(async () => { return { value: (";
    const wrapped = `${wrapPrefix}${src}\n) };})()`;
    if (parseThrows(wrapped) === null) return wrapped;
    const stmt = `(async () => { ${src}\n})()`;
    if (parseThrows(stmt) === null) return stmt;
    return null;
  }

  class REPLServer extends Interface {
    constructor(prompt, stream, eval_, useGlobal, ignoreUndefined, replMode) {
      let options;
      if (prompt !== null && typeof prompt === "object") {
        options = { ...prompt };
        stream = options.stream || options.socket;
        eval_ = options.eval;
        useGlobal = options.useGlobal;
        ignoreUndefined = options.ignoreUndefined;
        prompt = options.prompt;
        replMode = options.replMode;
      } else {
        options = {};
      }

      if (!options.input && !options.output) {
        stream = stream || process;
        options.input = stream.stdin || stream;
        options.output = stream.stdout || stream;
      }

      if (options.terminal === undefined) options.terminal = options.output.isTTY;
      options.terminal = !!options.terminal;

      if (options.terminal && options.useColors === undefined) {
        const out = options.output;
        options.useColors = !!(out && out.isTTY && (!out.getColorDepth || out.getColorDepth() > 2));
      }

      super({
        input: options.input,
        output: options.output,
        completer: options.completer || completerWrapper,
        terminal: options.terminal,
        historySize: options.historySize,
        prompt,
      });

      const self = this;

      Object.defineProperty(this, "inputStream", {
        get: () => this.input, set: (v) => { this.input = v; },
        enumerable: false, configurable: true,
      });
      Object.defineProperty(this, "outputStream", {
        get: () => this.output, set: (v) => { this.output = v; },
        enumerable: false, configurable: true,
      });

      this.allowBlockingCompletions = !!options.allowBlockingCompletions;
      this.useColors = !!options.useColors;
      this._isStandalone = !!options[kStandaloneREPL];

      if (options.domain !== undefined) {
        throw ERR_INVALID_ARG_VALUE("options.domain", options.domain,
                                    "is no longer supported.");
      }

      this.useGlobal = !!useGlobal;
      this.ignoreUndefined = !!ignoreUndefined;
      this.replMode = replMode || REPL_MODE_SLOPPY;
      this.underscoreAssigned = false;
      this.last = undefined;
      this.underscoreErrAssigned = false;
      this.lastError = undefined;
      this.breakEvalOnSigint = !!options.breakEvalOnSigint;
      this.editorMode = false;
      this._userErrorHandler = options.handleError;
      this[kContextId] = undefined;

      if (this.breakEvalOnSigint && eval_) throw ERR_INVALID_REPL_EVAL_CONFIG();

      if (options[kStandaloneREPL]) {
        replExports.repl = this;
      } else {
        addProcessNewListener();
        this.once("exit", removeProcessNewListener);
      }
      liveServers.push(this);
      this.once("exit", () => {
        const i = liveServers.indexOf(this);
        if (i !== -1) liveServers.splice(i, 1);
      });
      setupExceptionCapture();

      const savedRegExMatches = ["", "", "", "", "", "", "", "", "", ""];
      const regExMatchSeparator = "\u0000\u0000\u0000";
      const regExMatcher = new RegExp(
        `^${regExMatchSeparator}(.*)${regExMatchSeparator}(.*)` +
        `${regExMatchSeparator}(.*)${regExMatchSeparator}(.*)` +
        `${regExMatchSeparator}(.*)${regExMatchSeparator}(.*)` +
        `${regExMatchSeparator}(.*)${regExMatchSeparator}(.*)` +
        `${regExMatchSeparator}(.*)$`);

      function saveRegExpMatches() {
        try {
          for (let idx = 1; idx < savedRegExMatches.length; idx += 1) {
            savedRegExMatches[idx] = RegExp[`$${idx}`];
          }
        } catch (captureError) {
          // JSC currently exposes the legacy static captures through accessors
          // whose receiver check rejects its own RegExp constructor (issue #65).
          // That runtime defect must not replace an otherwise successful REPL
          // evaluation with an unrelated TypeError. Keep the normal node path
          // active for runtimes where the accessors are readable.
          if (!(captureError instanceof TypeError) ||
              captureError.message !==
                "RegExp.$N getters require RegExp constructor as |this|") {
            throw captureError;
          }
        }
      }

      eval_ = eval_ || defaultEval;

      const pausedBuffer = [];
      let paused = false;
      function pause() { paused = true; }
      function unpause() {
        if (!paused) return;
        paused = false;
        let entry;
        const tmpCompletionEnabled = self.isCompletionEnabled;
        while ((entry = pausedBuffer.shift()) !== undefined) {
          const [type, payload, isCompletionEnabled] = entry;
          if (type === "key") {
            const [d, key] = payload;
            self.isCompletionEnabled = isCompletionEnabled;
            self._ttyWrite(d, key);
          } else if (type === "close") {
            self.emit("exit");
          }
          if (paused) break;
        }
        self.isCompletionEnabled = tmpCompletionEnabled;
      }

      // node's repl.js never requires node:domain — it only reads
      // `process.domain` (repl.js:629). Requiring it eagerly here loaded the
      // whole module for every REPL, and node:domain installs a permanent
      // process 'newListener' listener of its own, so
      // test-repl-no-terminal-restore-process-listeners counted two listeners
      // added by one REPLServer and one removed on close. Consult the module
      // only once a domain is actually active, which cannot happen before
      // something else required it.
      // M["domain"] is a lazy accessor, so merely READING it builds the module —
      // hence the guard, which must also hold after the eval returned and
      // process.domain is null again. node:domain installs the runtime's
      // scheduling seam when it builds, so the seam answers "has anything loaded
      // node:domain?" without forcing that build.
      const domainErrorsHandled = () => {
        if (!G.__mbunSchedHook) return 0;
        const mod = M["domain"] || M["node:domain"];
        return (mod && typeof mod.__errorsHandled === "number") ? mod.__errorsHandled : 0;
      };

      function defaultEval(code, context, file, cb) {
        let result, script, wrappedErr;
        let err = null;
        let wrappedCmd = false;
        let awaitPromise = false;
        const input = code;

        if (isObjectLiteral(code) && isValidSyntax(code)) {
          code = `(${code.trim()})\n`;
          wrappedCmd = true;
        }

        if (code.includes("await")) {
          try {
            const potential = processTopLevelAwait(code);
            if (potential !== null) {
              code = potential;
              wrappedCmd = true;
              awaitPromise = true;
            }
          } catch (e) {
            err = e;
          }
        }

        if (code === "\n") return cb(null);

        if (err === null) {
          for (;;) {
            try {
              if (self.replMode === REPL_MODE_STRICT && !/^\s*$/.test(code)) {
                code = `'use strict'; void 0;\n${code}`;
              }
              script = new vm.Script(code, { filename: file, displayErrors: false });
            } catch (e) {
              if (wrappedCmd) {
                wrappedCmd = false;
                awaitPromise = false;
                code = input;
                wrappedErr = e;
                continue;
              }
              const error = wrappedErr || e;
              if (isRecoverableError(error, code)) err = new Recoverable(error);
              else err = error;
            }
            break;
          }
        }

        // Restore the captures hidden by REPL bookkeeping before user code runs,
        // matching node's default evaluator protocol.
        regExMatcher.exec(savedRegExMatches.join(regExMatchSeparator));

        let finished = false;
        function finishExecution(e, r) {
          if (finished) return;
          finished = true;
          saveRegExpMatches();
          cb(e, r);
        }

        if (!err) {
          const domainErrorsBefore = domainErrorsHandled();
          try {
            const scriptOptions = { displayErrors: false };
            if (self.useGlobal) result = script.runInThisContext(scriptOptions);
            else result = script.runInContext(context, scriptOptions);
          } catch (e) {
            err = e;
            if (process.domain && process.domain.listenerCount("error") > 0) {
              process.domain.emit("error", err);
              return;
            }
            self._handleError(err);
            return;
          }
          // An active domain swallowed the error instead of letting it reach
          // the catch above: node reaches the same state through
          // `process.domain.emit('error')` and returns without calling cb, so
          // the command is neither a value to print nor an error to report.
          if (domainErrorsHandled() !== domainErrorsBefore) return;

          if (awaitPromise && !err) {
            pause();
            const promise = result;
            (async () => {
              try {
                const r = (await promise) && (await promise).value;
                finishExecution(null, r);
              } catch (e) {
                if (process.domain && process.domain.listenerCount("error") > 0) {
                  process.domain.emit("error", e);
                } else {
                  self._handleError(e);
                }
              } finally {
                unpause();
              }
            })();
          }
        }

        if (!awaitPromise || err) finishExecution(err, result);
      }

      const originalEval = eval_;
      self.eval = function REPLEval(code, context, file, cb) {
        const prev = activeReplServer;
        activeReplServer = self;
        lastEvaluatingServer = self;
        try {
          originalEval.call(self, code, context, file, cb);
        } finally {
          activeReplServer = prev;
        }
      };

      self.clearBufferedCommand();

      function completerWrapper(text, cb) {
        complete.call(self, text, self.editorMode ? self.completeOnEditorMode(cb) : cb);
      }

      self.resetContext();

      this.commands = { __proto__: null };
      defineDefaultCommands(this);

      self.writer = options.writer || replExports.writer;
      if (self.writer === writer) {
        writer.options.colors = self.useColors;
        if (options[kStandaloneREPL]) {
          Object.defineProperty(inspect, "replDefaults", {
            get() { return writer.options; },
            set(o) {
              if (typeof o !== "object" || o === null) {
                throw ERR("ERR_INVALID_ARG_TYPE", TypeError,
                          'The "options" argument must be of type object.');
              }
              return Object.assign(writer.options, o);
            },
            enumerable: true, configurable: true,
          });
        }
      }

      function _parseREPLKeyword(keyword, rest) {
        const cmd = this.commands[keyword];
        if (cmd) {
          cmd.action.call(this, rest);
          return true;
        }
        return false;
      }

      self.on("close", function emitExit() {
        if (paused) { pausedBuffer.push(["close"]); return; }
        self.emit("exit");
      });

      let sawSIGINT = false;
      let sawCtrlD = false;
      const prioritizedSigintQueue = new Set();
      self.on("SIGINT", function onSigInt() {
        if (prioritizedSigintQueue.size > 0) {
          for (const task of prioritizedSigintQueue) task();
          return;
        }
        const empty = self.line.length === 0;
        self.clearLine();
        _turnOffEditorMode(self);
        const cmd = self[kBufferedCommandSymbol];
        if (!(cmd && cmd.length > 0) && empty) {
          if (sawSIGINT) { self.close(); sawSIGINT = false; return; }
          self.output.write("(To exit, press Ctrl+C again or Ctrl+D or type .exit)\n");
          sawSIGINT = true;
        } else {
          sawSIGINT = false;
        }
        self.clearBufferedCommand();
        self.lines.level = [];
        self.displayPrompt();
      });

      self.on("line", function onLine(cmd) {
        cmd = cmd || "";
        sawSIGINT = false;

        if (self.editorMode) {
          self[kBufferedCommandSymbol] += cmd + "\n";
          const matches = self._sawKeyPress && !self[kLoadingSymbol]
            ? /^\s+/.exec(cmd) : null;
          if (matches) {
            const prefix = matches[0];
            self.write(prefix);
            self.line = prefix;
            self.cursor = prefix.length;
          }
          _memory.call(self, cmd);
          return;
        }

        const trimmedCmd = cmd.trim();
        if (trimmedCmd) {
          if (trimmedCmd.charAt(0) === "." && trimmedCmd.charAt(1) !== "." &&
              Number.isNaN(Number.parseFloat(trimmedCmd))) {
            const matches = /^\.([^\s]+)\s*(.*)$/.exec(trimmedCmd);
            const keyword = matches && matches[1];
            const rest = matches && matches[2];
            if (_parseREPLKeyword.call(self, keyword, rest) === true) return;
            if (!self[kBufferedCommandSymbol]) {
              self.output.write("Invalid REPL keyword\n");
              finish(null);
              return;
            }
          }
        }

        const evalCmd = self[kBufferedCommandSymbol] + cmd + "\n";
        self.eval(evalCmd, self.context, getREPLResourceName(), finish);

        function finish(e, ret) {
          _memory.call(self, cmd);

          if (e && !self[kBufferedCommandSymbol] && cmd.trim().startsWith("npm ") &&
              !(e instanceof Recoverable)) {
            self.output.write("npm should be run outside of the Node.js REPL, " +
                              "in your normal shell.\n(Press Ctrl+D to exit.)\n");
            self.displayPrompt();
            return;
          }

          if (e instanceof Recoverable && !sawCtrlD) {
            self[kBufferedCommandSymbol] += cmd + "\n";
            self.displayPrompt();
            return;
          }

          if (e) self._handleError(e.err || e);

          self.clearBufferedCommand();
          sawCtrlD = false;

          if (!e && arguments.length === 2 &&
              (!self.ignoreUndefined || ret !== undefined)) {
            if (!self.underscoreAssigned) self.last = ret;
            self.output.write(self.writer(ret) + "\n");
          }

          if (!self.closed && !e) self.displayPrompt();
        }
      });

      self.on("SIGCONT", function onSigCont() {
        if (self.editorMode) {
          self.output.write(`${self._initialPrompt}.editor\n`);
          self.output.write(
            "// Entering editor mode (Ctrl+D to finish, Ctrl+C to cancel)\n");
          self.output.write(`${self[kBufferedCommandSymbol]}\n`);
          self.prompt(true);
        } else {
          self.displayPrompt(true);
        }
      });

      const ttyWrite = self._ttyWrite.bind(self);
      self._ttyWrite = (d, key) => {
        key = key || {};
        if (paused && !(self.breakEvalOnSigint && key.ctrl && key.name === "c")) {
          pausedBuffer.push(["key", [d, key], self.isCompletionEnabled]);
          return;
        }
        if (!self.editorMode || !self.terminal) {
          if (key.ctrl && key.name === "d" && self.cursor === 0 && self.line.length === 0) {
            self.clearLine();
          }
          ttyWrite(d, key);
          return;
        }
        // Editor mode
        if (key.ctrl && !key.shift) {
          switch (key.name) {
            case "d":
              _turnOffEditorMode(self);
              sawCtrlD = true;
              ttyWrite(d, { name: "return" });
              break;
            case "n":
            case "p":
              break;
            default:
              ttyWrite(d, key);
          }
        } else {
          switch (key.name) {
            case "up":
            case "down":
              break;
            case "tab":
              self._previousKey = null;
              ttyWrite(d, key);
              break;
            default:
              ttyWrite(d, key);
          }
        }
      };

      self.displayPrompt();
    }

    setupHistory(historyConfig, cb) {
      // node repl.js: `setupHistory(historyConfig, cb)` where historyConfig is
      // either the plain file path (the long-standing programmatic form) or the
      // options bag `{ filePath, size, onHistoryFileLoaded }`. Both build the
      // ReplHistory manager and expose it as `repl.historyManager`.
      const options = typeof historyConfig === "string"
        ? { filePath: historyConfig } : (historyConfig || {});
      const onLoaded = typeof cb === "function" ? cb : options.onHistoryFileLoaded;
      const done = typeof onLoaded === "function" ? onLoaded : () => {};
      this._historyFilePath = options.filePath;
      this.historyManager = new ReplHistory(this, {
        filePath: options.filePath,
        size: options.size,
        history: options.history,
        removeHistoryDuplicates: this.removeHistoryDuplicates,
      });
      this.historyManager.initialize(done);
    }

    clearBufferedCommand() {
      this[kBufferedCommandSymbol] = "";
    }

    _handleError(e) {
      if (this._userErrorHandler) {
        const state = this._userErrorHandler(e);
        if (state !== "ignore" && state !== "print" && state !== "unhandled") {
          throw ERR_INVALID_STATE(
            'External REPL error handler must return either "ignore", "print"' +
            `, or "unhandled", but received: ${state}`);
        }
        if (state === "ignore") return;
        if (state === "unhandled") return "unhandled";
      }
      let errStack = "";

      if (typeof e === "object" && e !== null) {
        // node's repl.js installs an overrideStackTrace hook that drops every
        // frame from the bottom of the stack up to and including the first one
        // with a null function name. For a throw at the REPL's top level that is
        // the whole stack, which is why node prints just
        // "Uncaught ReferenceError: x is not defined" with no frames, while
        // frames from functions the user defined stay (test-repl-pretty-stack).
        // JSC's textual frames name that same frame "global code"; everything
        // below it is the REPL's own machinery (defaultEval/onLine/emit/…).
        try {
          const st = e.stack;
          if (typeof st === "string" && st.length) {
            const cut = st.search(/^global code@/m);
            if (cut !== -1) e.stack = st.slice(0, cut).replace(/\n+$/, "");
          }
        } catch { /* a throwing/readonly `stack` accessor is not fatal */ }
        if (isError(e)) {
          if (e.stack) {
            if (e.name === "SyntaxError") {
              // node drops every frame from a SyntaxError so the REPL prints
              // just "Uncaught SyntaxError: <message>". Its regex only matches
              // V8's `    at ...` frames, and — unlike V8 — a JSC `.stack`
              // holds ONLY frames, with no leading "SyntaxError: msg" line:
              // `eval@[native code]\n@REPL1:1:11\nglobal code@REPL1:1:1`.
              // So every `name@source` line has to go too. Without this the
              // surviving frames are consumed by the NEXT expectation in
              // test-repl.js, which then stalls for the full timeout.
              // The tail alternation (empty / [native code] / …:line:col) is
              // deliberate, so a message line such as
              // `SyntaxError: Unexpected token '@'` survives the strip.
              e.stack = e.stack.replace(/^REPL\d+:\d+\r?\n/, "")
                .replace(/^\s+at\s.*\n?/gm, "")
                .replace(/^[^\n]*@(?:\[native code\]|[^\n]*:\d+:\d+)?\r?$\n?/gm, "")
                .replace(/\n+$/, "");
              const importErrorStr = "Cannot use import statement outside a module";
              if (String(e.message).includes(importErrorStr)) {
                e.message = "Cannot use import statement inside the Node.js REPL, " +
                  "alternatively use dynamic import: " +
                  toDynamicImport(this.lines[this.lines.length - 1]);
                e.stack = e.stack.replace(/SyntaxError:.*\n/,
                                          `SyntaxError: ${e.message}\n`);
              }
            } else if (this.replMode === REPL_MODE_STRICT) {
              e.stack = e.stack.replace(/(\s+at\s+REPL\d+:)(\d+)/,
                                        (_, pre, line) => pre + (line - 1));
            }
          }
          errStack = this.writer(e);
          // A user-supplied writer may return anything at all
          // (test-repl-options passes a bare `function writer() {}`), and mbun
          // can reach this path for a foreign uncaught error that node would
          // never route into a REPL. Without this the indexing below threw.
          if (typeof errStack !== "string") errStack = String(errStack);
          if (errStack[0] === "[" && errStack[errStack.length - 1] === "]") {
            errStack = errStack.slice(1, -1);
          }
        }
      }

      if (!this.underscoreErrAssigned) this.lastError = e;

      if (this._isStandalone && userUncaughtExceptionListeners() !== 0) {
        process.nextTick(() => {
          process.emit("uncaughtException", e);
          this.clearBufferedCommand();
          this.lines.level = [];
          if (!this.closed) this.displayPrompt();
        });
      } else {
        if (errStack === "") errStack = this.writer(e);
        const lines = errStack.split(/(?<=\n)/);
        let matched = false;
        errStack = "";
        // Indexed loop, not for-of: this is the last step before the error text
        // reaches the terminal, and it runs for errors raised by code that may
        // have just deleted Array.prototype[Symbol.iterator]
        // (test-repl-unsafe-array-iteration). A for-of here would throw inside the
        // reporter and turn the user's TypeError into a REPL crash.
        for (let li = 0; li < lines.length; li++) {
          const line = lines[li];
          if (!matched && /^\[?([A-Z][a-z0-9_]*)*Error/.test(line)) {
            errStack += writer.options.breakLength >= line.length
              ? `Uncaught ${line}` : `Uncaught:\n${line}`;
            matched = true;
          } else {
            errStack += line;
          }
        }
        if (!matched) {
          const ln = lines.length === 1 ? " " : ":\n";
          errStack = `Uncaught${ln}${errStack}`;
        }
        errStack += errStack.endsWith("\n") ? "" : "\n";
        this.output.write(errStack);
        this.clearBufferedCommand();
        this.lines.level = [];
        if (!this.closed) this.displayPrompt();
      }
    }

    close() {
      // node repl.js REPLServer.prototype.close: a terminal REPL must not tear
      // the Interface down while a debounced history write is still pending,
      // or the `close` listener runs before the entries reach disk. The next
      // REPL to open the same NODE_REPL_HISTORY file then reads it empty —
      // which is exactly how test-repl-history-navigation lost every entry
      // test #1 had typed before test #2 tried to navigate them.
      const hm = this.historyManager;
      if (this.terminal && hm && hm.isFlushing && !this._closingOnFlush) {
        this._closingOnFlush = true;
        this.once("flushHistory", () => super.close());
        return;
      }
      process.nextTick(() => super.close());
    }

    createContext() {
      let context;
      if (this.useGlobal) {
        context = G;
      } else {
        context = vm.createContext();
        for (const name of Object.getOwnPropertyNames(G)) {
          if (!globalBuiltinNames().has(name)) {
            try {
              Object.defineProperty(context, name,
                                    Object.getOwnPropertyDescriptor(G, name));
            } catch { /* ignore */ }
          }
        }
        context.global = context;
        // mbun's util.types.isProxy recognises a Proxy by having wrapped the
        // global Proxy constructor and remembered every instance (see
        // node_util_extra); a fresh vm context gets JSC's own unwrapped Proxy,
        // so proxies built inside the REPL would be invisible to it. Share the
        // host's wrapped constructor so the completer's getter/Proxy bail-out
        // can actually see them.
        try {
          const hostProxy = Object.getOwnPropertyDescriptor(G, "Proxy");
          if (hostProxy) Object.defineProperty(context, "Proxy", hostProxy);
        } catch { /* leave the context's own Proxy in place */ }
        const _console = new Console(this.output);
        Object.defineProperty(context, "console", {
          configurable: true, writable: true, value: _console,
        });
      }

      const replModule = new CJSModule("<repl>");
      try {
        replModule.filename = path.resolve("repl");
      } catch {
        replModule.filename = path.resolve(path.dirname(process.execPath), "repl");
      }
      replModule.paths = CJSModule._nodeModulePaths(replModule.filename);

      Object.defineProperty(context, "module", {
        configurable: true, writable: true, value: replModule,
      });
      Object.defineProperty(context, "require", {
        configurable: true, writable: true, value: makeRequireFunction(replModule),
      });

      addBuiltinLibsToObject(context, "<REPL>");
      return context;
    }

    resetContext() {
      this.context = this.createContext();
      this.underscoreAssigned = false;
      this.underscoreErrAssigned = false;
      this.lines = [];
      this.lines.level = [];

      Object.defineProperty(this.context, "_", {
        configurable: true,
        get: () => this.last,
        set: (value) => {
          this.last = value;
          if (!this.underscoreAssigned) {
            this.underscoreAssigned = true;
            this.output.write("Expression assignment to _ now disabled.\n");
          }
        },
      });

      Object.defineProperty(this.context, "_error", {
        configurable: true,
        get: () => this.lastError,
        set: (value) => {
          this.lastError = value;
          if (!this.underscoreErrAssigned) {
            this.underscoreErrAssigned = true;
            this.output.write("Expression assignment to _error now disabled.\n");
          }
        },
      });

      this.emit("reset", this.context);
    }

    displayPrompt(preserveCursor) {
      let prompt = this._initialPrompt;
      if (this[kBufferedCommandSymbol].length) prompt = kMultilinePrompt;
      Interface.prototype.setPrompt.call(this, prompt);
      this.prompt(preserveCursor);
    }

    setPrompt(prompt) {
      this._initialPrompt = prompt;
      Interface.prototype.setPrompt.call(this, prompt);
    }

    complete() {
      Reflect.apply(this.completer, this, arguments);
    }

    completeOnEditorMode(callback) {
      return (err, results) => {
        if (err) return callback(err);
        const [completions, completeOn = ""] = results;
        let result = completions.filter(Boolean);
        if (completeOn && result.length !== 0) result = [commonPrefix(result)];
        callback(null, [result, completeOn]);
      };
    }

    defineCommand(keyword, cmd) {
      if (typeof cmd === "function") {
        cmd = { action: cmd };
      } else if (typeof cmd.action !== "function") {
        throw ERR("ERR_INVALID_ARG_TYPE", TypeError,
                  'The "cmd.action" argument must be of type function. ' +
                  `Received ${typeof cmd.action}`);
      }
      this.commands[keyword] = cmd;
    }
  }

  const kStandaloneREPL = Symbol("kStandaloneREPL");

  function makeRequireFunction(mod) {
    const r = (id) => mod.require(id);
    r.resolve = (request, options) =>
      CJSModule._resolveFilename(request, mod, false, options);
    r.resolve.paths = (request) => CJSModule._resolveLookupPaths(request, mod);
    r.main = process.mainModule;
    r.extensions = CJSModule._extensions;
    r.cache = CJSModule._cache;
    return r;
  }

  function addBuiltinLibsToObject(object, dummyModuleName) {
    const libs = getReplBuiltinLibs();
    const setReal = (name, val) => {
      if (name in G) return;
      Object.defineProperty(object, name, {
        configurable: true, enumerable: false, writable: true, value: val,
      });
    };
    for (const name of libs) {
      if (!isIdentifier(name)) continue;
      if (Object.prototype.hasOwnProperty.call(object, name)) continue;
      let value;
      Object.defineProperty(object, name, {
        get() {
          if (value === undefined) {
            try { value = req(name); } catch { value = undefined; }
          }
          return value;
        },
        set(v) {
          Object.defineProperty(object, name, {
            configurable: true, enumerable: false, writable: true, value: v,
          });
        },
        configurable: true,
        enumerable: false,
      });
    }
    void setReal;
    void dummyModuleName;
  }

  const toDynamicImport = (codeLine) => {
    if (!codeLine) return "";
    const m = /^\s*import\s+(?:([\w$]+)\s*,?\s*)?(?:\*\s*as\s+([\w$]+)\s*)?(?:\{([^}]*)\}\s*)?(?:from\s*)?['"]([^'"]+)['"]/
      .exec(codeLine);
    if (!m) return "";
    const [, def, ns, named, source] = m;
    const awaitDynamicImport = `await import(${JSON.stringify(source)});`;
    if (ns) return `const ${ns} = ${awaitDynamicImport}`;
    const names = [];
    if (def) names.push(`default: ${def}`);
    if (named) {
      for (const part of named.split(",")) {
        const p = part.trim();
        if (!p) continue;
        const asMatch = /^([\w$]+)\s+as\s+([\w$]+)$/.exec(p);
        names.push(asMatch ? `${asMatch[1]}: ${asMatch[2]}` : p);
      }
    }
    if (names.length === 0) return awaitDynamicImport;
    return `const { ${names.join(", ")} } = ${awaitDynamicImport}`;
  };

  function start(prompt, source, eval_, useGlobal, ignoreUndefined, replMode) {
    return new REPLServer(prompt, source, eval_, useGlobal, ignoreUndefined, replMode);
  }

  function _memory(cmd) {
    const self = this;
    self.lines = self.lines || [];
    self.lines.level = self.lines.level || [];
    if (cmd) {
      const len = self.lines.level.length ? self.lines.level.length - 1 : 0;
      self.lines.push("  ".repeat(len) + cmd);
    } else {
      self.lines.push("");
    }
    if (!cmd) { self.lines.level = []; return; }

    const countMatches = (regex, str) => {
      let count = 0;
      let m;
      while ((m = regex.exec(str)) !== null) { count++; if (m.index === regex.lastIndex) regex.lastIndex++; }
      return count;
    };
    const dw = countMatches(/[{(]/g, cmd);
    const up = countMatches(/[})]/g, cmd);
    let depth = dw - up;
    if (depth) {
      (function workIt() {
        if (depth > 0) {
          self.lines.level.push({ line: self.lines.length - 1, depth });
        } else if (depth < 0) {
          const curr = self.lines.level.pop();
          if (curr) {
            const tmp = curr.depth + depth;
            if (tmp < 0) { depth += curr.depth; workIt(); }
            else if (tmp > 0) { curr.depth += depth; self.lines.level.push(curr); }
          }
        }
      }());
    }
  }

  function _turnOnEditorMode(repl) {
    repl.editorMode = true;
    Interface.prototype.setPrompt.call(repl, "");
  }

  function _turnOffEditorMode(repl) {
    repl.editorMode = false;
    repl.setPrompt(repl._initialPrompt);
  }

  function defineDefaultCommands(repl) {
    repl.defineCommand("break", {
      help: "Sometimes you get stuck, this gets you out",
      action: function () { this.clearBufferedCommand(); this.displayPrompt(); },
    });

    const clearMessage = repl.useGlobal
      ? "Alias for .break" : "Break, and also clear the local context";
    repl.defineCommand("clear", {
      help: clearMessage,
      action: function () {
        this.clearBufferedCommand();
        if (!this.useGlobal) {
          this.output.write("Clearing context...\n");
          this.resetContext();
        }
        this.displayPrompt();
      },
    });

    repl.defineCommand("exit", {
      help: "Exit the REPL",
      action: function () { this.close(); },
    });

    repl.defineCommand("help", {
      help: "Print this help message",
      action: function () {
        const names = Object.keys(this.commands).sort();
        const longestNameLength = Math.max(...names.map((name) => name.length));
        for (const name of names) {
          const cmd = this.commands[name];
          const spaces = " ".repeat(longestNameLength - name.length + 3);
          this.output.write(`.${name}${cmd.help ? spaces + cmd.help : ""}\n`);
        }
        this.output.write("\nPress Ctrl+C to abort current expression, " +
                          "Ctrl+D to exit the REPL\n");
        this.displayPrompt();
      },
    });

    repl.defineCommand("save", {
      help: "Save all evaluated commands in this REPL session to a file",
      action: function (file) {
        try {
          if (file === "") throw ERR_MISSING_ARGS("file");
          fs.writeFileSync(file, this.lines.join("\n"));
          this.output.write(`Session saved to: ${file}\n`);
        } catch (error) {
          if (error && error.code === "ERR_MISSING_ARGS") {
            this.output.write(`${error.message}\n`);
          } else {
            this.output.write(`Failed to save: ${file}\n`);
          }
        }
        this.displayPrompt();
      },
    });

    repl.defineCommand("load", {
      help: "Load JS from a file into the REPL session",
      action: function (file) {
        try {
          if (file === "") throw ERR_MISSING_ARGS("file");
          const stats = fs.statSync(file);
          if (stats && stats.isFile()) {
            _turnOnEditorMode(this);
            this[kLoadingSymbol] = true;
            const data = fs.readFileSync(file, "utf8");
            this.write(data);
            this[kLoadingSymbol] = false;
            _turnOffEditorMode(this);
            this.write("\n");
          } else {
            this.output.write(`Failed to load: ${file} is not a valid file\n`);
          }
        } catch (error) {
          if (error && error.code === "ERR_MISSING_ARGS") {
            this.output.write(`${error.message}\n`);
          } else {
            this.output.write(`Failed to load: ${file}\n`);
          }
        }
        this.displayPrompt();
      },
    });

    if (repl.terminal) {
      repl.defineCommand("editor", {
        help: "Enter editor mode",
        action() {
          _turnOnEditorMode(this);
          this.output.write(
            "// Entering editor mode (Ctrl+D to finish, Ctrl+C to cancel)\n");
        },
      });
    }
  }

  // node lib/internal/repl.js createRepl(): the factory the CLI uses for
  // `node -i` / `node --interactive`. It lives in this partition because
  // kStandaloneREPL is module-private, and is attached to the module object
  // non-enumerably below so `Object.keys(require("repl"))` still reports only
  // node's public surface. Its one caller is the -i CLI path in src/app.cppm,
  // which stands in for node's lib/internal/main/repl.js.
  function createInternalRepl(env, opts, cb) {
    if (typeof opts === "function") { cb = opts; opts = null; }
    opts = Object.assign(
      { ignoreUndefined: false, useGlobal: true, breakEvalOnSigint: true },
      opts);
    opts[kStandaloneREPL] = true;
    if (parseInt(env.NODE_NO_READLINE, 10)) opts.terminal = false;
    if (env.NODE_REPL_MODE) {
      opts.replMode = { strict: REPL_MODE_STRICT, sloppy: REPL_MODE_SLOPPY }[
        String(env.NODE_REPL_MODE).toLowerCase().trim()];
    }
    if (opts.replMode === undefined) opts.replMode = REPL_MODE_SLOPPY;
    const size = Number(env.NODE_REPL_HISTORY_SIZE);
    opts.size = (!Number.isNaN(size) && size > 0) ? size : 1000;
    // No history file unless the session is a terminal — a piped stdin must not
    // read or rewrite the user's ~/.node_repl_history.
    const term = "terminal" in opts ? opts.terminal : process.stdout.isTTY;
    opts.filePath = term ? env.NODE_REPL_HISTORY : "";
    const repl = start(opts);
    repl.setupHistory({ filePath: opts.filePath, size: opts.size, onHistoryFileLoaded: cb });
    return repl;
  }

  const replExports = {
    start,
    writer,
    REPLServer,
    REPL_MODE_SLOPPY,
    REPL_MODE_STRICT,
    Recoverable,
    isValidSyntax,
  };

  Object.defineProperty(replExports, "createInternalRepl", {
    value: createInternalRepl, writable: true, configurable: true, enumerable: false,
  });

  Object.defineProperty(replExports, "builtinModules", {
    get: () => getReplBuiltinLibs(),
    set: (v) => setReplBuiltinLibs(v),
    enumerable: false, configurable: true,
  });
  Object.defineProperty(replExports, "_builtinLibs", {
    get: () => getReplBuiltinLibs(),
    set: (v) => setReplBuiltinLibs(v),
    enumerable: false, configurable: true,
  });

  M["repl"] = replExports;
  M["node:repl"] = replExports;

  // node's `internal/repl` and `internal/repl/await`, served from THIS repl.
  //
  // Under --expose-internals the corpus requires those two ids directly. Left
  // alone they resolve, through compat/node/tsconfig.json's
  // `"internal/*": ["./lib/internal/*"]`, to node's real lib files, and node's
  // own chain then dies twice over: internal/repl/utils.js wants the acorn
  // copy vendored under deps/ (not lib/internal/deps/, so the mapping can never
  // reach it), and past that internal/vm.js wants a contextify binding that
  // exposes ContextifyScript, which mbun's internalBinding("contextify") does
  // not. Registering them as builtins short-circuits require() before the
  // resolver runs (see builtin_module in runtime/process_extended.inc), so
  // node's lib chain is bypassed entirely.
  //
  // Shape is node's: lib/internal/repl.js is `{ __proto__: REPL }` plus an own
  // `createInternalRepl`, and lib/internal/repl/await.js exports exactly
  // `{ processTopLevelAwait }`.
  //
  // GATED. `internal/*` is node-internal namespace: handing it to an ordinary
  // program would let any script — or a package shipping its own
  // `internal/repl` — be shadowed by this. So the two entries are accessors
  // that yield the module only under node's own flag, --expose-internals, and
  // `undefined` otherwise; builtin_module treats undefined as "not a builtin"
  // and falls through to normal resolution. The check has to be lazy because
  // the builtins image is evaluated before process.execArgv exists (same
  // reason node_vm_modules gates vm.Module lazily). Non-enumerable so
  // `Object.keys(M)` — the source of module.builtinModules — never lists them.
  const exposeInternals = () => {
    const argv = (G.process && G.process.execArgv) || [];
    for (const a of argv) if (a === "--expose-internals") return true;
    return false;
  };
  const internalRepl = Object.create(replExports);
  Object.defineProperty(internalRepl, "createInternalRepl", {
    value: createInternalRepl, writable: true, configurable: true, enumerable: true,
  });
  const internalReplAwait = { processTopLevelAwait };
  for (const [id, value] of [["internal/repl", internalRepl],
                             ["internal/repl/await", internalReplAwait]]) {
    Object.defineProperty(M, id, {
      get() { return exposeInternals() ? value : undefined; },
      set(v) {
        Object.defineProperty(M, id, {
          value: v, writable: true, enumerable: false, configurable: true,
        });
      },
      enumerable: false, configurable: true,
    });
  }
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
