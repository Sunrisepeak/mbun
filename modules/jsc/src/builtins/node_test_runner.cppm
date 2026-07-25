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
    const state = { collecting: null, chain: Promise.resolve(), failures: 0, index: 0, scheduled: false };

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

    const eachHooks = (node, which) => {
      const list = [];
      for (let p = node.parent; p; p = p.parent) list.unshift(...p.hooks[which]);
      return list;
    };

    const runHooks = async (list, arg) => {
      for (const hook of list) await hook(arg);
    };

    const makeContext = (node) => {
      const controller = typeof G.AbortController === "function" ? new G.AbortController() : null;
      const context = {
        name: node.name,
        fullName: node.name,
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

    const runNode = async (node) => {
      if (node.kind === "suite") return runSuite(node);
      const opts = node.opts;
      if (opts.skip || opts.todo || node.fn === null) { skipped(node.name); return; }
      const context = makeContext(node);
      try {
        await runHooks(eachHooks(node, "beforeEach"), context);
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
        await runHooks(eachHooks(node, "afterEach"), context);
        if (context.__skipped) skipped(node.name); else ok(node.name);
      } catch (error) {
        if (context.__controller) { try { context.__controller.abort(); } catch (e) {} }
        try { await runHooks(eachHooks(node, "afterEach"), context); } catch (e) {}
        fail(node.name, error);
      }
    };

    const runSuite = async (node) => {
      try {
        if (node.opts.skip || node.opts.todo) { skipped(node.name); return; }
        await runHooks(node.hooks.before, node);
        for (const child of node.children) await runNode(child);
        await runHooks(node.hooks.after, node);
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

    const schedule = (node) => {
      state.chain = state.chain.then(() => runNode(node)).catch((e) => fail(node.name, e));
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
          const produced = fn.call(node, node);
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
    nodeTest.default = nodeTest;
    M["test"] = nodeTest;
    M["node:test"] = nodeTest;
    M["test/reporters"] = { tap: () => {}, spec: function spec() {}, dot: () => {}, junit: () => {}, lcov: () => {} };
    M["node:test/reporters"] = M["test/reporters"];
  } catch (e) {}
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
