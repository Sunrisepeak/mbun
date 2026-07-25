// node:cluster — a translation of node lib/cluster.js + lib/internal/cluster/*.
//
// bootstrap.cppm ships a stub whose `fork()` returns a bare EventEmitter, so
// every one of node's ~72 cluster tests failed at the first `worker.send` /
// `worker.kill` / `'online'` event. This partition replaces it with the real
// thing, translated from the upstream sources that live in this repo:
//
//   compat/node/lib/cluster.js                     → the primary/child switch
//   compat/node/lib/internal/cluster/worker.js     → the shared Worker class
//   compat/node/lib/internal/cluster/utils.js      → sendHelper / internal
//   compat/node/lib/internal/cluster/primary.js    → cluster in the primary
//   compat/node/lib/internal/cluster/child.js      → cluster inside a worker
//
// (bun's port — compat/bun/src/js/internal/cluster/* — was used as the
// cross-check for what a non-V8 runtime has to substitute.)
//
// Two substitutions for node internals mbun does not have:
//
//   * node's `sendHelper` marks a message `cmd: 'NODE_CLUSTER'` and relies on
//     lib/internal/child_process.js routing every `NODE_`-prefixed cmd to the
//     'internalMessage' event instead of 'message'. mbun's IPC channel
//     (process_web.cppm `isInternalIpc`) already does exactly that, so the
//     upstream transport is used unchanged.
//   * node keeps a worker's IPC channel from pinning the event loop when the
//     only listener is cluster's own `process.once('disconnect')` (V8 side:
//     `channel[kIgnoreOneDisconnectEvent]`; bun spells it
//     `channelIgnoreOneDisconnectEventListener`). mbun's pin predicate is the
//     `globalThis.__mbunIpcPin` seam, so the child overrides it to discount
//     that one listener — otherwise every worker with no work left would hang
//     instead of exiting.
//
// Handle sharing (SCHED_RR's `newconn` handoff and dgram's SharedHandle) rides
// on the NODE_HANDLE/SCM_RIGHTS protocol added to the IPC channel in
// process_web.cppm; the worker-side bind delegation lives in js_net.cppm
// (`Server._listenInCluster`) and node_net.cppm (dgram `bind`), which is where
// node puts it too (lib/net.js `listenInCluster`, lib/dgram.js `bind`).
//
// NOTE: appended AFTER the master builtins IIFE, so this is a self-contained
// IIFE that re-binds G = globalThis. Top level must never throw.
export module mbun.jsc.js_builtins:node_cluster;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kNodeClusterJS = R"JS(
(function () {
  const G = globalThis;
  try {
    const M = G.__mbunNativeModules;
    if (!M) return;
    const proc = G.process;
    if (!proc || !proc.env) return;

    const events = M["events"];
    const EventEmitter = (events && (events.EventEmitter || events)) || null;
    if (typeof EventEmitter !== "function") return;

    // node lib/cluster.js: the role is frozen at first require, and
    // pre_execution.js has already deleted NODE_UNIQUE_ID by then in a worker,
    // so the decision has to be captured here — before the delete below.
    const uniqueId = proc.env.NODE_UNIQUE_ID;
    const isChild = uniqueId != null && uniqueId !== "";

    // ------------------------------------------ internal/cluster/utils.js
    const callbacks = new Map();
    let seq = 0;

    const sendHelper = (target, message, handle, cb) => {
      if (!target || !target.connected) return false;
      // Mark message as internal. See INTERNAL_PREFIX in
      // lib/internal/child_process.js.
      message = Object.assign({ cmd: "NODE_CLUSTER" }, message, { seq });
      if (typeof cb === "function") callbacks.set(seq, cb);
      seq += 1;
      return target.send(message, handle);
    };

    // Returns an internalMessage listener that hands off normal messages to the
    // callback but intercepts and redirects ACK messages.
    const internal = (worker, cb) => function onInternalMessage(message, handle) {
      if (message === null || typeof message !== "object" || message.cmd !== "NODE_CLUSTER") return;
      let fn = cb;
      if (message.ack !== undefined) {
        const callback = callbacks.get(message.ack);
        if (callback !== undefined) { fn = callback; callbacks.delete(message.ack); }
      }
      return fn.apply(worker, arguments);
    };

    // ----------------------------------------- internal/cluster/worker.js
    function Worker(options) {
      if (!(this instanceof Worker)) return new Worker(options);
      EventEmitter.call(this);
      if (options === null || typeof options !== "object") options = {};
      this.exitedAfterDisconnect = undefined;
      this.state = options.state || "none";
      this.id = options.id | 0;
      if (options.process) {
        this.process = options.process;
        this.process.on("error", (code, signal) => this.emit("error", code, signal));
        this.process.on("message", (message, handle) => this.emit("message", message, handle));
      }
    }
    Object.setPrototypeOf(Worker.prototype, EventEmitter.prototype);
    Object.setPrototypeOf(Worker, EventEmitter);

    Worker.prototype.kill = function () { return this.destroy.apply(this, arguments); };
    Worker.prototype.send = function () { return this.process.send.apply(this.process, arguments); };
    Worker.prototype.isDead = function () {
      return this.process.exitCode != null || this.process.signalCode != null;
    };
    Worker.prototype.isConnected = function () { return this.process.connected; };

    const SCHED_NONE = 1;
    const SCHED_RR = 2;

    // ==================================================================
    //                    internal/cluster/primary.js
    // ==================================================================
    const buildPrimary = () => {
      const cluster = new EventEmitter();
      const intercom = new EventEmitter();
      const handles = new Map();

      cluster.isWorker = false;
      cluster.isMaster = true;   // Deprecated alias. Must be same as isPrimary.
      cluster.isPrimary = true;
      cluster.Worker = Worker;
      cluster.workers = {};
      cluster.settings = {};
      cluster.SCHED_NONE = SCHED_NONE;  // Leave it to the operating system.
      cluster.SCHED_RR = SCHED_RR;      // Primary distributes connections.

      let ids = 0;
      let initialized = false;

      const policyEnv = proc.env.NODE_CLUSTER_SCHED_POLICY;
      let schedulingPolicy = SCHED_RR;
      if (policyEnv === "rr") schedulingPolicy = SCHED_RR;
      else if (policyEnv === "none") schedulingPolicy = SCHED_NONE;
      else if (proc.platform === "win32") schedulingPolicy = SCHED_NONE;
      cluster.schedulingPolicy = schedulingPolicy;

      const nextTick = (fn, ...a) => proc.nextTick(fn, ...a);

      cluster.setupPrimary = function (options) {
        const settings = Object.assign({
          args: Array.prototype.slice.call(proc.argv, 2),
          exec: proc.argv[1],
          execArgv: proc.execArgv,
          silent: false,
        }, cluster.settings, options);

        cluster.settings = settings;

        if (initialized === true) return nextTick(setupSettingsNT, settings);

        initialized = true;
        schedulingPolicy = cluster.schedulingPolicy;  // Freeze policy.
        if (schedulingPolicy !== SCHED_NONE && schedulingPolicy !== SCHED_RR) {
          const e = new Error("Bad cluster.schedulingPolicy: " + schedulingPolicy);
          e.code = "ERR_INVALID_ARG_VALUE";
          throw e;
        }
        nextTick(setupSettingsNT, settings);
      };
      // Deprecated alias must be same as setupPrimary.
      cluster.setupMaster = cluster.setupPrimary;

      function setupSettingsNT(settings) { cluster.emit("setup", settings); }

      function createWorkerProcess(id, env) {
        const workerEnv = Object.assign({}, proc.env, env, { NODE_UNIQUE_ID: "" + id });
        const execArgv = (cluster.settings.execArgv || []).slice();
        const child_process = M["child_process"] || M["node:child_process"];
        return child_process.fork(cluster.settings.exec, cluster.settings.args, {
          cwd: cluster.settings.cwd,
          env: workerEnv,
          serialization: cluster.settings.serialization,
          silent: cluster.settings.silent,
          windowsHide: cluster.settings.windowsHide,
          execArgv: execArgv,
          stdio: cluster.settings.stdio,
          gid: cluster.settings.gid,
          uid: cluster.settings.uid,
        });
      }

      function removeWorker(worker) {
        if (!worker) throw new Error("ERR_INTERNAL_ASSERTION");
        delete cluster.workers[worker.id];
        if (Object.keys(cluster.workers).length === 0) {
          if (handles.size !== 0) throw new Error("Resource leak detected.");
          intercom.emit("disconnect");
        }
      }

      function removeHandlesForWorker(worker) {
        if (!worker) throw new Error("ERR_INTERNAL_ASSERTION");
        handles.forEach((handle, key) => { if (handle.remove(worker)) handles.delete(key); });
      }

      cluster.fork = function (env) {
        cluster.setupPrimary();
        const id = ++ids;
        const workerProcess = createWorkerProcess(id, env);
        const worker = new Worker({ id: id, process: workerProcess });

        worker.on("message", function (message, handle) {
          cluster.emit("message", this, message, handle);
        });

        worker.process.once("exit", (exitCode, signalCode) => {
          // Remove the worker from the workers list only if it has
          // disconnected, otherwise we might still want to access it.
          if (!worker.isConnected()) {
            removeHandlesForWorker(worker);
            removeWorker(worker);
          }
          worker.exitedAfterDisconnect = !!worker.exitedAfterDisconnect;
          worker.state = "dead";
          worker.emit("exit", exitCode, signalCode);
          cluster.emit("exit", worker, exitCode, signalCode);
        });

        worker.process.once("disconnect", () => {
          // Now is a good time to remove the handles associated with this
          // worker because it is not connected to the primary anymore.
          removeHandlesForWorker(worker);
          // Remove the worker from the workers list only if its process has
          // exited. Otherwise, we might still want to access it.
          if (worker.isDead()) removeWorker(worker);
          worker.exitedAfterDisconnect = !!worker.exitedAfterDisconnect;
          worker.state = "disconnected";
          worker.emit("disconnect");
          cluster.emit("disconnect", worker);
        });

        worker.process.on("internalMessage", internal(worker, onmessage));
        nextTick(emitForkNT, worker);
        cluster.workers[worker.id] = worker;
        return worker;
      };

      function emitForkNT(worker) { cluster.emit("fork", worker); }

      cluster.disconnect = function (cb) {
        const workers = Object.keys(cluster.workers);
        if (workers.length === 0) {
          nextTick(() => intercom.emit("disconnect"));
        } else {
          for (const key of workers) {
            const worker = cluster.workers[key];
            if (worker && worker.isConnected()) worker.disconnect();
          }
        }
        if (typeof cb === "function") intercom.once("disconnect", cb);
      };

      function onmessage(message, handle) {
        const worker = this;
        if (message.act === "online") online(worker);
        else if (message.act === "queryServer") queryServer(worker, message);
        else if (message.act === "listening") listening(worker, message);
        else if (message.act === "exitedAfterDisconnect") exitedAfterDisconnect(worker, message);
        else if (message.act === "close") close(worker, message);
      }

      function online(worker) {
        worker.state = "online";
        worker.emit("online");
        cluster.emit("online", worker);
      }

      function exitedAfterDisconnect(worker, message) {
        worker.exitedAfterDisconnect = true;
        send(worker, { ack: message.seq });
      }

      // ---------------------------------------------------------------
      //   internal/cluster/{round_robin_handle,shared_handle}.js
      // ---------------------------------------------------------------
      // node hands a libuv handle object to the worker; mbun's net/dgram layers
      // are descriptor-based, so the thing that crosses the channel is the raw
      // fd (SCM_RIGHTS, process_web.cppm's NODE_HANDLE protocol) wrapped in the
      // minimal object the receiving side needs. Substitutions, all documented
      // at their use sites:
      //   * `errno` in a reply is the errno NAME (a string) rather than a
      //     negative uv code — mbun's native layer reports codes, and nothing
      //     but net.js's error builder ever reads the value;
      //   * RoundRobinHandle keeps net.Server itself as `this.handle` (mbun has
      //     no detachable `server._handle`) and taps its accept loop through
      //     `server._rawAccept`.
      const PROCN = () => G.__mbunProcNative;
      const closeFd = (fd) => { if (typeof fd === "number" && fd >= 0) { try { PROCN().close(fd); } catch (e) {} } };
      const ownedFd = (fd) => ({
        fd,
        __ipcSendFd() { return { fd: this.fd, type: "net.Native" }; },
        close(cb) { closeFd(this.fd); this.fd = -1; if (typeof cb === "function") G.queueMicrotask(cb); },
      });
      // A descriptor the primary keeps: every worker gets a dup, the original is
      // closed only when the last worker drops it.
      const sharedFd = (getFd, type) => ({
        __ipcSendFd() { return { fd: getFd(), type: type }; },
        close() {},
      });

      function SharedHandle(key, address, message) {
        this.key = key;
        this.workers = new Map();
        this.fd = -1;
        this.errno = 0;
        this.data = undefined;
        this.sockname = null;
        const udp = message.addressType === "udp4" || message.addressType === "udp6";
        try {
          if (udp) {
            const ND = G.__mbunDgramNative;
            const fd = ND.create(message.addressType, true);
            const info = ND.bind(fd, address === "" ? "" : String(address),
                                 message.port < 0 ? 0 : message.port,
                                 message.addressType === "udp6", !!(message.flags & 1));
            this.fd = fd;
            this.sockname = info || null;
          } else {
            const NN = G.__mbunNetNative;
            const host = address === "::" || address === "" ? "0.0.0.0" : (address === "::1" ? "127.0.0.1" : String(address));
            const lh = NN.listen(host, message.port < 0 ? 0 : message.port);
            this.fd = lh.fd;
            this.sockname = { address: address, port: lh.port, family: message.addressType === 6 ? "IPv6" : "IPv4" };
          }
        } catch (e) {
          // The natives throw a plain Error whose message carries the errno
          // name (runtime/net.inc net_errno_name); net.js classifies the same
          // way. Without this a bind that failed EACCES was reported as
          // EADDRINUSE (test-cluster-shared-handle-bind-privileged-port).
          const hit = /\b(E[A-Z]+)\b/.exec(String((e && e.message) || e));
          this.errno = hit ? hit[1] : "EADDRINUSE";
        }
      }
      SharedHandle.prototype.add = function (worker, send) {
        this.workers.set(worker.id, worker);
        if (this.errno) { send(this.errno, null, null); return; }
        const self = this;
        send(0, { sockname: this.sockname }, sharedFd(() => self.fd, "net.Native"));
      };
      SharedHandle.prototype.remove = function (worker) {
        if (!this.workers.has(worker.id)) return false;
        this.workers.delete(worker.id);
        if (this.workers.size !== 0) return false;
        closeFd(this.fd);
        this.fd = -1;
        return true;
      };

      function RoundRobinHandle(key, address, message) {
        const net = M["net"] || M["node:net"];
        this.key = key;
        this.all = new Map();
        this.free = new Map();
        this.queue = [];
        this.listening = false;
        this.errno = 0;
        this.data = undefined;
        this.server = net.createServer();
        this.server._rawAccept = (fd) => this.distribute(0, fd);
        const opts = { backlog: message.backlog, exclusive: true };
        if (message.port >= 0) {
          opts.port = message.port;
          if (address != null && address !== "") opts.host = address;
          if (message.flags & 1) opts.ipv6Only = true;
        } else {
          opts.path = address;
        }
        this.server.once("listening", () => { this.listening = true; });
        this.server.once("error", (err) => { this.errno = (err && err.code) || "EADDRINUSE"; });
        this.server.listen(opts);
      }
      RoundRobinHandle.prototype.add = function (worker, send) {
        this.all.set(worker.id, worker);
        const done = () => {
          if (this.errno) { send(this.errno, null, null); return; }
          const out = Object.assign({}, this.server.address() || {});
          send(0, { sockname: out }, null);
          this.handoff(worker);   // In case there are connections pending.
        };
        if (this.listening || this.errno) return done();
        this.server.once("listening", done);
        this.server.once("error", () => done());
      };
      RoundRobinHandle.prototype.remove = function (worker) {
        const existed = this.all.delete(worker.id);
        if (!existed) return false;
        this.free.delete(worker.id);
        if (this.all.size !== 0) return false;
        while (this.queue.length) closeFd(this.queue.shift());
        try { this.server.close(); } catch (e) {}
        return true;
      };
      RoundRobinHandle.prototype.distribute = function (err, fd) {
        if (err) return;   // If `accept` fails just skip it.
        this.queue.push(fd);
        for (const entry of this.free) {
          this.free.delete(entry[0]);
          this.handoff(entry[1]);
          break;
        }
      };
      RoundRobinHandle.prototype.handoff = function (worker) {
        if (!this.all.has(worker.id)) return;   // Worker is closing the server.
        if (this.queue.length === 0) { this.free.set(worker.id, worker); return; }
        const handle = ownedFd(this.queue.shift());
        send(worker, { act: "newconn", key: this.key }, handle, (reply) => {
          if (reply.accepted) handle.close();
          else this.distribute(0, handle.fd);   // Worker is shutting down. Send to another.
          this.handoff(worker);
        });
      };

      cluster._handleFactory = (key, address, message, policy, worker) => {
        // UDP is exempt from round-robin connection balancing for what should be
        // obvious reasons: it's connectionless. There is nothing to send to the
        // workers except raw datagrams and that's pointless.
        if (message.addressType === "udp4" || message.addressType === "udp6") {
          return new SharedHandle(key, address, message);
        }
        if (policy !== SCHED_RR) return new SharedHandle(key, address, message);
        return new RoundRobinHandle(key, address, message);
      };

      function queryServer(worker, message) {
        // Stop processing if worker already disconnecting.
        if (worker.exitedAfterDisconnect) return;

        const key = message.address + ":" + message.port + ":" + message.addressType +
                    ":" + message.fd + ":" + message.index;
        let handle = handles.get(key);

        if (handle === undefined) {
          let address = message.address;

          // Find shortest path for unix sockets because of the ~100 byte limit.
          if (message.port < 0 && typeof address === "string" && proc.platform !== "win32") {
            const path = M["path"] || M["node:path"];
            address = path.relative(proc.cwd(), address);
            if (message.address.length < address.length) address = message.address;
          }

          const factory = cluster._handleFactory;
          if (typeof factory !== "function") {
            // No handle-passing support in this build: answer with an errno so
            // the worker's listen() fails loudly instead of hanging forever.
            send(worker, { errno: -95, key, ack: message.seq, data: message.data });
            return;
          }
          handle = factory(key, address, message, schedulingPolicy, worker);
          if (handle === null) return;   // factory already replied/emitted
          handles.set(key, handle);
        }

        if (!handle.data) handle.data = message.data;

        // Set custom server data.
        handle.add(worker, (errno, reply, replyHandle) => {
          const entry = handles.get(key);
          const data = entry ? entry.data : undefined;
          if (errno) handles.delete(key);   // Gives other workers a chance to retry.
          send(worker, Object.assign({ errno, key, ack: message.seq, data }, reply), replyHandle);
        });
      }

      function listening(worker, message) {
        const info = {
          addressType: message.addressType,
          address: message.address,
          port: message.port,
          fd: message.fd,
        };
        worker.state = "listening";
        worker.emit("listening", info);
        cluster.emit("listening", worker, info);
      }

      // Server in worker is closing, remove from list. The handle may have been
      // removed by a prior call to removeHandlesForWorker() so guard against that.
      function close(worker, message) {
        const key = message.key;
        const handle = handles.get(key);
        if (handle && handle.remove(worker)) handles.delete(key);
      }

      function send(worker, message, handle, cb) {
        return sendHelper(worker.process, message, handle, cb);
      }

      // Extend generic Worker with methods specific to the primary process.
      Worker.prototype.disconnect = function () {
        this.exitedAfterDisconnect = true;
        send(this, { act: "disconnect" });
        this.process.disconnect();
        removeHandlesForWorker(this);
        removeWorker(this);
        return this;
      };

      Worker.prototype.destroy = function (signo) {
        const p = this.process;
        const signal = signo || "SIGTERM";
        p.kill(signal);
      };

      cluster._handles = handles;
      cluster._sendHelper = sendHelper;
      return cluster;
    };

    // ==================================================================
    //                     internal/cluster/child.js
    // ==================================================================
    const buildChild = () => {
      const cluster = new EventEmitter();
      const handles = new Map();
      const indexes = new Map();
      const noop = function () {};
      const TIMEOUT_MAX = 2147483647;
      const kNoFailure = 0;

      cluster.isWorker = true;
      cluster.isMaster = false;   // Deprecated alias. Must be same as isPrimary.
      cluster.isPrimary = false;
      cluster.worker = null;
      cluster.Worker = Worker;

      const send = (message, cb) => sendHelper(proc, message, null, cb);
      // process.disconnect() raises ERR_IPC_DISCONNECTED on a channel that is
      // already gone (node lib/internal/child_process.js target.disconnect), and
      // the primary closes its end at the same time it sends `act: "disconnect"`,
      // so the teardown below can legitimately race the EOF.
      const disconnectChannel = () => { if (proc.connected) proc.disconnect(); };
      cluster._send = send;
      cluster._handles = handles;

      cluster._setupWorker = function () {
        const worker = new Worker({ id: +uniqueId | 0, process: proc, state: "online" });
        cluster.worker = worker;

        // node does not let this one listener pin the loop (V8:
        // channel[kIgnoreOneDisconnectEvent]). mbun's equivalent seam is the
        // __mbunIpcPin predicate consulted by the io tick.
        const prevPin = typeof G.__mbunIpcPin === "function" ? G.__mbunIpcPin : null;
        try {
          Object.defineProperty(G, "__mbunIpcPin", {
            value: function () {
              if (prevPin) { try { if (prevPin()) return true; } catch (e) {} }
              return proc.listenerCount("message") > 0 || proc.listenerCount("disconnect") > 1;
            },
            writable: true, configurable: true, enumerable: false,
          });
        } catch (e) {}

        proc.once("disconnect", () => {
          proc.channel = null;
          worker.emit("disconnect");
          if (!worker.exitedAfterDisconnect) {
            // Unexpected disconnect, primary exited, or some such nastiness, so
            // worker exits immediately.
            proc.exit(kNoFailure);
          }
        });

        proc.on("internalMessage", internal(worker, onmessage));
        send({ act: "online" });

        function onmessage(message, handle) {
          if (message.act === "newconn") onconnection(message, handle);
          else if (message.act === "disconnect") worker._disconnect(true);
        }
      };

      // `obj` is a net#Server or a dgram#Socket object.
      cluster._getServer = function (obj, options, cb) {
        let address = options.address;

        // Resolve unix socket paths to absolute paths.
        if (options.port < 0 && typeof address === "string" && proc.platform !== "win32") {
          const path = M["path"] || M["node:path"];
          address = path.resolve(address);
        }

        const indexesKey = [address, options.port, options.addressType, options.fd].join(":");
        let indexSet = indexes.get(indexesKey);
        if (indexSet === undefined) {
          indexSet = { nextIndex: 0, set: new Set() };
          indexes.set(indexesKey, indexSet);
        }
        const index = indexSet.nextIndex++;
        indexSet.set.add(index);

        const message = Object.assign({ act: "queryServer", index, data: null }, options);
        message.address = address;

        // Set custom data on handle (i.e. tls tickets key).
        if (obj._getServerData) message.data = obj._getServerData();

        send(message, (reply, handle) => {
          if (typeof obj._setServerData === "function") obj._setServerData(reply.data);
          if (handle) shared(reply, { handle, indexesKey, index }, cb);   // Shared listen socket.
          else rr(reply, { indexesKey, index }, cb);                      // Round-robin.
        });

        obj.once("listening", () => {
          // Short-lived sockets might have been closed.
          if (!indexes.has(indexesKey)) return;
          cluster.worker.state = "listening";
          const addr = obj.address();
          message.act = "listening";
          message.port = (addr && addr.port) || options.port;
          send(message);
        });
      };

      function removeIndexesKey(indexesKey, index) {
        const indexSet = indexes.get(indexesKey);
        if (!indexSet) return;
        indexSet.set.delete(index);
        if (indexSet.set.size === 0) indexes.delete(indexesKey);
      }

      // Shared listen socket.
      function shared(message, ctx, cb) {
        const handle = ctx.handle;
        const key = message.key;
        // mbun substitution: the descriptor arrives without libuv's getsockname,
        // so the primary's bound address rides in the reply instead.
        if (message.sockname) handle.sockname = message.sockname;
        // Monkey-patch the close() method so we can keep track of when it's
        // closed. Avoids resource leaks when the handle is short-lived.
        const close = handle.close;
        handle.close = function () {
          send({ act: "close", key });
          handles.delete(key);
          removeIndexesKey(ctx.indexesKey, ctx.index);
          return close.apply(handle, arguments);
        };
        handles.set(key, handle);
        cb(message.errno, handle);
      }

      // Round-robin. Primary distributes handles across workers.
      function rr(message, ctx, cb) {
        if (message.errno) return cb(message.errno, null);

        let key = message.key;
        let fakeHandle = null;

        function ref() { if (!fakeHandle) fakeHandle = G.setInterval(noop, TIMEOUT_MAX); }
        function unref() { if (fakeHandle) { G.clearInterval(fakeHandle); fakeHandle = null; } }
        function listen(backlog) { return 0; }

        function close() {
          // lib/net.js treats server._handle.close() as effectively synchronous.
          // That means there is a time window between the call to close() and
          // the ack by the primary process in which we can still receive
          // handles. onconnection() below handles that by sending those handles
          // back to the primary.
          if (key === undefined) return;
          unref();
          send({ act: "close", key });
          handles.delete(key);
          removeIndexesKey(ctx.indexesKey, ctx.index);
          key = undefined;
        }

        function getsockname(out) {
          if (key) Object.assign(out, message.sockname);
          return 0;
        }

        // Faux handle. Mimics a TCPWrap with just enough fidelity to fool
        // net.Server. Fools net.Server into thinking that it's backed by a
        // real handle. Use a noop function for ref() and unref() because the
        // control channel is going to keep the worker alive anyway.
        const handle = { close, listen, ref, unref };
        handle.ref();
        if (message.sockname) handle.getsockname = getsockname;  // TCP handles only.
        handles.set(key, handle);
        cb(0, handle);
      }

      // Round-robin connection.
      function onconnection(message, handle) {
        const key = message.key;
        const server = handles.get(key);
        let accepted = server !== undefined;

        if (accepted && server.owner) {
          const self = server.owner;
          if (self.maxConnections != null && self._connections >= self.maxConnections) accepted = false;
        }

        send({ ack: message.seq, accepted });

        if (accepted) server.onconnection(0, handle);
        else if (handle && typeof handle.close === "function") handle.close();
      }

      // Extend generic Worker with methods specific to worker processes.
      Worker.prototype.disconnect = function () {
        if (this.state !== "disconnecting" && this.state !== "destroying") {
          this.state = "disconnecting";
          this._disconnect();
        }
        return this;
      };

      Worker.prototype._disconnect = function (primaryInitiated) {
        this.exitedAfterDisconnect = true;
        let waitingCount = 1;

        function checkWaitingCount() {
          waitingCount--;
          if (waitingCount === 0) {
            // If disconnect is worker initiated, wait for ack to be sure
            // exitedAfterDisconnect is properly set in the primary, otherwise,
            // if it's primary initiated there's no need to send the
            // exitedAfterDisconnect message.
            if (primaryInitiated) disconnectChannel();
            else send({ act: "exitedAfterDisconnect" }, disconnectChannel);
          }
        }

        handles.forEach((handle) => {
          waitingCount++;
          if (handle.owner) handle.owner.close(checkWaitingCount);
          else handle.close(checkWaitingCount);
        });
        handles.clear();
        checkWaitingCount();
      };

      Worker.prototype.destroy = function () {
        if (this.state === "destroying") return;
        this.exitedAfterDisconnect = true;
        if (!this.isConnected()) {
          proc.exit(kNoFailure);
        } else {
          this.state = "destroying";
          send({ act: "exitedAfterDisconnect" }, disconnectChannel);
          proc.once("disconnect", () => proc.exit(kNoFailure));
        }
      };

      return cluster;
    };

    const built = isChild ? buildChild() : buildPrimary();
    M["cluster"] = built;
    M["node:cluster"] = built;
    G.__mbunCluster = built;

    // node internal/process/pre_execution.js initializeClusterIPC(): a forked
    // worker wires its Worker object up BEFORE the entry script runs, then drops
    // NODE_UNIQUE_ID so grandchildren do not inherit it.
    if (isChild && proc.argv && proc.argv[1]) {
      try { built._setupWorker(); } catch (e) {}
      try { delete proc.env.NODE_UNIQUE_ID; } catch (e) {}
      // mbun's process.env is a startup snapshot, not a live view of environ, so
      // the delete above would not stop a grandchild from inheriting the id
      // (node's test-cluster-basic spawns one and asserts it is a primary).
      try {
        const PN = G.__mbunProcNative;
        if (PN && typeof PN.setenv === "function") PN.setenv("NODE_UNIQUE_ID", null);
      } catch (e) {}
    }
  } catch (e) {}
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
