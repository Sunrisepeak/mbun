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
    const out = (line) => { try { G.console.log(line); } catch (e) {} };

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
            impl = function (...a) { return (calls.length === index ? once : prev).apply(this, a); };
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
      return {
        fn: mockFn,
        method,
        getter: (object, name, implementation, options) => method(object, name, implementation, options),
        setter: (object, name, implementation, options) => method(object, name, implementation, options),
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

    const fail = (name, error) => {
      state.failures += 1;
      out("not ok " + ++state.index + " - " + name);
      const message = error && error.message !== undefined
        ? (error.name || "Error") + ": " + error.message
        : String(error);
      out("  " + message);
      if (error && error.stack) out(String(error.stack).split("\n").map((l) => "  " + l).join("\n"));
      try { G.process.exitCode = 1; } catch (e) {}
    };
    const ok = (name) => { out("ok " + ++state.index + " - " + name); };
    const skipped = (name) => { out("ok " + ++state.index + " - " + name + " # SKIP"); };

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
        signal: controller ? controller.signal : undefined,
        __controller: controller,
        __skipped: false,
        __plan: null,
        __assertions: 0,
        __subs: [],
        diagnostic: (message) => out("# " + message),
        skip: (message) => { context.__skipped = true; if (message) out("# SKIP " + message); },
        todo: (message) => { if (message) out("# TODO " + message); },
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
      if (opts.skip || opts.todo || node.fn === null) { skipped(node.name); return; }
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
        if (context.__skipped) skipped(node.name); else ok(node.name);
        state.current = previousContext;
      } catch (error) {
        state.current = previousContext;
        if (context.__controller) { try { context.__controller.abort(); } catch (e) {} }
        try { await runEachHooks(eachHooks(node, "afterEach"), context); } catch (e) {}
        fail(node.name, error);
      }
    };

    const runSuite = async (node) => {
      try {
        if (node.opts.skip || node.opts.todo) { skipped(node.name); return; }
        const ctx = node.__ctx || (node.__ctx = makeSuiteContext(node));
        await withContext(ctx, () => runHooks(node.hooks.before, ctx));
        for (const child of node.children) await runNode(child);
        await withContext(ctx, () => runHooks(node.hooks.after, ctx));
      } catch (error) {
        fail(node.name, error);
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
        return runNode(node);
      }
      if (state.collecting) {
        const node = makeNode("test", name, opts, fn, state.collecting);
        state.collecting.children.push(node);
        return Promise.resolve();
      }
      const node = makeNode("test", name, opts, fn, root);
      return schedule(node);
    }

    function registerSuite(parent, ...args) {
      const { name, opts, fn } = normalize(args);
      const node = makeNode("suite", name, opts, fn, parent || state.collecting || root);
      if (node.parent !== root) node.parent.children.push(node);
      const previous = state.collecting;
      state.collecting = node;
      try {
        if (typeof fn === "function") {
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
  } catch (e) {}
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
