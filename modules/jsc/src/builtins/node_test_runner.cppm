// node:test standalone runner partition.
//
// bootstrap.cppm maps node:test onto the bun:test harness (globalThis.__mbunBT),
// which only exists under `mbun test`. Node's own corpus files are plain scripts
// executed directly (`mbun file.js`): there is no harness, so every
// `test(name, fn)` threw "bt.test is not a function" before the first assertion
// ran. This partition installs a self-contained runner used exactly in that
// case — when __mbunBT is absent the calls go here, otherwise the bootstrap
// delegation to bun:test is kept untouched.
//
// Semantics modeled on node lib/internal/test_runner: tests run sequentially in
// registration order, a suite (describe) collects its children synchronously and
// runs them after their before/beforeEach hooks, subtests registered on the test
// context are awaited before the parent finishes, and a failure sets
// process.exitCode = 1 (which is what "the file passes" means for the corpus).
//
// NOTE: appended AFTER the master builtins IIFE, so this is a self-contained
// IIFE that re-binds G = globalThis. Top level must never throw.
export module mbun.jsc.js_builtins:node_test_runner;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kNodeTestRunnerJS = R"JS(
(function () {
  const G = globalThis;
  try {
    const M = G.__mbunNativeModules;
    if (!M) return;
    const delegate = M["test"];
    if (typeof delegate !== "function") return;

    const assertMod = () => M["assert"] || M["node:assert"];

    // ------------------------------------------------- reporting surface ----
    // node routes every runner observation through one event vocabulary
    // (lib/internal/test_runner/tests_stream.js) and lets the consumers —
    // TAP on stdout, a TestsStream handed back by run(), a child process
    // reporting to its parent — subscribe to it. mbun's runner does the same so
    // a test reports exactly once no matter who is listening, instead of the
    // TAP writer being the only thing that ever sees a result.
    //
    // The `:node_test_run` partition is the second subscriber: it turns these
    // events into run()'s TestsStream, and in a child process (NODE_TEST_CONTEXT
    // set) forwards them over the IPC channel and turns TAP off.
    const subscribers = [];
    let tapEnabled = true;
    // run({isolation:'none'}) evaluates OTHER files' tests in this process; their
    // failures belong to the returned stream, not to this process's exit status
    // (a runner script that reports a failing file still exits 0).
    let ownExitCode = true;
    const emit = (type, data) => {
      for (let i = 0; i < subscribers.length; i++) {
        try { subscribers[i](type, data); } catch (e) {}
      }
    };
    const out = (line) => { if (tapEnabled) { try { G.console.log(line); } catch (e) {} } };

    // ------------------------------------------------------------- mocking
    const makeMock = () => {
      const tracked = [];
      const mockFn = (original, implementation, options) => {
        if (typeof original === "object" && original !== null) { options = original; original = undefined; }
        if (typeof implementation === "object" && implementation !== null) { options = implementation; implementation = undefined; }
        const base = typeof original === "function" ? original : function () {};
        let impl = typeof implementation === "function" ? implementation : base;
        const times = options && typeof options.times === "number" ? options.times : Infinity;
        let used = 0;
        const calls = [];
        const fn = function (...args) {
          const target = used++ < times ? impl : base;
          const record = { arguments: args, this: this, result: undefined, error: undefined, stack: null, target: undefined };
          calls.push(record);
          try {
            record.result = target.apply(this, args);
            return record.result;
          } catch (e) {
            record.error = e;
            throw e;
          }
        };
        fn.mock = {
          calls,
          callCount: () => calls.length,
          mockImplementation: (f) => { impl = f; },
          mockImplementationOnce: (f, at) => {
            const once = f;
            const prev = impl;
            const index = typeof at === "number" ? at : calls.length;
            // The call record is pushed BEFORE the implementation runs, so the
            // index of the call in progress is calls.length - 1. Comparing
            // against calls.length meant the once-implementation was never the
            // one selected (test-fs-write-stream-eagain's mocked EAGAIN write
            // silently ran the real fs.write instead).
            impl = function (...a) { return (calls.length - 1 === index ? once : prev).apply(this, a); };
          },
          resetCalls: () => { calls.length = 0; },
          restore: () => { impl = base; },
        };
        tracked.push(fn.mock);
        return fn;
      };
      const method = (object, name, implementation, options) => {
        const original = object[name];
        const fn = mockFn(typeof original === "function" ? original : function () {}, implementation, options);
        fn.mock.restore = () => { object[name] = original; };
        tracked.push(fn.mock);
        object[name] = fn;
        return fn;
      };
      // Find the descriptor wherever it actually lives — a stream's `destroyed`
      // is defined on the prototype, not on the instance.
      const findDescriptor = (object, name) => {
        let o = object;
        while (o != null) {
          const d = Object.getOwnPropertyDescriptor(o, name);
          if (d) return d;
          o = Object.getPrototypeOf(o);
        }
        return undefined;
      };
      const accessor = (object, name, kind, implementation, options) => {
        const desc = findDescriptor(object, name);
        const original = desc && typeof desc[kind] === "function"
          ? desc[kind]
          : (kind === "get" ? function () { return desc ? desc.value : undefined; } : function () {});
        const fn = mockFn(original, implementation, options);
        const next = {
          configurable: true,
          enumerable: desc ? desc.enumerable !== false : true,
          get: kind === "get" ? fn : (desc && desc.get),
          set: kind === "set" ? fn : (desc && desc.set),
        };
        if (next.get === undefined) delete next.get;
        if (next.set === undefined) delete next.set;
        Object.defineProperty(object, name, next);
        fn.mock.restore = () => {
          if (desc && Object.getOwnPropertyDescriptor(object, name)) {
            try { Object.defineProperty(object, name, desc); } catch (e) {}
          } else {
            try { delete object[name]; } catch (e) {}
          }
        };
        tracked.push(fn.mock);
        return fn;
      };
      return {
        fn: mockFn,
        method,
        // mock.getter/setter replace an ACCESSOR, so they must go through
        // defineProperty — assigning object[name] = fn (what `method` does)
        // either silently fails against a prototype getter or turns the
        // property into a plain function value. `mock.getter(stream,
        // 'destroyed', () => true)` was a no-op for exactly that reason
        // (test-fs-write-stream-eagain).
        getter: (object, name, implementation, options) => accessor(object, name, "get", implementation, options),
        setter: (object, name, implementation, options) => accessor(object, name, "set", implementation, options),
        reset: () => { for (const m of tracked) { try { m.resetCalls(); } catch (e) {} } tracked.length = 0; },
        restoreAll: () => { for (const m of tracked) { try { m.restore(); } catch (e) {} } },
        timers: { enable() {}, reset() {}, tick() {}, runAll() {}, setTime() {} },
      };
    };

    // ---------------------------------------------------------- test tree
    // A node is { kind: "test"|"suite", name, fn, opts, children, hooks, parent }.
    const makeNode = (kind, name, opts, fn, parent) => ({
      kind, name: name === undefined ? "<anonymous>" : String(name),
      opts: opts || {}, fn: fn || null, parent: parent || null,
      children: [],
      __loc: captureLoc(),
      // node lib/internal/test_runner/test.js: `runOnlySubtests` is set on the
      // PARENT of an `only` test, `hasOnlyTests` is propagated up the ancestor
      // chain from there; Test#filter() reads both.
      runOnlySubtests: false,
      hasOnlyTests: false,
      hooks: { before: [], after: [], beforeEach: [], afterEach: [] },
    });

    const root = makeNode("suite", "<root>", {}, null, null);
    const state = { collecting: null, chain: Promise.resolve(), failures: 0, index: 0, scheduled: false,
                    current: undefined, barrier: null, topLevel: 0, filterByOnly: false };

    // node's TestContext#fullName joins the ancestor names with " > " and drops
    // the root; the root's own hooks see "<root>" (test-runner-test-fullname).
    const fullNameOf = (node) => {
      const parts = [];
      for (let n = node; n && n !== root; n = n.parent) parts.unshift(n.name);
      return parts.length ? parts.join(" > ") : "<root>";
    };
    // node's TestContext#filePath is the file the RUNNER was given, not the
    // module that happened to call test() — a suite required from another file
    // still reports the entry test file (test-runner-test-filepath).
    const entryFile = () => { try { return G.process.argv[1]; } catch (e) { return undefined; } };

    // node lib/internal/test_runner/assert.js: `assert.register(name, fn)` adds a
    // custom assertion to every TestContext#assert, and may override a built-in.
    const customAssertions = new Map();
    const argTypeError = (name, expected, value) => {
      const t = typeof value;
      const recv = value === null ? "null"
        : t === "string" ? "type string ('" + value + "')"
        : t === "object" ? "an instance of " + ((value.constructor && value.constructor.name) || "Object")
        : "type " + t + " (" + String(value) + ")";
      const e = new TypeError('The "' + name + '" argument must be of type ' + expected + '. Received ' + recv);
      e.code = "ERR_INVALID_ARG_TYPE";
      return e;
    };
    const propTypeError = (name, expected, value) => {
      const e = new TypeError('The "' + name + '" property must be of type ' + expected +
                              ". Received type " + typeof value + " (" + String(value) + ")");
      e.code = "ERR_INVALID_ARG_TYPE";
      return e;
    };
    // node lib/internal/test_runner/tag_filter.js validateTags(): the option is
    // validated where the Test/Suite is CONSTRUCTED, so test/suite/describe/it
    // all reject the same shapes. Hooks never reach here — they take no options
    // bag — which is why before(fn, { tags }) is silently tolerated.
    const validateTags = (opts) => {
      if (opts === null || typeof opts !== "object") return;
      const tags = opts.tags;
      if (tags === undefined) return;
      if (!Array.isArray(tags)) throw argTypeError("options.tags", "an array", tags);
      for (const tag of tags) {
        if (typeof tag !== "string") throw argTypeError("options.tags[]", "string", tag);
        if (tag.length === 0) {
          const e = new TypeError(
            "The argument 'options.tags' must not contain an empty string. Received ''");
          e.code = "ERR_INVALID_ARG_VALUE";
          throw e;
        }
      }
    };
    const rangeError = (name, range, value) => {
      const e = new RangeError('The value of "' + name + '" is out of range. It must be ' +
                               range + ". Received " + String(value));
      e.code = "ERR_OUT_OF_RANGE";
      return e;
    };
    // node lib/internal/test_runner/test.js Test's constructor validates
    // `options.timeout` with validateNumber(0, TIMEOUT_MAX) and
    // `options.concurrency` with validateUint32(positive) — both BEFORE the test
    // is registered, so `test({ timeout: -1 })` throws synchronously
    // (test-runner-option-validation). Neither was checked here at all.
    const TIMEOUT_MAX = 2147483647;
    const validateTestOptions = (opts) => {
      if (opts === null || typeof opts !== "object") return;
      const timeout = opts.timeout;
      if (timeout !== undefined && timeout !== null && timeout !== Infinity) {
        if (typeof timeout !== "number") throw argTypeError("options.timeout", "number", timeout);
        if (timeout < 0 || timeout > TIMEOUT_MAX || Number.isNaN(timeout)) {
          throw rangeError("options.timeout", ">= 0 && <= " + TIMEOUT_MAX, timeout);
        }
      }
      const concurrency = opts.concurrency;
      if (concurrency !== undefined && concurrency !== null && typeof concurrency !== "boolean") {
        if (typeof concurrency !== "number") {
          const e = new TypeError('The "options.concurrency" argument must be one of type boolean' +
                                  " or number. Received " + typeof concurrency);
          e.code = "ERR_INVALID_ARG_TYPE";
          throw e;
        }
        if (!Number.isInteger(concurrency)) {
          throw rangeError("options.concurrency", "an integer", concurrency);
        }
        if (concurrency < 1 || concurrency > 4294967295) {
          throw rangeError("options.concurrency", ">= 1 && <= 4294967295", concurrency);
        }
      }
    };
    const testAssert = {
      register(name, fn) {
        if (typeof name !== "string") throw argTypeError("name", "string", name);
        if (typeof fn !== "function") throw argTypeError("fn", "function", fn);
        customAssertions.set(name, fn);
      },
    };

    // node lib/internal/test_runner/snapshot.js already provides the snapshot
    // file manager. The standalone runner owns the TestContext, so it creates
    // one manager lazily and shares it across contexts in this process.
    let snapshotRuntime;
    const snapshotRequire = () => {
      try {
        const moduleApi = M["module"] || M["node:module"];
        const createRequire = moduleApi && moduleApi.createRequire;
        const file = entryFile();
        if (typeof createRequire === "function" && typeof file === "string") {
          return createRequire(file);
        }
      } catch (error) {}
      if (typeof G.require === "function") return G.require;
      return undefined;
    };
    const getSnapshotRuntime = () => {
      if (snapshotRuntime !== undefined) return snapshotRuntime;
      const req = snapshotRequire();
      if (typeof req !== "function") return undefined;
      const previousRequire = G.require;
      const replaceGlobalRequire = previousRequire !== req;
      if (replaceGlobalRequire) G.require = req;
      try {
        const snapshotModule = req("internal/test_runner/snapshot");
        if (!snapshotModule || typeof snapshotModule.SnapshotManager !== "function") return undefined;
        const argvHas = (list, flag) => Array.isArray(list) && list.indexOf(flag) !== -1;
        const processObj = G.process;
        const updateSnapshots = argvHas(processObj && processObj.argv, "--test-update-snapshots") ||
          argvHas(processObj && processObj.execArgv, "--test-update-snapshots");
        const manager = new snapshotModule.SnapshotManager(updateSnapshots);
        const assertion = manager.createAssert();
        const fileAssertion = manager.createFileAssert();
        if (processObj && typeof processObj.on === "function") {
          processObj.on("exit", () => {
            try { manager.writeSnapshotFiles(); } catch (error) { processObj.exitCode = 1; }
          });
        }
        snapshotRuntime = { snapshotModule, assertion, fileAssertion };
        return snapshotRuntime;
      } catch (error) {
        return undefined;
      } finally {
        if (replaceGlobalRequire) {
          if (previousRequire === undefined) delete G.require;
          else G.require = previousRequire;
        }
      }
    };
    const snapshotApi = {
      setResolveSnapshotPath(...a) {
        const runtime = getSnapshotRuntime();
        if (!runtime) throw new Error("Snapshot support is unavailable");
        return runtime.snapshotModule.setResolveSnapshotPath(...a);
      },
      setDefaultSnapshotSerializers(...a) {
        const runtime = getSnapshotRuntime();
        if (!runtime) throw new Error("Snapshot support is unavailable");
        return runtime.snapshotModule.setDefaultSnapshotSerializers(...a);
      },
    };

    // node exposes the context of the innermost running test/suite; mbun's
    // standalone runner is strictly sequential, so one slot is enough (it also
    // survives an await/setImmediate inside the body, which is what
    // test-runner-get-test-context checks).
    const getTestContext = () => state.current;
    const withContext = async (ctx, fn) => {
      const previous = state.current;
      state.current = ctx;
      try { return await fn(); } finally { state.current = previous; }
    };

    // `reported` = a test:fail event has already gone out for this failure. The
    // paths that have no test node to report (a throwing describe() body, a
    // failing root hook) pass false and get a synthetic top-level failure, so a
    // TAP document never loses a failure it counted.
    const fail = (name, error, reported) => {
      state.failures += 1;
      if (reported !== true) {
        emit("test:fail", {
          name, nesting: 0, testNumber: ++state.index, tags: [],
          details: { duration_ms: 0, type: "test", error },
        });
      }
      if (ownExitCode) { try { G.process.exitCode = 1; } catch (e) {} }
    };
    const ok = (name) => {};
    const skipped = (name) => {};

    // ------------------------------------------------------- the TAP writer ---
    // PORT-SOURCE: node lib/internal/test_runner/reporter/tap.js. A direct
    // `mbun file.js` run gets node's TAP document: version line, `# Subtest:`
    // banners, a YAML block per result and the counted trailer. The ad-hoc
    // "ok N - name" lines this replaced carried none of it, and the corpus greps
    // all three — `duration_ms` (test-runner-root-duration), `cancelled 1`
    // (test-runner-misc), `failureType` (the error-reporter cluster).
    //
    // Written SYNCHRONOUSLY as the events arrive, not through the async-generator
    // reporter :node_test_run exposes: the trailer has to go out from
    // process.on('exit'), where nothing asynchronous can still be flushed.
    const tap = {
      counts: { tests: 0, suites: 0, pass: 0, fail: 0, cancelled: 0, skipped: 0, todo: 0 },
      duration: 0, topLevel: 0, header: false, trailer: false,
    };
    const tapEscape = (text) => String(text).replace(/\\/g, "\\\\").replace(/#/g, "\\#");
    const pad = (nesting) => "    ".repeat(nesting);
    const tapHeader = () => {
      if (tap.header) return;
      tap.header = true;
      out("TAP version 13");
    };
    const tapYaml = (nesting, data) => {
      const p = pad(nesting);
      const ms = data.details && data.details.duration_ms !== undefined ? data.details.duration_ms : 0;
      out(p + "  ---");
      out(p + "  duration_ms: " + ms);
      if (data.file !== undefined && data.line !== undefined) {
        out(p + "  location: '" + data.file + ":" + data.line + ":" + data.column + "'");
      }
      const error = data.details && data.details.error;
      if (error) {
        if (error.failureType !== undefined) out(p + "  failureType: '" + error.failureType + "'");
        out(p + "  error: '" + String(error.message === undefined ? error : error.message).replace(/'/g, "''") + "'");
        if (error.code !== undefined) out(p + "  code: '" + error.code + "'");
        if (error.stack !== undefined) {
          out(p + "  stack: |-");
          for (const line of String(error.stack).split("\n")) out(p + "    " + line);
        }
      }
      out(p + "  ...");
    };
    const tapResult = (type, data) => {
      tapHeader();
      const failed = type === "test:fail";
      // node lib/internal/test_runner/utils.js countCompletedTest(): a SUITE
      // increments `suites` and RETURNS — it never lands in tests/pass/fail/
      // skipped/todo/cancelled. Counting suites in `pass` too made every
      // `# pass N` assertion over a file that uses describe()/suite() off by the
      // number of suites (test-runner-tag-filter-cli: `# pass 13` for 10 tests
      // and 3 suites).
      const isSuite = !!(data.details && data.details.type === "suite");
      if (isSuite) {
        tap.counts.suites += 1;
      } else {
        tap.counts.tests += 1;
        if (data.skip !== undefined) tap.counts.skipped += 1;
        else if (data.todo !== undefined) tap.counts.todo += 1;
        else if (failed) {
          if (data.details && data.details.error && data.details.error.failureType === "cancelledByParent") {
            tap.counts.cancelled += 1;
          } else tap.counts.fail += 1;
        } else tap.counts.pass += 1;
      }
      const number = data.nesting === 0 ? ++tap.topLevel
        : (data.testNumber === undefined ? 1 : data.testNumber);
      const directive = data.skip !== undefined
        ? " # SKIP" + (typeof data.skip === "string" ? " " + tapEscape(data.skip) : "")
        : data.todo !== undefined
          ? " # TODO" + (typeof data.todo === "string" ? " " + tapEscape(data.todo) : "")
          : data.expectFailure !== undefined
            ? " # EXPECTED FAILURE" +
              (typeof data.expectFailure === "string" ? " " + tapEscape(data.expectFailure) : "")
            : "";
      out(pad(data.nesting) + (failed ? "not ok " : "ok ") + number + " - " +
          tapEscape(data.name) + directive);
      tapYaml(data.nesting, data);
      if (data.nesting === 0) {
        tap.duration += data.details && data.details.duration_ms !== undefined ? data.details.duration_ms : 0;
      }
    };
    const tapTrailer = () => {
      if (tap.trailer || !tap.header) return;
      tap.trailer = true;
      out("1.." + tap.topLevel);
      out("# tests " + tap.counts.tests);
      out("# suites " + tap.counts.suites);
      out("# pass " + tap.counts.pass);
      out("# fail " + tap.counts.fail);
      out("# cancelled " + tap.counts.cancelled);
      out("# skipped " + tap.counts.skipped);
      out("# todo " + tap.counts.todo);
      out("# duration_ms " + (Date.now() - tapStartedAt));
    };
    const tapStartedAt = Date.now();
    subscribers.push((type, data) => {
      if (!tapEnabled) return;
      if (type === "test:start") { tapHeader(); out(pad(data.nesting) + "# Subtest: " + tapEscape(data.name)); }
      else if (type === "test:pass" || type === "test:fail") tapResult(type, data);
      else if (type === "test:diagnostic") { tapHeader(); out(pad(data.nesting || 0) + "# " + tapEscape(data.message)); }
    });
    try { G.process.on("exit", () => { if (tapEnabled) tapTrailer(); }); } catch (e) {}

    // ------------------------------------------------------- event shapes ----
    // node identifies a test instance by a number that is stable across its own
    // start/complete/pass/fail events, and reports where it was *declared*, not
    // where it ran. Both are read straight back by the corpus
    // (test-runner-test-id, test-runner-filetest-location).
    let nextTestId = 1;
    const idOf = (node) => (node.__id !== undefined ? node.__id : (node.__id = nextTestId++));
    const nestingOf = (node) => {
      let depth = -1;
      for (let p = node; p && p !== root; p = p.parent) depth++;
      return depth < 0 ? 0 : depth;
    };
    // The registration call site: the first stack frame carrying a real path.
    // The runner itself is a builtin blob, so its own frames have none.
    // (a function *declaration*: makeNode is defined above this block and
    // `root` is built during that pass, so this has to be hoisted.)
    function captureLoc() {
      try {
        const stack = new Error().stack;
        if (typeof stack !== "string") return {};
        const lines = stack.split("\n");
        for (let i = 0; i < lines.length; i++) {
          const m = /((?:\/|[A-Za-z]:\\)[^\s()]+?):(\d+):(\d+)/.exec(lines[i]);
          if (m) return { file: m[1], line: +m[2], column: +m[3] };
        }
      } catch (e) {}
      return {};
    }
    // node lib/internal/test_runner/tag_filter.js: a test's tag set is the
    // union of its ancestors' and its own, lowercased, deduplicated, in
    // parent-first declaration order (test-runner-tags-inheritance).
    let warnedAboutTags = false;
    const tagsOf = (node) => {
      const chain = [];
      for (let p = node; p && p !== root; p = p.parent) chain.unshift(p);
      const seen = Object.create(null);
      const list = [];
      for (const n of chain) {
        const own = Array.isArray(n.opts.tags) ? n.opts.tags : [];
        if (own.length !== 0 && !warnedAboutTags) {
          warnedAboutTags = true;
          try {
            G.process.emitWarning(
              "Test tags is an experimental feature and might change at any time",
              "ExperimentalWarning");
          } catch (e) {}
        }
        for (const tag of own) {
          if (typeof tag !== "string" || tag.length === 0) continue;
          const lower = tag.toLowerCase();
          if (seen[lower] === undefined) { seen[lower] = true; list.push(lower); }
        }
      }
      return Object.freeze(list);
    };
    // node applies at most ONE directive and skip outranks todo, both when they
    // come from the options bag and when they come from the context methods
    // (test-runner-todo-skip-tests asserts `todo === undefined` for a test that
    // asked for both).
    const directiveOf = (node, ctx) => {
      const optSkip = node.opts.skip;
      const ctxSkip = ctx && ctx.__skipped ? (ctx.__skipReason === undefined ? true : ctx.__skipReason) : undefined;
      const skip = (optSkip !== undefined && optSkip !== false) ? (optSkip === true ? true : optSkip) : ctxSkip;
      if (skip !== undefined) return { skip };
      const optTodo = node.opts.todo;
      const ctxTodo = ctx && ctx.__todo !== undefined ? ctx.__todo : undefined;
      let todo = (optTodo !== undefined && optTodo !== false) ? (optTodo === true ? true : optTodo) : ctxTodo;
      // node lib/internal/test_runner/test.js:
      //   this.isTodo = (todo !== undefined && todo !== false) || this.parent?.isTodo
      // — todo is INHERITED, so every test inside `describe.todo(...)` reports
      // (and is counted) as todo even though it declared nothing
      // (test-runner-exit-code's todo_exit_code.js fixture: "should inherit
      // todo"; test-runner-todo-suite-hook-failure's two children).
      if (todo === undefined) {
        for (let p = node.parent; p; p = p.parent) {
          const t = p.opts && p.opts.todo;
          if (t !== undefined && t !== false) { todo = true; break; }
        }
      }
      if (todo !== undefined) return { todo };
      return {};
    };
    const baseEvent = (node) => {
      const loc = node.__loc || {};
      const e = {
        name: node.name,
        nesting: nestingOf(node),
        testId: idOf(node),
        parentId: node.parent && node.parent !== root ? idOf(node.parent) : undefined,
        tags: tagsOf(node),
      };
      if (loc.file !== undefined) { e.file = loc.file; e.line = loc.line; e.column = loc.column; }
      return e;
    };
    const emitEnqueue = (node) => {
      const e = baseEvent(node);
      e.type = node.kind === "suite" ? "suite" : "test";
      emit("test:enqueue", e);
    };
    const emitDequeue = (node) => {
      const e = baseEvent(node);
      e.type = node.kind === "suite" ? "suite" : "test";
      emit("test:dequeue", e);
      emit("test:start", baseEvent(node));
    };
    // `expectFailure` inverts the verdict: node reports a test that was expected
    // to fail AND failed as a pass carrying the directive (test-runner-xfail).
    const emitResult = (node, error, ctx, startedAt) => {
      const parent = node.parent || root;
      parent.__childNumber = (parent.__childNumber || 0) + 1;
      const e = baseEvent(node);
      e.testNumber = parent.__childNumber;
      const directive = directiveOf(node, ctx);
      if (directive.skip !== undefined) e.skip = directive.skip;
      if (directive.todo !== undefined) e.todo = directive.todo;
      const expectation = node.opts.expectFailure;
      const expected = expectation !== undefined && expectation !== false;
      let passed = error === undefined;
      let reported = error;
      if (expected) {
        if (!passed) { passed = true; reported = undefined; e.expectFailure = expectation === true ? true : expectation; }
        else {
          reported = new Error("test was expected to fail but passed");
          reported.code = "ERR_TEST_FAILURE";
          reported.failureType = "expectedFailure";
          passed = false;
          e.expectFailure = expectation === true ? true : expectation;
        }
      }
      e.details = {
        duration_ms: startedAt === undefined ? 0 : Date.now() - startedAt,
        type: node.kind === "suite" ? "suite" : "test",
      };
      if (!passed) e.details.error = reported;
      emit(passed ? "test:pass" : "test:fail", e);
      emit("test:complete", e);
      return passed;
    };

    // Each entry remembers the node that OWNS the hook: node reports that node's
    // context from getTestContext() while the hook runs, so a suite's beforeEach
    // sees the suite and a test's own beforeEach sees that test — not the child
    // being set up (test-runner-get-test-context).
    const eachHooks = (node, which) => {
      const list = [];
      for (let p = node.parent; p; p = p.parent) {
        list.unshift(...p.hooks[which].map((fn) => ({ fn, owner: p })));
      }
      return list;
    };

    // node lib/internal/test_runner/test.js Test#run invokes the body through
    // `runInAsyncScope(fn, ctx, ctx)`, i.e. the context is BOTH the argument and
    // the `this` value; a hook goes through the same path with the context its
    // caller passed (a suite's own for before/after, the child test's for
    // beforeEach/afterEach). A `function () { this.name }` hook — which the
    // no-isolation fixtures use throughout — therefore reads a real context in
    // node and threw "undefined is not an object" here while `this` stayed
    // unbound.
    const callWithCtx = (fn, ctx, extra) => (extra === undefined
      ? fn.call(ctx, ctx)
      : fn.call(ctx, ctx, extra));

    // node applies runOnce to before/after hooks (createHook), so a hook that
    // already ran — see rootBeforeNow() — is not run a second time when the
    // enclosing test reaches its before phase. beforeEach/afterEach are per-test
    // and must NOT be latched.
    const onceHook = (fn) => {
      let started = false;
      let result;
      const wrapper = (arg) => {
        if (started) return result;
        started = true;
        try { result = Promise.resolve(callWithCtx(fn, arg)); }
        catch (e) { result = Promise.reject(e); }
        return result;
      };
      wrapper.__raw = fn;
      return wrapper;
    };
    const eachHook = (fn) => {
      const wrapper = (arg) => callWithCtx(fn, arg);
      wrapper.__raw = fn;
      return wrapper;
    };
    const addHook = (node, which, fn) => {
      const wrapped = (which === "before" || which === "after") ? onceHook(fn) : eachHook(fn);
      node.hooks[which].push(wrapped);
      return wrapped;
    };
    // node lib/internal/test_runner/harness.js createTestTree() stamps the root
    // test's startTime the moment the tree exists, so Test#createHook's "test has
    // already started, run the hook immediately" branch ALWAYS fires for a
    // top-level before(): the body runs synchronously at the before() call rather
    // than being queued for the first test. run({ isolation: 'none' }) makes that
    // observable — each file's root before() has to be seen before that file's
    // suite bodies (test-runner-no-isolation pins the exact interleaving).
    const rootBeforeNow = (hook) => {
      const ctx = root.__ctx || (root.__ctx = makeSuiteContext(root));
      const previous = state.current;
      state.current = ctx;
      let promise;
      try { promise = hook(ctx); } finally { state.current = previous; }
      // The latched promise is re-awaited by the root's before phase, which is
      // where a failure is reported; swallow it here only so the rejection is not
      // counted as unhandled in the meantime.
      if (promise && typeof promise.catch === "function") promise.catch(() => {});
    };

    const runHooks = async (list, arg) => {
      for (const hook of list) await hook(arg);
    };

    const runEachHooks = async (list, arg) => {
      for (const item of list) {
        const owner = item.owner;
        const ctx = owner.kind === "suite"
          ? (owner.__ctx || (owner.__ctx = makeSuiteContext(owner)))
          : owner.__testCtx;
        if (ctx === undefined) await item.fn(arg);
        else await withContext(ctx, () => item.fn(arg));
      }
    };

    // node lib/internal/test_runner/test.js TestContext#workerId: the id the
    // runner put in the environment when it spawned this file, or undefined
    // when the file was not run by the test runner at all.
    const workerIdOf = () => {
      try {
        const id = Number(G.process.env.NODE_TEST_WORKER_ID);
        return id || undefined;
      } catch (e) { return undefined; }
    };

    const makeContext = (node) => {
      const controller = typeof G.AbortController === "function" ? new G.AbortController() : null;
      const context = {
        name: node.name,
        fullName: fullNameOf(node),
        filePath: entryFile(),
        get workerId() { return workerIdOf(); },
        tags: tagsOf(node),
        signal: controller ? controller.signal : undefined,
        __controller: controller,
        __skipped: false,
        __plan: null,
        __planWait: undefined,
        __planPending: null,
        __planWaiting: false,
        __deferredError: undefined,
        __assertions: 0,
        __subs: [],
        diagnostic: (message) => { out("# " + message); emit("test:diagnostic", { nesting: nestingOf(node), message, level: "info" }); },
        skip: (message) => {
          context.__skipped = true;
          context.__skipReason = message === undefined ? true : message;
          if (message) out("# SKIP " + message);
        },
        todo: (message) => {
          context.__todo = message === undefined ? true : message;
          if (message) out("# TODO " + message);
        },
        runOnly: () => {},
        // node lib/internal/test_runner/test.js TestContext#plan validates both
        // arguments before recording the count and the `wait` policy.
        plan: (count, options) => {
          if (typeof count !== "number") throw argTypeError("count", "number", count);
          if (options !== undefined) {
            if (options === null || typeof options !== "object") {
              throw argTypeError("options", "object", options);
            }
            const wait = options.wait;
            if (wait !== undefined && typeof wait !== "boolean" && typeof wait !== "number") {
              const e = new TypeError('The "options.wait" property must be one of type boolean or' +
                                      " number. Received type " + typeof wait +
                                      " (" + String(wait) + ")");
              e.code = "ERR_INVALID_ARG_TYPE";
              throw e;
            }
            context.__planWait = wait;
          }
          context.__plan = count;
        },
        mock: makeMock(),
        before: (fn) => { addHook(node, "before", fn); },
        after: (fn) => { addHook(node, "after", fn); },
        beforeEach: (fn) => { addHook(node, "beforeEach", fn); },
        afterEach: (fn) => { addHook(node, "afterEach", fn); },
        // node lib/internal/test_runner/test.js TestContext#waitFor: poll
        // `condition` every `interval` ms until it stops throwing, giving up
        // after `timeout` ms with the last error.
        waitFor: (condition, options) => {
          if (typeof condition !== "function") throw argTypeError("condition", "function", condition);
          if (options !== undefined) {
            if (options === null || typeof options !== "object") throw argTypeError("options", "object", options);
            if (options.interval !== undefined && typeof options.interval !== "number") {
              throw propTypeError("options.interval", "number", options.interval);
            }
            if (options.timeout !== undefined && typeof options.timeout !== "number") {
              throw propTypeError("options.timeout", "number", options.timeout);
            }
          }
          const interval = options && options.interval !== undefined ? options.interval : 50;
          const timeout = options && options.timeout !== undefined ? options.timeout : 1000;
          const deadline = Date.now() + timeout;
          return (async () => {
            let lastError;
            for (;;) {
              try { return await condition(); }
              catch (e) { lastError = e; }
              if (Date.now() >= deadline) {
                const e = new Error("waitFor() timed out after " + timeout + "ms");
                e.code = "ERR_TEST_FAILURE";
                e.cause = lastError;
                throw e;
              }
              await new Promise((r) => G.setTimeout(r, interval));
            }
          })();
        },
      };
      // node lib/internal/test_runner/test.js TestPlan: a plan is only "met" at
      // exactly the planned count. Without a `wait` policy the check is
      // immediate; with one it hands back a promise the test awaits until the
      // count arrives, or until `wait` ms elapse (`wait: true` = no deadline).
      const planActual = () => context.__assertions + context.__subs.length;
      const planCheck = () => {
        if (context.__plan === null) return undefined;
        if (planActual() === context.__plan) {
          const pending = context.__planPending;
          if (pending) {
            context.__planPending = null;
            if (pending.timer !== null) G.clearTimeout(pending.timer);
            pending.resolve();
          }
          return undefined;
        }
        if (context.__planWait === undefined || context.__planWait === false) {
          const e = new Error("plan expected " + context.__plan + " assertions but received " + planActual());
          e.code = "ERR_TEST_FAILURE";
          throw e;
        }
        if (context.__planPending === null) {
          let resolve, reject;
          const promise = new Promise((res, rej) => { resolve = res; reject = rej; });
          const pending = { promise, resolve, reject, timer: null };
          context.__planPending = pending;
          if (context.__planWait !== true) {
            pending.timer = G.setTimeout(() => {
              context.__planPending = null;
              const e = new Error("plan timed out after " + context.__planWait + "ms with " +
                                  planActual() + " assertions when expecting " + context.__plan);
              e.code = "ERR_TEST_FAILURE";
              reject(e);
            }, context.__planWait);
          }
        }
        return context.__planPending.promise;
      };
      // Node re-checks the plan on every counted assertion, but only while one
      // is actually being awaited (outside that window check() would throw).
      const planCount = () => { if (context.__planPending !== null) planCheck(); };
      // An assertion that throws after the body returned still belongs to this
      // test: node attributes it through uncaughtException, we hand it to the
      // await below. The count is bumped first, exactly as node does, so a
      // failing assertion can still be the one that completes the plan.
      const counted = (call) => function (...a) {
        context.__assertions += 1;
        planCount();
        try {
          return call.apply(this, a);
        } catch (error) {
          if (context.__planWaiting && context.__deferredError === undefined) {
            context.__deferredError = error;
          }
          throw error;
        }
      };
      // t.assert mirrors node:assert, counting calls so t.plan() can check them.
      const assert = assertMod();
      const bound = {};
      if (assert) {
        const uncopiedKeys = new Set(["AssertionError", "strict", "Assert", "options"]);
        for (const key of Object.keys(assert)) {
          if (uncopiedKeys.has(key)) continue;
          const value = assert[key];
          if (typeof value !== "function") continue;
          bound[key] = counted(function (...a) { return value.apply(assert, a); });
        }
        bound.ok = counted(function (...a) {
          try {
            return assert.ok.apply(assert, a);
          } catch (error) {
            throw describeFalsyOk(error);
          }
        });
      }
      const snapshot = getSnapshotRuntime();
      if (snapshot) {
        bound.snapshot = counted(function (...a) { return snapshot.assertion.apply(context, a); });
        bound.fileSnapshot = counted(function (...a) { return snapshot.fileAssertion.apply(context, a); });
      }
      // Custom assertions registered through node:test's `assert.register` are
      // bound to the TestContext (`this` === t) and count towards t.plan().
      for (const [name, fn] of customAssertions) {
        bound[name] = counted(function (...a) { return fn.apply(context, a); });
      }
      context.assert = bound;
      context.__planCheck = planCheck;
      // node accepts the plan as a test option too (`test(name, { plan: 1 }, fn)`),
      // which is the same TestPlan with no `wait` policy.
      if (node.opts && typeof node.opts.plan === "number") context.__plan = node.opts.plan;
      // Subtests: registered while the parent body runs, awaited by the parent.
      const sub = (...a) => {
        const promise = register(node, ...a);
        context.__subs.push(promise);
        planCount();
        return promise;
      };
      context.test = sub;
      context.it = sub;
      context.describe = (...a) => registerSuite(node, ...a);
      return context;
    };

    // node's SuiteContext: describe()/suite() bodies and a suite's before/after
    // hooks receive this, NOT the raw node (test-runner-test-fullname reads
    // fullName/passed/attempt/diagnostic off it).
    const makeSuiteContext = (node) => {
      const controller = typeof G.AbortController === "function" ? new G.AbortController() : null;
      return {
        name: node.name,
        get fullName() { return fullNameOf(node); },
        filePath: entryFile(),
        tags: tagsOf(node),
        signal: controller ? controller.signal : undefined,
        passed: true,
        attempt: 0,
        diagnostic: (message) => out("# " + message),
        before: (fn) => { addHook(node, "before", fn); },
        after: (fn) => { addHook(node, "after", fn); },
        beforeEach: (fn) => { addHook(node, "beforeEach", fn); },
        afterEach: (fn) => { addHook(node, "afterEach", fn); },
      };
    };

    // node lib/internal/test_runner/test.js Test#filter(): with only-filtering
    // active, a test that is neither `only` nor an ancestor of one is dropped
    // whenever its parent saw an `only` sibling. filteredRun() marks it passed
    // but installs `report = noop`, so a filtered test produces NO event and is
    // absent from every counter — which is why the fixture that marks its inner
    // test `{ only: true }` yields exactly four test:pass events under
    // isolation:'none' (test-runner-no-isolation).
    const filteredByOnly = (node) => {
      if (!state.filterByOnly) return false;
      if (node.opts.only === true || node.hasOnlyTests) return false;
      const parent = node.parent;
      if (!parent) return false;
      return !!(parent.runOnlySubtests || parent.hasOnlyTests || node.opts.only === false);
    };
    // Called where the node is CONSTRUCTED, as node does.
    const markOnly = (node) => {
      if (node.opts.only !== true) return;
      const parent = node.parent;
      if (!parent) return;
      parent.runOnlySubtests = true;
      for (let t = parent; t !== null && !t.hasOnlyTests; t = t.parent) t.hasOnlyTests = true;
    };

    const runNode = async (node) => {
      if (filteredByOnly(node)) return;
      if (node.kind === "suite") return runSuite(node);
      const opts = node.opts;
      // node only lets `skip` prevent a body from running; a `todo` test still
      // runs and its result is reported under the todo directive
      // (test-runner-todo-skip-tests relies on t.skip() inside a todo test).
      if (opts.skip || node.fn === null) {
        emitDequeue(node);
        emitResult(node, undefined, undefined, Date.now());
        skipped(node.name);
        return;
      }
      emitDequeue(node);
      const startedAt = Date.now();
      const context = makeContext(node);
      const previousContext = state.current;
      state.current = context;
      try {
        node.__testCtx = context;
        await runEachHooks(eachHooks(node, "beforeEach"), context);
        const fn = node.fn;
        let result;
        if (fn.length >= 2) {
          result = await new Promise((resolve, reject) => {
            let settled = false;
            const done = (error) => {
              if (settled) return;
              settled = true;
              if (error) reject(error); else resolve();
            };
            try {
              const maybe = callWithCtx(fn, context, done);
              if (maybe && typeof maybe.then === "function") maybe.then(() => done(), done);
            } catch (e) { done(e); }
          });
        } else {
          result = callWithCtx(fn, context);
          if (result && typeof result.then === "function") await result;
        }
        for (const promise of context.__subs) await promise;
        const planPromise = context.__planCheck();
        if (planPromise) {
          context.__planWaiting = true;
          try { await planPromise; } finally { context.__planWaiting = false; }
        }
        // A late assertion may have satisfied the plan and thrown in the same
        // call; the plan is met but the test still failed.
        if (context.__deferredError !== undefined) throw context.__deferredError;
        if (context.__controller) context.__controller.abort();
        await runEachHooks(eachHooks(node, "afterEach"), context);
        state.current = previousContext;
        if (emitResult(node, undefined, context, startedAt)) {
          if (context.__skipped) skipped(node.name); else ok(node.name);
        } else {
          // expectFailure was set and the body did NOT throw.
          fail(node.name, new Error("test was expected to fail but passed"), true);
        }
      } catch (error) {
        state.current = previousContext;
        if (context.__controller) { try { context.__controller.abort(); } catch (e) {} }
        try { await runEachHooks(eachHooks(node, "afterEach"), context); } catch (e) {}
        const directive = directiveOf(node, context);
        if (emitResult(node, error, context, startedAt)) ok(node.name);
        else if (directive.todo !== undefined) skipped(node.name);
        else fail(node.name, error, true);
      }
    };

    const runSuite = async (node) => {
      const startedAt = Date.now();
      emitDequeue(node);
      try {
        if (node.opts.skip) {
          emitResult(node, undefined, undefined, startedAt);
          skipped(node.name);
          return;
        }
        const ctx = node.__ctx || (node.__ctx = makeSuiteContext(node));
        // node runs a suite's before hook lazily, from inside the FIRST child's
        // run() (`await this.parent.runHook('before', ...)`), so a throwing before
        // hook fails every child individually — the children are still reported,
        // and a `todo` suite's children still come out under the todo directive
        // (test-runner-todo-suite-hook-failure expects `# todo 2`, `# fail 0`).
        // Dropping them silently lost both.
        let hookError;
        try { await withContext(ctx, () => runHooks(node.hooks.before, ctx)); }
        catch (e) { hookError = e; }
        for (const child of node.children) {
          if (hookError === undefined) { await runNode(child); continue; }
          if (filteredByOnly(child)) continue;
          emitDequeue(child);
          const directive = directiveOf(child, undefined);
          if (emitResult(child, hookError, undefined, Date.now())) ok(child.name);
          else if (directive.todo !== undefined) skipped(child.name);
          else fail(child.name, hookError, true);
        }
        if (hookError !== undefined) throw hookError;
        await withContext(ctx, () => runHooks(node.hooks.after, ctx));
        emit("test:plan", { nesting: nestingOf(node) + 1, count: node.children.length });
        emitResult(node, undefined, undefined, startedAt);
      } catch (error) {
        emitResult(node, error, undefined, startedAt);
        fail(node.name, error, true);
      }
    };

    // Normalize node's flexible ([name][, options][, fn]) signature.
    const normalize = (args) => {
      let name; let opts; let fn;
      for (const arg of args) {
        if (typeof arg === "function") { if (fn === undefined) fn = arg; }
        else if (typeof arg === "string") { if (name === undefined) name = arg; }
        else if (arg && typeof arg === "object") { if (opts === undefined) opts = arg; }
      }
      if (name === undefined && fn && fn.name) name = fn.name;
      validateTags(opts);
      validateTestOptions(opts);
      return { name, opts: opts || {}, fn };
    };

    // node runs the ROOT test's before/after hooks around the whole file; these
    // were registered (top-level `before()`/`after()`) and never executed.
    // Chained lazily so the whole synchronous registration pass is visible.
    const runRootAfter = () => {
      const ctx = root.__ctx || (root.__ctx = makeSuiteContext(root));
      state.chain = state.chain
        .then(() => withContext(ctx, () => runHooks(root.hooks.after, ctx)))
        .catch((e) => fail("<root>", e));
    };
    const scheduleRootAfter = () => {
      if (state.rootAfterScheduled) return;
      // run({ isolation: 'none' }) evaluates the files one at a time and drains
      // the chain between them, so latching the root after() onto the chain as
      // soon as the FIRST file registers a test would run it before the second
      // file had even been loaded. node's root test spans the whole run, so the
      // runner takes ownership of the root after() phase instead
      // (test-runner-no-isolation expects both files' after() last).
      if (state.deferRootAfter) return;
      state.rootAfterScheduled = true;
      G.queueMicrotask(runRootAfter);
    };
    // node lib/internal/test_runner/runner.js runFiles() for isolation:'none'
    // extends harness.bootstrapPromise with a deferred it only resolves AFTER the
    // last file has been imported, and every subtest awaits that in
    // startSubtestAfterBootstrap(). So under isolation:'none' the whole tree is
    // COLLECTED first and only then run — file two's suites are registered before
    // file one's tests execute. state.barrier is that deferred.
    // PORT-SOURCE: node lib/internal/assert/utils.js getErrMessage() — when
    // `assert.ok(expr)` fails with no explicit message, node reads the source of
    // the call site back off the stack and puts the ORIGINAL EXPRESSION in the
    // message ("The expression evaluated to a falsy value:\n\n  t.assert.ok(1 ===
    // 2)\n"). mbun's native assert only produces the first line, so
    // test-runner-assert's "t.assert.ok correctly parses the stacktrace" (which
    // matches /t\.assert\.ok\(1 === 2\)/ against the thrown error) never saw the
    // expression. node parses the call with acorn; here the call site is always a
    // direct call of this wrapper, so a balanced-paren scan of the one source
    // line the frame names is enough — and every failure path falls back to the
    // untouched error.
    const describeFalsyOk = (error) => {
      try {
        const prefix = "The expression evaluated to a falsy value";
        if (!error || typeof error.message !== "string") return error;
        if (error.message.indexOf(prefix) !== 0) return error;
        // Already expanded (or a user-supplied message): leave it alone.
        if (error.message.indexOf("\n") !== -1) return error;
        let site = null;
        for (const frame of String(error.stack || "").split("\n")) {
          const m = /(\/[^\s()]*):(\d+):(\d+)\)?\s*$/.exec(frame);
          if (m === null) continue;
          site = { file: m[1], line: +m[2], column: +m[3] };
          break;
        }
        if (site === null) return error;
        const fs = M["fs"] || M["node:fs"];
        if (!fs || typeof fs.readFileSync !== "function") return error;
        const lines = String(fs.readFileSync(site.file, "utf8")).split("\n");
        const text = lines[site.line - 1];
        if (typeof text !== "string") return error;
        // Every `ok(` on the line is a candidate; the one the frame points at is
        // the one whose parenthesis sits nearest the reported column.
        const re = /\bok\s*\(/g;
        let best = -1;
        let bestDistance = Infinity;
        let match;
        while ((match = re.exec(text)) !== null) {
          const open = match.index + match[0].length - 1;
          const distance = Math.abs(open - (site.column - 1));
          if (distance < bestDistance) { bestDistance = distance; best = match.index; }
        }
        if (best === -1) return error;
        let start = best;
        while (start > 0 && /[A-Za-z0-9_$.\]['"]/.test(text[start - 1])) start--;
        let depth = 0;
        let end = -1;
        for (let i = best; i < text.length; i++) {
          const ch = text[i];
          if (ch === "(") depth++;
          else if (ch === ")") { depth--; if (depth === 0) { end = i + 1; break; } }
        }
        if (end === -1) return error;
        error.message = prefix + ":\n\n  " + text.slice(start, end) + "\n";
        return error;
      } catch (e) { return error; }
    };

    // PORT-SOURCE: node lib/internal/util.js setupCoverageHooks() — the branch
    // taken when `--experimental-test-coverage` is on but the build has no
    // inspector. node's harness calls setupCoverage() as the root test starts,
    // and with no inspector it collects nothing and says so on stderr instead of
    // failing. mbun has no inspector at all, so this is the only branch that can
    // ever be taken; it was missing entirely, which left the flag completely
    // silent (test-runner-coverage's "handles the inspector not being
    // available" asserts the warning is on stderr, that no report is printed and
    // that the exit status is still 0).
    let coverageWarned = false;
    const warnCoverageUnavailable = () => {
      if (coverageWarned) return;
      coverageWarned = true;
      try {
        const p = G.process;
        if (!p) return;
        if (p.features && p.features.inspector) return;
        const wanted = (list) => Array.isArray(list) && list.some(
          (a) => typeof a === "string" &&
                 (a === "--experimental-test-coverage" || a === "--test-coverage"));
        if (!wanted(p.execArgv) && !wanted(p.argv)) return;
        if (typeof p.emitWarning === "function") {
          p.emitWarning("The inspector is disabled, coverage could not be collected", "Warning");
        }
      } catch (e) {}
    };

    const ensureRootStarted = () => {
      if (state.rootStarted) return;
      state.rootStarted = true;
      warnCoverageUnavailable();
      const ctx = root.__ctx || (root.__ctx = makeSuiteContext(root));
      state.chain = state.chain
        .then(() => state.barrier)
        .then(() => withContext(ctx, () => runHooks(root.hooks.before, ctx)))
        .catch((e) => fail("<root>", e));
    };
    const schedule = (node) => {
      ensureRootStarted();
      state.topLevel += 1;
      state.chain = state.chain.then(() => runNode(node)).catch((e) => fail(node.name, e));
      scheduleRootAfter();
      return state.chain;
    };

    // register(parent, ...) — a subtest of a running test (parent is its node)
    // runs immediately; a test declared inside describe() is collected; a
    // top-level test is queued on the sequential chain.
    function register(parent, ...args) {
      const { name, opts, fn } = normalize(args);
      if (parent) {
        const node = makeNode("test", name, opts, fn, parent);
        emitEnqueue(node);
        return runNode(node);
      }
      if (state.collecting) {
        const node = makeNode("test", name, opts, fn, state.collecting);
        state.collecting.children.push(node);
        markOnly(node);
        emitEnqueue(node);
        return Promise.resolve();
      }
      const node = makeNode("test", name, opts, fn, root);
      markOnly(node);
      emitEnqueue(node);
      return schedule(node);
    }

    function registerSuite(parent, ...args) {
      const { name, opts, fn } = normalize(args);
      const node = makeNode("suite", name, opts, fn, parent || state.collecting || root);
      if (node.parent !== root) node.parent.children.push(node);
      markOnly(node);
      emitEnqueue(node);
      const previous = state.collecting;
      state.collecting = node;
      try {
        // A skipped suite's body never runs — node reports the suite itself as
        // skipped and never touches its children (test-runner-todo-skip-tests
        // registers a mustNotCall() body on one).
        if (typeof fn === "function" && !opts.skip) {
          const ctx = node.__ctx || (node.__ctx = makeSuiteContext(node));
          const produced = fn.call(ctx, ctx);
          if (produced && typeof produced.then === "function") {
            // An async suite body cannot be collected synchronously; node awaits
            // it before running the children, so mirror that.
            state.collecting = previous;
            return schedule(node);
          }
        }
      } catch (error) {
        state.collecting = previous;
        fail(name || "<suite>", error);
        return Promise.resolve();
      }
      state.collecting = previous;
      if (node.parent === root) return schedule(node);
      return Promise.resolve();
    }

    const standalone = {
      test: (...a) => register(null, ...a),
      it: (...a) => register(null, ...a),
      describe: (...a) => registerSuite(null, ...a),
      suite: (...a) => registerSuite(null, ...a),
      before: (fn) => {
        const target = state.collecting || root;
        const hook = addHook(target, "before", fn);
        if (target === root) rootBeforeNow(hook);
      },
      after: (fn) => { addHook(state.collecting || root, "after", fn); },
      beforeEach: (fn) => { addHook(state.collecting || root, "beforeEach", fn); },
      afterEach: (fn) => { addHook(state.collecting || root, "afterEach", fn); },
      skip: (...a) => { const n = normalize(a); return register(null, n.name, Object.assign({}, n.opts, { skip: true }), n.fn); },
      todo: (...a) => { const n = normalize(a); return register(null, n.name, Object.assign({}, n.opts, { todo: true }), n.fn); },
      only: (...a) => register(null, ...a),
      mock: makeMock(),
      run: () => state.chain,
      assert: testAssert,
      snapshot: snapshotApi,
      getTestContext,
    };

    // Dispatch: the bun:test harness owns node:test under `mbun test`; a direct
    // script run has no harness and lands on the standalone runner above.
    const useDelegate = () => !!G.__mbunBT;
    const pick = (name) => function (...a) {
      const target = useDelegate() ? delegate[name] : standalone[name];
      return typeof target === "function" ? target.apply(null, a) : undefined;
    };
    const nodeTest = function (...a) {
      return useDelegate() ? delegate.apply(null, a) : standalone.test(...a);
    };
    for (const key of ["test", "it", "describe", "suite", "before", "after",
                       "beforeEach", "afterEach", "skip", "todo", "only", "run"]) {
      nodeTest[key] = pick(key);
    }
    // node lib/test.js exports the SAME function object under its aliases:
    // `test.test === test`, `test.it === test`, `test.describe === test.suite`
    // (test-runner-aliases asserts identity, not just equivalent behaviour).
    nodeTest.test = nodeTest;
    nodeTest.it = nodeTest;
    // node lib/internal/test_runner/harness.js runInParentContext() hangs
    // expectFailure/skip/todo/only off BOTH `test` and `suite`, so
    // `describe.todo(...)` declares a TODO SUITE. Only the test-side variants
    // existed here: `describe.todo` was undefined, so the fixture
    // test-runner-exit-code runs (todo_exit_code.js) threw "not a function" at
    // top level and lost its last suite entirely.
    for (const keyword of ["expectFailure", "skip", "todo", "only"]) {
      nodeTest.describe[keyword] = function (...a) {
        const n = normalize(a);
        const opts = Object.assign({}, n.opts);
        opts[keyword] = true;
        return nodeTest.describe(n.name, opts, n.fn);
      };
    }
    nodeTest.suite = nodeTest.describe;
    Object.defineProperty(nodeTest, "mock", {
      configurable: true,
      get() { return useDelegate() ? delegate.mock : standalone.mock; },
    });
    // node:test's own exports, independent of which runner is in charge.
    nodeTest.assert = testAssert;
    nodeTest.snapshot = snapshotApi;
    nodeTest.getTestContext = () => (useDelegate() ? undefined : getTestContext());
    nodeTest.default = nodeTest;
    M["test"] = nodeTest;
    M["node:test"] = nodeTest;
    M["test/reporters"] = { tap: () => {}, spec: function spec() {}, dot: () => {}, junit: () => {}, lcov: () => {} };
    M["node:test/reporters"] = M["test/reporters"];

    // The seam the :node_test_run partition (run(), TestsStream, reporters,
    // the child-process reporter) attaches to. Non-enumerable so node's
    // leaked-globals check in test/common does not see it.
    Object.defineProperty(G, "__mbunNodeTest", {
      configurable: true, writable: true, enumerable: false,
      value: {
        subscribe(fn) { subscribers.push(fn); return () => {
          const i = subscribers.indexOf(fn);
          if (i >= 0) subscribers.splice(i, 1);
        }; },
        setTap(enabled) { tapEnabled = !!enabled; },
        setOwnExitCode(enabled) { ownExitCode = !!enabled; },
        // isolation:'none' only — see scheduleRootAfter().
        setDeferRootAfter(enabled) { state.deferRootAfter = !!enabled; },
        // isolation:'none' only — see ensureRootStarted() and filteredByOnly().
        setLoadBarrier(promise) { state.barrier = promise; },
        setFilterByOnly(enabled) { state.filterByOnly = !!enabled; },
        // How many top-level tests/suites this process has queued. The runner
        // compares it across a file's evaluation to tell "this file declared no
        // tests" (node counts root.subtests for the same purpose) — it cannot use
        // the event stream for that any more, because with the load barrier up no
        // result has been emitted yet.
        topLevelCount() { return state.topLevel; },
        // Let the runner append its own step (a placeholder FileTest) to the
        // sequential chain, so it lands in the same position node's inline
        // root.createSubtest() would have put it.
        appendChain(fn) {
          ensureRootStarted();
          state.chain = state.chain.then(fn).catch(() => {});
          return state.chain;
        },
        flushRootAfter() {
          if (state.rootAfterScheduled) return state.chain;
          state.rootAfterScheduled = true;
          runRootAfter();
          return state.chain;
        },
        nextId() { return nextTestId++; },
        // Everything the standalone runner has queued, including the root
        // after() hooks — run() waits on this for isolation:'none'.
        drain() { return state.chain; },
        counts() { return { failures: state.failures, total: state.index }; },
        emit,
      },
    });
  } catch (e) {}
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
