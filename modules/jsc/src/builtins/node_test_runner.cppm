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
      hooks: { before: [], after: [], beforeEach: [], afterEach: [] },
    });

    const root = makeNode("suite", "<root>", {}, null, null);
    const state = { collecting: null, chain: Promise.resolve(), failures: 0, index: 0, scheduled: false,
                    current: undefined };

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
    const testAssert = {
      register(name, fn) {
        if (typeof name !== "string") throw argTypeError("name", "string", name);
        if (typeof fn !== "function") throw argTypeError("fn", "function", fn);
        customAssertions.set(name, fn);
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
      if (data.details && data.details.type === "suite") tap.counts.suites += 1;
      else tap.counts.tests += 1;
      if (data.skip !== undefined) tap.counts.skipped += 1;
      else if (data.todo !== undefined) tap.counts.todo += 1;
      else if (failed) {
        if (data.details && data.details.error && data.details.error.failureType === "cancelledByParent") {
          tap.counts.cancelled += 1;
        } else tap.counts.fail += 1;
      } else tap.counts.pass += 1;
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
      const todo = (optTodo !== undefined && optTodo !== false) ? (optTodo === true ? true : optTodo) : ctxTodo;
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

    const makeContext = (node) => {
      const controller = typeof G.AbortController === "function" ? new G.AbortController() : null;
      const context = {
        name: node.name,
        fullName: fullNameOf(node),
        filePath: entryFile(),
        tags: tagsOf(node),
        signal: controller ? controller.signal : undefined,
        __controller: controller,
        __skipped: false,
        __plan: null,
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
        plan: (count) => { context.__plan = count; },
        mock: makeMock(),
        before: (fn) => node.hooks.before.push(fn),
        after: (fn) => node.hooks.after.push(fn),
        beforeEach: (fn) => node.hooks.beforeEach.push(fn),
        afterEach: (fn) => node.hooks.afterEach.push(fn),
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
      // t.assert mirrors node:assert, counting calls so t.plan() can check them.
      const assert = assertMod();
      const bound = {};
      if (assert) {
        for (const key of Object.keys(assert)) {
          const value = assert[key];
          if (typeof value !== "function") continue;
          bound[key] = function (...a) { context.__assertions += 1; return value.apply(assert, a); };
        }
        bound.ok = function (...a) { context.__assertions += 1; return assert.ok.apply(assert, a); };
      }
      // Custom assertions registered through node:test's `assert.register` are
      // bound to the TestContext (`this` === t) and count towards t.plan().
      for (const [name, fn] of customAssertions) {
        bound[name] = function (...a) { context.__assertions += 1; return fn.apply(context, a); };
      }
      context.assert = bound;
      // Subtests: registered while the parent body runs, awaited by the parent.
      const sub = (...a) => {
        const promise = register(node, ...a);
        context.__subs.push(promise);
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
        before: (fn) => node.hooks.before.push(fn),
        after: (fn) => node.hooks.after.push(fn),
        beforeEach: (fn) => node.hooks.beforeEach.push(fn),
        afterEach: (fn) => node.hooks.afterEach.push(fn),
      };
    };

    const runNode = async (node) => {
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
              const maybe = fn(context, done);
              if (maybe && typeof maybe.then === "function") maybe.then(() => done(), done);
            } catch (e) { done(e); }
          });
        } else {
          result = fn(context);
          if (result && typeof result.then === "function") await result;
        }
        for (const promise of context.__subs) await promise;
        if (context.__plan !== null && context.__plan !== context.__assertions + context.__subs.length) {
          throw new Error("plan expected " + context.__plan + " assertions, got " +
                          (context.__assertions + context.__subs.length));
        }
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
        await withContext(ctx, () => runHooks(node.hooks.before, ctx));
        for (const child of node.children) await runNode(child);
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
      return { name, opts: opts || {}, fn };
    };

    // node runs the ROOT test's before/after hooks around the whole file; these
    // were registered (top-level `before()`/`after()`) and never executed.
    // Chained lazily so the whole synchronous registration pass is visible.
    const scheduleRootAfter = () => {
      if (state.rootAfterScheduled) return;
      state.rootAfterScheduled = true;
      G.queueMicrotask(() => {
        const ctx = root.__ctx || (root.__ctx = makeSuiteContext(root));
        state.chain = state.chain
          .then(() => withContext(ctx, () => runHooks(root.hooks.after, ctx)))
          .catch((e) => fail("<root>", e));
      });
    };
    const schedule = (node) => {
      if (!state.rootStarted) {
        state.rootStarted = true;
        const ctx = root.__ctx || (root.__ctx = makeSuiteContext(root));
        state.chain = state.chain
          .then(() => withContext(ctx, () => runHooks(root.hooks.before, ctx)))
          .catch((e) => fail("<root>", e));
      }
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
        emitEnqueue(node);
        return Promise.resolve();
      }
      const node = makeNode("test", name, opts, fn, root);
      emitEnqueue(node);
      return schedule(node);
    }

    function registerSuite(parent, ...args) {
      const { name, opts, fn } = normalize(args);
      const node = makeNode("suite", name, opts, fn, parent || state.collecting || root);
      if (node.parent !== root) node.parent.children.push(node);
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
      before: (fn) => (state.collecting || root).hooks.before.push(fn),
      after: (fn) => (state.collecting || root).hooks.after.push(fn),
      beforeEach: (fn) => (state.collecting || root).hooks.beforeEach.push(fn),
      afterEach: (fn) => (state.collecting || root).hooks.afterEach.push(fn),
      skip: (...a) => { const n = normalize(a); return register(null, n.name, Object.assign({}, n.opts, { skip: true }), n.fn); },
      todo: (...a) => { const n = normalize(a); return register(null, n.name, Object.assign({}, n.opts, { todo: true }), n.fn); },
      only: (...a) => register(null, ...a),
      mock: makeMock(),
      run: () => state.chain,
      assert: testAssert,
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
    nodeTest.suite = nodeTest.describe;
    Object.defineProperty(nodeTest, "mock", {
      configurable: true,
      get() { return useDelegate() ? delegate.mock : standalone.mock; },
    });
    // node:test's own exports, independent of which runner is in charge.
    nodeTest.assert = testAssert;
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
