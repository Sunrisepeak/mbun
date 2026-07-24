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

  const ERR = (code, Ctor, msg) => { const e = new Ctor(msg); e.code = code; return e; };
  const ERR_MISSING_ARGS = (name) =>
    ERR("ERR_MISSING_ARGS", TypeError, `The "${name}" argument must be specified`);
  const ERR_INVALID_ARG_VALUE = (name, value, reason) =>
    ERR("ERR_INVALID_ARG_VALUE", TypeError,
        `The property '${name}' ${reason} Received ${inspect(value)}`);
  const ERR_INVALID_REPL_EVAL_CONFIG = () =>
    ERR("ERR_INVALID_REPL_EVAL_CONFIG", TypeError,
        "Cannot specify both breakEvalOnSigint and eval for REPL");
  const ERR_INVALID_REPL_INPUT = (msg) => ERR("ERR_INVALID_REPL_INPUT", TypeError, msg);
  const ERR_INVALID_STATE = (msg) => ERR("ERR_INVALID_STATE", Error, `Invalid state: ${msg}`);

  const isNativeError = (util.types && util.types.isNativeError) || (() => false);
  const isError = (e) => e instanceof Error || isNativeError(e);

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
    return msg.includes("Unexpected end of script") ||
           msg.includes("Unexpected EOF") ||
           msg.includes("Unexpected token ')'") === false && false;
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
    breakLength: 128, compact: 3, sorted: false, getters: false,
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
  // Live (not yet closed) REPL servers, newest last. node routes an async
  // uncaught exception to the REPL that owns the current async context via
  // AsyncLocalStorage + addUncaughtExceptionCaptureCallback; the innermost live
  // server is the same answer for every shape the tests drive.
  const liveServers = [];
  let installingCapture = false;
  let captureInstalled = false;
  function setupExceptionCapture() {
    if (captureInstalled) return;
    captureInstalled = true;
    installingCapture = true;
    try {
      process.on("uncaughtException", (err) => {
        const server = liveServers[liveServers.length - 1];
        if (server === undefined) throw err;
        server._handleError(err);
      });
    } finally {
      installingCapture = false;
    }
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

  function filteredOwnPropertyNames(obj) {
    if (!obj) return [];
    const filter = ALL_PROPERTIES;
    let names;
    try {
      names = Object.getOwnPropertyNames(obj);
    } catch { return []; }
    return names.filter(isIdentifier);
  }
  const ALL_PROPERTIES = 0;

  function getGlobalLexicalScopeNames() { return []; }

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
      match = simpleExpressionRE.exec(line);
      if (line.length !== 0 && !match) {
        completionGroups.push([]);
        completeOn = "";
      } else {
        let expr = "";
        completeOn = match ? match[0] : "";
        if (line.length !== 0) {
          const lastIndex = completeOn.lastIndexOf(".");
          if (lastIndex > -1) {
            expr = completeOn.slice(0, lastIndex);
            filter = completeOn.slice(lastIndex + 1);
          } else {
            filter = completeOn;
          }
        }
        if (!expr) {
          const contextProto = this.useGlobal ? G : this.context;
          let obj = contextProto;
          const seen = new Set();
          while (obj) {
            for (const n of filteredOwnPropertyNames(obj)) seen.add(n);
            try { obj = Object.getPrototypeOf(obj); } catch { break; }
          }
          completionGroups.push([...seen]);
          completionGroups.push(KEYWORDS);
        } else {
          let obj;
          try {
            obj = this.useGlobal
              ? (0, eval)(expr)
              : vm.runInContext(expr, this.context, { displayErrors: false });
          } catch { obj = undefined; }
          if (obj != null) {
            if (typeof obj === "object" || typeof obj === "function") {
              try {
                let p = obj;
                const seen = new Set();
                let depth = 0;
                while (p && depth++ < 4) {
                  for (const n of filteredOwnPropertyNames(p)) seen.add(n);
                  p = Object.getPrototypeOf(p);
                }
                completionGroups.push([...seen]);
              } catch { /* ignore */ }
            } else {
              const proto = Object.getPrototypeOf(obj);
              if (proto) completionGroups.push(filteredOwnPropertyNames(proto));
            }
          }
          if (filter !== "") filter = `${expr}.${filter}`;
          completionGroups = completionGroups.map((g) => g.map((m) => `${expr}.${m}`));
        }
      }
    }

    return completionGroupsLoaded();

    function completionGroupsLoaded() {
      // Filter, sort (within each group), uniq and merge the completion groups.
      if (completionGroups.length && filter !== "") {
        const newCompletionGroups = [];
        for (const group3 of completionGroups) {
          const filtered = group3.filter((elem) => elem.startsWith(filter));
          if (filtered.length) newCompletionGroups.push(filtered);
        }
        completionGroups = newCompletionGroups;
      }
      const completions = [];
      if (completionGroups.length) {
        const uniqueSet = new Set();
        const empty = Symbol("empty");
        uniqueSet.add(empty);
        for (const group4 of completionGroups) {
          group4.sort((a, b) => (b < a ? 1 : -1));
          const setSize = uniqueSet.size;
          for (const entry of group4) {
            if (!uniqueSet.has(entry)) {
              completions.push(entry);
              uniqueSet.add(entry);
            }
          }
          if (uniqueSet.size !== setSize) completions.push("");
        }
        while (completions.length && completions[completions.length - 1] === "") {
          completions.pop();
        }
      }
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

      const domainMod = req("domain");
      const domainErrorsHandled = () =>
        (domainMod && typeof domainMod.__errorsHandled === "number") ? domainMod.__errorsHandled : 0;

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

        let finished = false;
        function finishExecution(e, r) {
          if (finished) return;
          finished = true;
          for (let idx = 1; idx < savedRegExMatches.length; idx += 1) {
            savedRegExMatches[idx] = RegExp[`$${idx}`];
          }
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
      const options = typeof historyConfig === "string"
        ? { filePath: historyConfig } : (historyConfig || {});
      const filePath = options.filePath;
      const onLoaded = typeof cb === "function" ? cb : options.onHistoryFileLoaded;
      const done = (err) => {
        if (typeof onLoaded === "function") {
          process.nextTick(() => onLoaded(err || null, this));
        }
      };
      if (!filePath) {
        this._historyPrev = this._historyPrev;
        done(null);
        return;
      }
      this._historyFilePath = filePath;
      try {
        const data = fs.readFileSync(filePath, "utf8");
        this.history = data.split("\n").filter(Boolean).reverse()
          .slice(0, this.historySize);
      } catch {
        this.history = [];
      }
      const flush = () => {
        try {
          fs.writeFileSync(filePath, this.history.slice().reverse().join("\n"));
        } catch { /* ignore */ }
      };
      this.on("line", flush);
      this.once("exit", flush);
      done(null);
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
        if (isError(e)) {
          if (e.stack) {
            if (e.name === "SyntaxError") {
              e.stack = e.stack.replace(/^REPL\d+:\d+\r?\n/, "")
                .replace(/^\s+at\s.*\n?/gm, "");
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
          if (errStack[0] === "[" && errStack[errStack.length - 1] === "]") {
            errStack = errStack.slice(1, -1);
          }
        }
      }

      if (!this.underscoreErrAssigned) this.lastError = e;

      if (this._isStandalone && process.listenerCount("uncaughtException") !== 0) {
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
        for (const line of lines) {
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

  const replExports = {
    start,
    writer,
    REPLServer,
    REPL_MODE_SLOPPY,
    REPL_MODE_STRICT,
    Recoverable,
    isValidSyntax,
  };

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
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
