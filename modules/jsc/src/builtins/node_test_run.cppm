// node:test's `run()` — the programmatic runner — plus the TestsStream it
// returns, node:test/reporters, and the reporter a child process installs.
//
// PORT-SOURCE: node lib/internal/test_runner/tests_stream.js (TestsStream, in
// full), lib/internal/test_runner/runner.js (run()'s option validation, the
// per-file child process, `isolation`, `concurrency`, `shard`, the summary),
// and lib/internal/test_runner/reporter/{tap,dot,spec}.js.
//
// Why this is a separate partition from :node_test_runner — that one owns the
// in-process test tree and reports through the event surface installed as
// `__mbunNodeTest`; this one owns everything that consumes those events. The
// two directions of the same protocol: a *parent* subscribes to a child's
// events over IPC, a *child* forwards its own there. A single process can be
// both (a `run()` inside a test file), which is why neither side may assume it
// is the only reporter.
//
// The child protocol is mbun's own: node ships v8-serialized events on the
// child's stdout (NODE_TEST_CONTEXT=child-v8), which forces the parent to
// de-interleave test events from the test's own console output. Both ends here
// are mbun, so the events go over the IPC channel and stdout stays purely the
// test's own output — observationally identical for every consumer, since the
// corpus asserts on the parent's stream, never on the wire format.
// NODE_TEST_CONTEXT is still set to node's value, because the corpus branches
// on it to tell "I am the runner" from "I am the test" inside one file.
//
// NOTE: appended AFTER :node_test_runner, so `__mbunNodeTest` exists. Own IIFE,
// re-binds G, must never throw at top level.
export module mbun.jsc.js_builtins:node_test_run;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kNodeTestRunJS = R"JS(
(function () {
  const G = globalThis;
  try {
    const M = G.__mbunNativeModules;
    if (!M) return;
    const internals = G.__mbunNodeTest;
    if (!internals) return;
    const nodeTest = M["test"];
    if (typeof nodeTest !== "function") return;

    const mod = (name) => M[name] || M["node:" + name] || {};
    const streamMod = () => mod("stream");
    const pathMod = () => mod("path");
    const fsMod = () => mod("fs");

    const typeError = (name, expected, value) => {
      const e = new TypeError('The "' + name + '" argument must be ' + expected +
                              ". Received " + (value === null ? "null" : typeof value));
      e.code = "ERR_INVALID_ARG_TYPE";
      return e;
    };
    const valueError = (name, value, reason) => {
      const e = new TypeError("The value of \"" + name + "\" is out of range." +
                              (reason ? " " + reason : "") + " Received " + String(value));
      e.code = "ERR_OUT_OF_RANGE";
      return e;
    };

    // ------------------------------------------------------- TestsStream ----
    // PORT-SOURCE: lib/internal/test_runner/tests_stream.js. Every message is
    // BOTH an EventEmitter event (`stream.on('test:pass')`) and an object-mode
    // chunk (`for await (const {type, data} of stream)`); the corpus uses both
    // on the same stream, so neither may consume the other.
    const Readable = streamMod().Readable;
    if (typeof Readable !== "function") return;

    class TestsStream extends Readable {
      constructor() {
        super({ objectMode: true, highWaterMark: Number.MAX_SAFE_INTEGER });
        this.__buffer = [];
        this.__canPush = true;
      }
      _read() {
        this.__canPush = true;
        while (this.__buffer.length > 0) {
          if (!this.__tryPush(this.__buffer.shift())) return;
        }
      }
      __tryPush(message) {
        if (this.__canPush) this.__canPush = this.push(message);
        else this.__buffer.push(message);
        return this.__canPush;
      }
      emitMessage(type, data) {
        this.emit(type, data);
        this.__tryPush({ type, data });
      }
      finish() { this.__tryPush(null); }
    }

    // ---------------------------------------------------------- reporters ----
    // PORT-SOURCE: lib/internal/test_runner/reporter/{dot,tap,spec}.js.
    const reporters = {};

    reporters.dot = async function* dot(source) {
      let count = 0;
      for await (const { type } of source) {
        if (type === "test:pass") yield ".";
        if (type === "test:fail") yield "X";
        if ((type === "test:pass" || type === "test:fail") && ++count === 76) {
          yield "\n";
          count = 0;
        }
      }
      yield "\n";
    };

    const tapEscape = (s) => String(s).replace(/\\/g, "\\\\").replace(/#/g, "\\#");
    const indent = (n) => "    ".repeat(n);

    reporters.tap = async function* tap(source) {
      yield "TAP version 13\n";
      const counts = { tests: 0, suites: 0, pass: 0, fail: 0, cancelled: 0, skipped: 0, todo: 0 };
      let duration = 0;
      let topLevel = 0;
      for await (const { type, data } of source) {
        if (type === "test:enqueue" && data.nesting === 0 && data.type !== "suite") {
          // node prints the subtest banner when the test starts.
        } else if (type === "test:start") {
          yield indent(data.nesting) + "# Subtest: " + tapEscape(data.name) + "\n";
        } else if (type === "test:pass" || type === "test:fail") {
          const failed = type === "test:fail";
          if (data.details && data.details.type === "suite") counts.suites++;
          else counts.tests++;
          if (data.skip !== undefined) counts.skipped++;
          else if (data.todo !== undefined) counts.todo++;
          else if (failed) counts.fail++;
          else counts.pass++;
          if (data.nesting === 0) topLevel++;
          const directive = data.skip !== undefined
            ? " # SKIP" + (typeof data.skip === "string" ? " " + tapEscape(data.skip) : "")
            : data.todo !== undefined
              ? " # TODO" + (typeof data.todo === "string" ? " " + tapEscape(data.todo) : "")
              : "";
          const number = data.testNumber === undefined ? 1 : data.testNumber;
          yield indent(data.nesting) + (failed ? "not ok " : "ok ") + number + " - " +
                tapEscape(data.name) + directive + "\n";
          const ms = data.details && data.details.duration_ms !== undefined ? data.details.duration_ms : 0;
          if (data.nesting === 0) duration += ms;
          yield indent(data.nesting) + "  ---\n" + indent(data.nesting) + "  duration_ms: " + ms + "\n" +
                indent(data.nesting) + "  ...\n";
        } else if (type === "test:diagnostic") {
          yield indent(data.nesting) + "# " + tapEscape(data.message) + "\n";
        }
      }
      yield "1.." + topLevel + "\n";
      yield "# tests " + counts.tests + "\n";
      yield "# suites " + counts.suites + "\n";
      yield "# pass " + counts.pass + "\n";
      yield "# fail " + counts.fail + "\n";
      yield "# cancelled " + counts.cancelled + "\n";
      yield "# skipped " + counts.skipped + "\n";
      yield "# todo " + counts.todo + "\n";
      yield "# duration_ms " + duration + "\n";
    };

    // node's spec reporter is a Transform subclass that is ALSO usable as
    // `spec`, `spec()` and `new spec()` (test-runner-run pipes all three).
    const Transform = streamMod().Transform;
    let SpecReporter = null;
    if (typeof Transform === "function") {
      SpecReporter = function spec(options) {
        if (!(this instanceof SpecReporter)) return new SpecReporter(options);
        Transform.call(this, { writableObjectMode: true });
        this.__counts = { tests: 0, pass: 0, fail: 0, skipped: 0, todo: 0, suites: 0 };
        this.__duration = 0;
      };
      SpecReporter.prototype = Object.create(Transform.prototype);
      SpecReporter.prototype.constructor = SpecReporter;
      SpecReporter.prototype._transform = function (chunk, _enc, cb) {
        const type = chunk.type;
        const data = chunk.data || {};
        let text = "";
        if (type === "test:pass" || type === "test:fail") {
          const failed = type === "test:fail";
          if (data.details && data.details.type === "suite") this.__counts.suites++;
          else this.__counts.tests++;
          if (data.skip !== undefined) this.__counts.skipped++;
          else if (data.todo !== undefined) this.__counts.todo++;
          else if (failed) this.__counts.fail++;
          else this.__counts.pass++;
          const ms = data.details && data.details.duration_ms !== undefined ? data.details.duration_ms : 0;
          if (data.nesting === 0) this.__duration += ms;
          text = "  ".repeat(data.nesting) + (failed ? "✖ " : "✔ ") + data.name +
                 " (" + ms + "ms)\n";
          if (failed && data.details && data.details.error) {
            text += "  ".repeat(data.nesting + 1) + String(data.details.error.message || data.details.error) + "\n";
          }
        } else if (type === "test:diagnostic") {
          text = "  ".repeat(data.nesting || 0) + data.message + "\n";
        } else if (type === "test:stdout" || type === "test:stderr") {
          text = data.message;
        }
        cb(null, text);
      };
      SpecReporter.prototype._flush = function (cb) {
        const c = this.__counts;
        cb(null, "\nℹ tests " + c.tests + "\nℹ suites " + c.suites +
                 "\nℹ pass " + c.pass + "\nℹ fail " + c.fail +
                 "\nℹ cancelled 0\nℹ skipped " + c.skipped +
                 "\nℹ todo " + c.todo + "\nℹ duration_ms " + this.__duration + "\n");
      };
      reporters.spec = SpecReporter;
    }

    reporters.junit = async function* junit(source) {
      yield '<?xml version="1.0" encoding="utf-8"?>\n<testsuites>\n';
      for await (const { type, data } of source) {
        if (type === "test:pass") yield '  <testcase name="' + data.name + '"/>\n';
        else if (type === "test:fail") yield '  <testcase name="' + data.name + '"><failure/></testcase>\n';
      }
      yield "</testsuites>\n";
    };

    reporters.lcov = async function* lcov(source) {
      for await (const { type, data } of source) {
        if (type !== "test:coverage") continue;
        const files = data && data.summary && data.summary.files ? data.summary.files : [];
        for (const file of files) yield "SF:" + file.path + "\nend_of_record\n";
      }
    };

    M["test/reporters"] = reporters;
    M["node:test/reporters"] = reporters;

    // -------------------------------------------------- child-side reporter --
    // A file run by run({isolation:'process'}) reports over the IPC channel and
    // stops writing TAP: its stdout belongs to the test, and the parent turns
    // whatever lands there into `test:stdout` events.
    const isChild = (() => {
      try {
        return !!(G.process && G.process.env && G.process.env.NODE_TEST_CONTEXT &&
                  typeof G.process.send === "function");
      } catch (e) { return false; }
    })();
    if (isChild) {
      internals.setTap(false);
      internals.subscribe((type, data) => {
        try { G.process.send({ __mbunTestEvent: { type, data: serializable(data) } }); } catch (e) {}
      });
    }

    // An Error does not survive structured cloning with its own fields, and the
    // corpus reads `details.error.message` / `.code` / `.failureType` on the
    // parent side — so flatten it into a plain object the parent re-inflates.
    const serializable = (data) => {
      if (data === null || typeof data !== "object") return data;
      const copy = {};
      for (const key of Object.keys(data)) {
        const value = data[key];
        if (key === "details" && value && typeof value === "object") {
          const details = {};
          for (const k of Object.keys(value)) {
            if (k === "error") details.error = flattenError(value.error);
            else details[k] = value[k];
          }
          copy.details = details;
        } else if (value instanceof Error) copy[key] = flattenError(value);
        else copy[key] = value;
      }
      return copy;
    };
    const flattenError = (error) => {
      if (error === null || error === undefined) return error;
      if (typeof error !== "object") return { message: String(error) };
      return {
        __mbunError: true,
        name: error.name, message: error.message, stack: error.stack,
        code: error.code, failureType: error.failureType,
        cause: error.cause instanceof Error ? flattenError(error.cause) : error.cause,
      };
    };
    const inflateError = (value) => {
      if (!value || typeof value !== "object" || value.__mbunError !== true) return value;
      const e = new Error(value.message);
      if (value.name !== undefined) e.name = value.name;
      if (value.stack !== undefined) e.stack = value.stack;
      if (value.code !== undefined) e.code = value.code;
      if (value.failureType !== undefined) e.failureType = value.failureType;
      if (value.cause !== undefined) e.cause = inflateError(value.cause);
      return e;
    };

    // ---------------------------------------------------------------- run ----
    // PORT-SOURCE: lib/internal/test_runner/runner.js `run()`. The validation
    // order matters: test-runner-option-validation asserts the exact code of
    // the first rejected option.
    const validate = (options) => {
      if (options === null || typeof options !== "object") throw typeError("options", "an object", options);
      const {
        files, concurrency, timeout, signal, testNamePatterns, testSkipPatterns,
        shard, watch, setup, only, globalSetupPath, execArgv, argv, cwd, isolation,
      } = options;
      if (files !== undefined && !Array.isArray(files)) throw typeError("options.files", "an array", files);
      if (watch !== undefined && typeof watch !== "boolean") throw typeError("options.watch", "a boolean", watch);
      if (only !== undefined && typeof only !== "boolean") throw typeError("options.only", "a boolean", only);
      if (concurrency !== undefined && typeof concurrency !== "boolean") {
        if (typeof concurrency !== "number") throw typeError("options.concurrency", "a number", concurrency);
        if (!(concurrency >= 1)) throw valueError("options.concurrency", concurrency, "It must be >= 1.");
      }
      if (timeout !== undefined && timeout !== Infinity) {
        if (typeof timeout !== "number") throw typeError("options.timeout", "a number", timeout);
        if (!(timeout > 0)) throw valueError("options.timeout", timeout, "It must be a positive number.");
      }
      if (signal !== undefined && (signal === null || typeof signal !== "object" || !("aborted" in signal))) {
        throw typeError("options.signal", "an AbortSignal", signal);
      }
      if (shard !== undefined) {
        if (shard === null || typeof shard !== "object") throw typeError("options.shard", "an object", shard);
        if (typeof shard.total !== "number") throw typeError("options.shard.total", "a number", shard.total);
        if (typeof shard.index !== "number") throw typeError("options.shard.index", "a number", shard.index);
        if (!(shard.total >= 1)) throw valueError("options.shard.total", shard.total, "It must be >= 1.");
        if (!(shard.index >= 1 && shard.index <= shard.total)) {
          throw valueError("options.shard.index", shard.index, "It must be >= 1 and <= options.shard.total.");
        }
        if (watch) {
          const e = new TypeError("options.shard is not supported with watch mode");
          e.code = "ERR_INVALID_ARG_VALUE";
          throw e;
        }
      }
      if (setup !== undefined && typeof setup !== "function") throw typeError("options.setup", "a function", setup);
      if (globalSetupPath !== undefined && typeof globalSetupPath !== "string") {
        throw typeError("options.globalSetupPath", "a string", globalSetupPath);
      }
      if (cwd !== undefined && typeof cwd !== "string") throw typeError("options.cwd", "a string", cwd);
      if (isolation !== undefined && isolation !== "process" && isolation !== "none") {
        const e = new TypeError("The argument 'options.isolation' must be one of: 'none', 'process'. Received " +
                                JSON.stringify(isolation));
        e.code = "ERR_INVALID_ARG_VALUE";
        throw e;
      }
      if (options.testTagFilters !== undefined) {
        const list = Array.isArray(options.testTagFilters) ? options.testTagFilters : [options.testTagFilters];
        for (const item of list) {
          if (typeof item !== "string") throw typeError("options.testTagFilters", "an array of strings", item);
          if (item.length === 0) {
            const e = new TypeError("The argument 'options.testTagFilters' must be a non-empty string. Received ''");
            e.code = "ERR_INVALID_ARG_VALUE";
            throw e;
          }
        }
      }
      for (const [name, value] of [["options.execArgv", execArgv], ["options.argv", argv]]) {
        if (value === undefined) continue;
        if (!Array.isArray(value)) throw typeError(name, "an array", value);
        for (const item of value) if (typeof item !== "string") throw typeError(name, "an array of strings", item);
      }
      for (const [name, value] of [["options.testNamePatterns", testNamePatterns],
                                   ["options.testSkipPatterns", testSkipPatterns]]) {
        if (value === undefined) continue;
        const list = Array.isArray(value) ? value : [value];
        for (const item of list) {
          if (typeof item !== "string" && !(item instanceof RegExp)) {
            throw typeError(name, "a string or RegExp", item);
          }
        }
      }
    };

    // node's default file discovery: **/*.test.{js,mjs,cjs} plus every file
    // under a `test` directory, rooted at cwd (runner.js createTestFileList).
    const TEST_FILE = /((^|[\\/])(test|tests)[\\/].*|[.-](test|spec)|^test)\.(c|m)?js$/;
    const discover = (cwd) => {
      const fs = fsMod();
      const path = pathMod();
      const found = [];
      const walk = (dir, depth) => {
        if (depth > 12) return;
        let entries;
        try { entries = fs.readdirSync(dir, { withFileTypes: true }); } catch (e) { return; }
        for (const entry of entries) {
          const name = entry.name;
          if (name === "node_modules" || name.charCodeAt(0) === 46) continue;
          const full = path.join(dir, name);
          if (entry.isDirectory()) walk(full, depth + 1);
          else if (TEST_FILE.test(path.relative(cwd, full))) found.push(full);
        }
      };
      walk(cwd, 0);
      found.sort();
      return found;
    };

    const toRegExp = (pattern) => {
      if (pattern instanceof RegExp) return pattern;
      // node parses a `/…/flags`-shaped string as a RegExp literal and treats
      // anything else as a substring (lib/internal/test_runner/utils.js).
      const m = /^\/(.*)\/([a-z]*)$/.exec(String(pattern));
      try { return m ? new RegExp(m[1], m[2]) : new RegExp(String(pattern)); }
      catch (e) { return new RegExp(String(pattern).replace(/[.*+?^${}()|[\]\\]/g, "\\$&")); }
    };

    const run = (options) => {
      options = options === undefined ? {} : options;
      validate(options);
      const path = pathMod();
      const cwd = options.cwd === undefined ? G.process.cwd() : options.cwd;
      const isolation = options.isolation === undefined ? "process" : options.isolation;
      const stream = new TestsStream();

      let given = options.files;
      if (given === undefined) given = discover(cwd);
      if (options.shard !== undefined) {
        given = given.filter((_f, i) => (i % options.shard.total) === (options.shard.index - 1));
      }
      // `given` is what the caller wrote (node names the file test with it);
      // `files` is what gets executed.
      const files = given.map((f) => (path.isAbsolute(f) ? f : path.resolve(cwd, f)));

      // node lib/internal/test_runner/tag_filter.js: an include filter keeps a
      // test whose flattened tag set matches any filter (`db:*` is a prefix
      // wildcard); everything untagged is dropped.
      const tagFilters = options.testTagFilters === undefined ? null
        : (Array.isArray(options.testTagFilters) ? options.testTagFilters : [options.testTagFilters])
            .map((t) => String(t).toLowerCase());
      const matchesTags = (tags) => {
        if (tagFilters === null) return true;
        if (!Array.isArray(tags)) return false;
        for (const filter of tagFilters) {
          for (const tag of tags) {
            if (tag === filter) return true;
            if (filter.endsWith(":*") && tag.startsWith(filter.slice(0, -1))) return true;
          }
        }
        return false;
      };
      const namePatterns = options.testNamePatterns === undefined ? null
        : (Array.isArray(options.testNamePatterns) ? options.testNamePatterns : [options.testNamePatterns]).map(toRegExp);
      const skipPatterns = options.testSkipPatterns === undefined ? null
        : (Array.isArray(options.testSkipPatterns) ? options.testSkipPatterns : [options.testSkipPatterns]).map(toRegExp);

      const counts = { tests: 0, suites: 0, passed: 0, failed: 0, cancelled: 0, skipped: 0, todo: 0 };
      const startedAt = Date.now();
      const track = (type, data) => {
        if (type === "test:pass" || type === "test:fail") {
          if (data.details && data.details.type === "suite") counts.suites++; else counts.tests++;
          if (data.skip !== undefined) counts.skipped++;
          else if (data.todo !== undefined) counts.todo++;
          else if (type === "test:fail") counts.failed++;
          else counts.passed++;
        }
      };
      const forward = (type, data) => {
        if ((type === "test:pass" || type === "test:fail" || type === "test:start" ||
             type === "test:enqueue" || type === "test:dequeue" || type === "test:complete") &&
            namePatterns !== null && !namePatterns.some((re) => re.test(data.name))) {
          return;
        }
        if ((type === "test:pass" || type === "test:fail") && skipPatterns !== null &&
            skipPatterns.some((re) => re.test(data.name))) {
          return;
        }
        if (tagFilters !== null &&
            (type === "test:pass" || type === "test:fail" || type === "test:complete") &&
            !matchesTags(data.tags)) {
          return;
        }
        track(type, data);
        stream.emitMessage(type, data);
      };

      const finish = () => {
        stream.emitMessage("test:summary", {
          success: counts.failed === 0,
          counts,
          duration_ms: Date.now() - startedAt,
          file: undefined,
        });
        stream.finish();
      };

      const abortSignal = options.signal;
      const aborted = () => abortSignal !== undefined && abortSignal.aborted;

      (async () => {
        // Yield first: run() returns the stream and the caller attaches its
        // listeners synchronously, so nothing may be emitted before that.
        await Promise.resolve();
        try {
          if (typeof options.setup === "function") await options.setup(stream);
          if (isolation === "none") await runInProcess(files, given, forward, aborted);
          else await runIsolated(files, given, forward, options, cwd, aborted);
          if (options.watch) stream.emitMessage("test:watch:drained", {});
        } catch (error) {
          stream.emitMessage("test:fail", {
            name: "run()", nesting: 0, testNumber: 1, tags: [],
            details: { duration_ms: 0, type: "test", error },
          });
          counts.failed++;
        }
        finish();
      })();

      return stream;
    };

    // isolation: 'none' — the files share this process and this runner, so the
    // events come straight off the in-process reporting surface.
    const runInProcess = async (files, given, forward, aborted) => {
      const createRequire = mod("module").createRequire;
      const unsubscribe = internals.subscribe(forward);
      // The evaluated files' failures belong to the returned stream; they must
      // not set the exit status of the process that called run().
      internals.setOwnExitCode(false);
      try {
        for (let i = 0; i < files.length; i++) {
          if (aborted()) break;
          const file = files[i];
          const name = given[i];
          const startedAt = Date.now();
          // node reports the file itself as a test, so a file that cannot even
          // be loaded still produces an enqueue and a failure.
          forward("test:enqueue", fileEvent(name, file, internals.nextId()));
          try {
            const req = typeof createRequire === "function" ? createRequire(file)
              : (typeof G.require === "function" ? G.require : null);
            if (req === null) throw new Error("no require available for isolation:'none'");
            req(file);
          } catch (error) {
            const e = fileEvent(name, file, internals.nextId());
            e.testNumber = i + 1;
            e.details = { duration_ms: Date.now() - startedAt, type: "test", error };
            forward("test:fail", e);
          }
          await drainFully();
        }
        await drainFully();
      } finally { unsubscribe(); internals.setOwnExitCode(true); }
    };

    // node's FileTest: named by the path the caller gave (which may be
    // relative), located at the top of the file it stands for
    // (test-runner-filetest-location asserts name/file/line/column together).
    const fileEvent = (name, file, testId) => ({
      name, nesting: 0, testId, tags: [], type: "test",
      file, line: 1, column: 1,
    });

    // The runner queues work onto a promise chain that keeps growing while the
    // file is being evaluated, so one await is not enough to know it is idle.
    const drainFully = async () => {
      for (let i = 0; i < 100; i++) {
        const chain = internals.drain();
        await chain;
        if (internals.drain() === chain) return;
      }
    };

    // isolation: 'process' (node's default) — one child per file.
    const runIsolated = async (files, given, forward, options, cwd, aborted) => {
      const { spawn } = mod("child_process");
      if (typeof spawn !== "function") throw new Error("node:child_process is unavailable");
      const limit = options.concurrency === true ? files.length
        : (typeof options.concurrency === "number" ? options.concurrency
           : Math.max(1, (mod("os").availableParallelism ? mod("os").availableParallelism() : 2) - 1));

      for (let i = 0; i < files.length; i++) {
        forward("test:enqueue", fileEvent(given[i], files[i], internals.nextId()));
      }

      let cursor = 0;
      const worker = async () => {
        for (;;) {
          if (aborted()) return;
          const index = cursor++;
          if (index >= files.length) return;
          await runOneFile(spawn, files[index], given[index], index + 1, forward, options, cwd);
        }
      };
      const workers = [];
      for (let i = 0; i < Math.min(limit, Math.max(files.length, 1)); i++) workers.push(worker());
      await Promise.all(workers);
    };

    const runOneFile = (spawn, file, given, ordinal, forward, options, cwd) => new Promise((resolve) => {
      const env = Object.assign({}, G.process.env, { NODE_TEST_CONTEXT: "child-v8" });
      if (options.only) env.NODE_TEST_ONLY = "1";
      const args = [];
      if (Array.isArray(options.execArgv)) args.push(...options.execArgv);
      args.push(file);
      if (Array.isArray(options.argv)) args.push(...options.argv);

      forward("test:dequeue", fileEvent(given, file, internals.nextId()));

      let child;
      try {
        child = spawn(G.process.execPath, args, {
          cwd: options.cwd === undefined ? cwd : options.cwd,
          env,
          stdio: ["pipe", "pipe", "pipe", "ipc"],
        });
      } catch (error) {
        const e = fileEvent(given, file, internals.nextId());
        e.testNumber = ordinal;
        e.details = { duration_ms: 0, type: "test", error };
        forward("test:fail", e);
        resolve();
        return;
      }

      const startedAt = Date.now();
      let sawResult = false;
      let timer = null;
      if (typeof options.timeout === "number" && options.timeout !== Infinity) {
        timer = G.setTimeout(() => { try { child.kill(); } catch (e) {} }, options.timeout);
      }

      child.on("message", (message) => {
        if (!message || typeof message !== "object" || !message.__mbunTestEvent) return;
        const { type, data } = message.__mbunTestEvent;
        if (data && data.details && data.details.error) data.details.error = inflateError(data.details.error);
        if (data && data.file === undefined) data.file = file;
        if (type === "test:pass" || type === "test:fail") sawResult = true;
        forward(type, data);
      });
      if (child.stdout) child.stdout.on("data", (d) => forward("test:stdout", { file, message: String(d) }));
      if (child.stderr) child.stderr.on("data", (d) => forward("test:stderr", { file, message: String(d) }));
      child.on("error", (error) => {
        const e = fileEvent(given, file, internals.nextId());
        e.testNumber = ordinal;
        e.details = { duration_ms: Date.now() - startedAt, type: "test", error };
        forward("test:fail", e);
        sawResult = true;
      });
      child.on("close", (code, signal) => {
        if (timer !== null) G.clearTimeout(timer);
        // node reports the FILE itself as a failing test when the process could
        // not produce results (missing file, syntax error, non-zero exit with
        // nothing reported) — that is the only way those become visible.
        if ((code !== 0 || signal) && !sawResult) {
          const error = new Error("test failed");
          error.code = "ERR_TEST_FAILURE";
          error.failureType = "testCodeFailure";
          error.exitCode = code;
          const e = fileEvent(given, file, internals.nextId());
          e.testNumber = ordinal;
          e.details = { duration_ms: Date.now() - startedAt, type: "test", error };
          forward("test:fail", e);
        }
        resolve();
      });
    });

    // Under `mbun test` the bun:test harness owns node:test and its own run();
    // only a plain script run (no harness) gets the programmatic runner.
    const previousRun = nodeTest.run;
    Object.defineProperty(nodeTest, "run", {
      writable: true, configurable: true, enumerable: true,
      value: function (...a) {
        if (G.__mbunBT && typeof previousRun === "function") return previousRun.apply(this, a);
        return run(...a);
      },
    });
  } catch (e) {}
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
