// T-LOOP smoke: Bun.serve over the native epoll event loop (__mbunServeNative
// → mbun.runtime_server.Http1Server) answered by the in-process fetch()
// client, all driven by the engine pump. Covers: bridge presence (Linux),
// GET round-trip, POST body echo, routes matching, keep-alive reuse of one
// server across sequential fetches, and stop().
import std;
import mbun.jsc.runtime;

namespace {

int gFailed{0};

void expect(bool cond, std::string_view what) {
    if (!cond) {
        ++gFailed;
        std::println("  FAIL: {}", what);
    }
}

}  // namespace

int main() {
#if !defined(_WIN32)
    using namespace mbun::jsc::runtime;

    auto setup = eval(R"MJS(
      globalThis.__done = 0; globalThis.__res = ""; globalThis.__err = "";
      (async () => {
        try {
          if (!globalThis.__mbunServeNative) throw new Error("native serve bridge missing");
          const srv = Bun.serve({
            port: 0,
            routes: { "/route/:name": (req) => new Response("route:" + req.params.name) },
            fetch(req) {
              const u = new URL(req.url);
              if (u.pathname === "/echo") return req.text().then((t) => new Response("echo:" + t, { status: 201 }));
              return new Response("hello-native");
            },
          });
          const base = "http://127.0.0.1:" + srv.port;
          const r1 = await fetch(base + "/");
          const t1 = await r1.text();
          const r2 = await fetch(base + "/echo", { method: "POST", body: "abc" });
          const t2 = await r2.text();
          const r3 = await fetch(base + "/route/xyz");
          const t3 = await r3.text();
          await srv.stop(true);
          globalThis.__res = [t1, r1.status, t2, r2.status, t3, r3.status].join("|");
        } catch (e) { globalThis.__err = String((e && e.stack) || e); }
        globalThis.__done = 1;
      })();
    )MJS");
    expect(setup.has_value(), "serve smoke script evaluates");
    if (setup.has_value()) {
        pump_event_loop("globalThis.__done");
        auto err{eval_to_string("globalThis.__err")};
        expect(err.has_value() && err->empty(),
               "serve smoke ran without error: " + err.value_or("<eval failed>"));
        auto res{eval_to_string("globalThis.__res")};
        expect(res.has_value() && *res == "hello-native|200|echo:abc|201|route:xyz|200",
               "native serve round-trips (got '" + res.value_or("<eval failed>") + "')");
    }

    // Round 2: request.signal abort on client disconnect, fetch AbortSignal,
    // chunked streaming for async-generator bodies, HEAD framing echo, and
    // IPv6 loopback listen (bun-server.test.ts fail harvest).
    auto setup2 = eval(R"MJS(
      globalThis.__done2 = 0; globalThis.__res2 = ""; globalThis.__err2 = "";
      (async () => {
        try {
          const out = [];
          // chunked stream + HEAD framing
          const srv = Bun.serve({
            port: 0,
            async fetch(req) {
              const u = new URL(req.url);
              if (u.pathname === "/gen") return new Response(async function* () { yield "Hello"; yield " "; yield "World"; }());
              return new Response("plain");
            },
          });
          const r1 = await fetch(srv.url + "gen");
          out.push(r1.headers.get("transfer-encoding"), await r1.text());
          const r2 = await fetch(srv.url + "gen", { method: "HEAD" });
          out.push(r2.headers.get("transfer-encoding"), (await r2.text()).length);
          await srv.stop(true);
          // abort: client aborts mid-handler; server req.signal must fire
          let signaled = Promise.withResolvers();
          const ac = new AbortController();
          const srv2 = Bun.serve({
            port: 0,
            async fetch(req) {
              req.signal.addEventListener("abort", () => signaled.resolve("signaled"));
              ac.abort();
              await signaled.promise;
              return new Response("late");
            },
          });
          let aborted = "";
          try { await fetch(srv2.url, { signal: ac.signal }).then((r) => r.text()); }
          catch (e) { aborted = (e && e.name) || "err"; }
          out.push(aborted, await signaled.promise);
          await srv2.stop(true);
          // IPv6 loopback listen
          const srv6 = Bun.serve({ hostname: "::1", port: 0, fetch() { return new Response("v6"); } });
          out.push(srv6.port > 0 ? "v6ok" : "v6fail");
          await srv6.stop(true);
          // streaming request body: req.body is a live stream (dispatch-on-
          // headers); bodiless GET keeps req.body === null.
          const srv7 = Bun.serve({
            port: 0,
            async fetch(req) {
              if (req.method === "GET") return new Response(req.body === null ? "nullbody" : "hasbody");
              const t = await req.text();
              return new Response("got:" + t);
            },
          });
          out.push(await fetch(srv7.url).then((r) => r.text()));
          out.push(await fetch(srv7.url, { method: "POST", body: "streamed!" }).then((r) => r.text()));
          await srv7.stop(true);
          globalThis.__res2 = out.join("|");
        } catch (e) { globalThis.__err2 = String((e && e.stack) || e); }
        globalThis.__done2 = 1;
      })();
    )MJS");
    // Round 3: Bun.connect raw client against Bun.serve + request.url
    // dot-segment normalization (bun-server "normlizes incoming request URLs").
    auto setup3 = eval(R"MJS(
      globalThis.__done3 = 0; globalThis.__res3 = ""; globalThis.__err3 = "";
      (async () => {
        try {
          const srv = Bun.serve({
            port: 0,
            fetch(req) { return new Response(req.url, { headers: { "Connection": "close" } }); },
          });
          const got = Promise.withResolvers();
          await Bun.connect({
            hostname: "localhost",
            port: srv.port,
            socket: {
              open(s) { s.write("GET /foo/bar/../ HTTP/1.1\r\nHost: localhost:" + srv.port + "\r\n\r\n"); },
              data(s, d) {
                const text = new TextDecoder().decode(d);
                got.resolve(text.split("\r\n\r\n").at(-1));
                s.end();
              },
            },
          });
          const body = await got.promise;
          await srv.stop(true);
          const want = new URL("/foo/bar/../", "http://localhost:" + srv.port).href;
          globalThis.__res3 = (body === want) ? "normalized" : ("got " + body + " want " + want);
        } catch (e) { globalThis.__err3 = String((e && e.stack) || e); }
        globalThis.__done3 = 1;
      })();
    )MJS");
    expect(setup3.has_value(), "round-3 script evaluates");
    if (setup3.has_value()) {
        pump_event_loop("globalThis.__done3");
        auto err{eval_to_string("globalThis.__err3")};
        expect(err.has_value() && err->empty(),
               "round 3 ran without error: " + err.value_or("<eval failed>"));
        auto res{eval_to_string("globalThis.__res3")};
        expect(res.has_value() && *res == "normalized",
               "Bun.connect + url normalization (got '" + res.value_or("<eval failed>") + "')");
    }

    // Round 4: WebSocket upgrade (T-LOOP.4) — client WebSocket against
    // Bun.serve({websocket}): echo text + binary, publish/subscribe fanout,
    // server-initiated close reaching the client.
    auto setup4 = eval(R"MJS(
      globalThis.__done4 = 0; globalThis.__res4 = ""; globalThis.__err4 = "";
      (async () => {
        try {
          const out = [];
          const srv = Bun.serve({
            port: 0,
            fetch(req, server) {
              if (server.upgrade(req, { data: { n: 7 } })) return;
              return new Response("no-ws", { status: 400 });
            },
            websocket: {
              open(ws) { ws.subscribe("room"); out.push("open:" + ws.data.n); },
              message(ws, msg) {
                if (typeof msg === "string") {
                  if (msg === "bye") { ws.close(1000, "done"); return; }
                  if (msg === "fan") { srv.publish("room", "fanout"); return; }
                  ws.send("echo:" + msg);
                } else {
                  ws.send(new Uint8Array([msg[0] + 1, msg[1] + 1]));
                }
              },
              close(ws, code) { globalThis.__srvClosed4 && globalThis.__srvClosed4.resolve(code); },
            },
          });
          const got = [];
          const closed = Promise.withResolvers();
          globalThis.__step5 = "wss";
          const opened = Promise.withResolvers();
          const srvClosed = (globalThis.__srvClosed4 = Promise.withResolvers());
          const msgN = () => new Promise((res) => { const h = (e) => { sock.removeEventListener("message", h); res(e.data); }; sock.addEventListener("message", h); });
          const sock = new WebSocket("ws://127.0.0.1:" + srv.port + "/chat");
          sock.binaryType = "arraybuffer";
          sock.onopen = () => opened.resolve();
          sock.onclose = (e) => closed.resolve(e);
          await opened.promise;
          out.push("clientopen");
          let p = msgN(); sock.send("hello"); got.push(await p);
          p = msgN(); sock.send(new Uint8Array([1, 2])); const bin = await p;
          got.push(bin instanceof ArrayBuffer ? Array.from(new Uint8Array(bin)).join(",") : "notab");
          p = msgN(); sock.send("fan"); got.push(await p);  // publish reaches subscriber
          out.push("subs:" + srv.subscriberCount("room"));
          sock.send("bye");
          const ce = await closed.promise;
          out.push("srvclose:" + (await srvClosed.promise));
          out.push("clientclose:" + ce.code + ":" + ce.wasClean);
          await srv.stop(true);
          globalThis.__res4 = out.join("|") + "#" + got.join("|");
        } catch (e) { globalThis.__err4 = String((e && e.stack) || e); }
        globalThis.__done4 = 1;
      })();
    )MJS");
    expect(setup4.has_value(), "round-4 script evaluates");
    if (setup4.has_value()) {
        pump_event_loop("globalThis.__done4");
        auto err{eval_to_string("globalThis.__err4")};
        expect(err.has_value() && err->empty(),
               "round 4 ran without error: " + err.value_or("<eval failed>"));
        auto res{eval_to_string("globalThis.__res4")};
        expect(res.has_value() &&
                   *res == "open:7|clientopen|subs:1|srvclose:1000|clientclose:1000:true"
                           "#echo:hello|2,3|fanout",
               "websocket upgrade round-trips (got '" + res.value_or("<eval failed>") + "')");
    }

    // Round 5: TLS (T-TLS.3) — serve({tls}) over the reactor TLS channels,
    // fetch(https://) with rejectUnauthorized:false, and a wss echo.
    auto setup5 = eval(R"MJS(
      globalThis.__done5 = 0; globalThis.__res5 = ""; globalThis.__err5 = "";
      (async () => {
        try {
          const out = [];
          globalThis.__step5 = "selfsigned";
          const pair = globalThis.__mbunNetNative.tlsSelfSigned("localhost");
          globalThis.__step5 = "serve1";
          const srv = Bun.serve({
            port: 0,
            tls: { cert: pair.cert, key: pair.key },
            async fetch(req) {
              const u = new URL(req.url);
              if (u.pathname === "/echo") return new Response("s:" + (await req.text()), { status: 201 });
              return new Response("hello-tls:" + u.protocol);
            },
            websocket: {
              message(ws, msg) { ws.send("wss-echo:" + msg); },
            },
          });
          out.push(srv.protocol, String(srv.url).slice(0, 8));
          globalThis.__step5 = "fetch1";
          const r1 = await fetch("https://localhost:" + srv.port + "/", { tls: { rejectUnauthorized: false } });
          out.push(r1.status, await r1.text());
          globalThis.__step5 = "fetch2";
          const r2 = await fetch("https://localhost:" + srv.port + "/echo", { method: "POST", body: "abc", tls: { rejectUnauthorized: false } });
          out.push(r2.status, await r2.text());
          // wss upgrade over the same server (fetch handler falls through to
          // upgrade when the request carries websocket headers)
          const srv2 = Bun.serve({
            port: 0,
            tls: { cert: pair.cert, key: pair.key },
            fetch(req, server) { if (server.upgrade(req)) return; return new Response("no", { status: 400 }); },
            websocket: { message(ws, msg) { ws.send("wss:" + msg); } },
          });
          globalThis.__step5 = "wss";
          const opened = Promise.withResolvers();
          const gotMsg = Promise.withResolvers();
          const sock = new WebSocket("wss://localhost:" + srv2.port + "/", { tls: { rejectUnauthorized: false } });
          sock.onopen = () => opened.resolve();
          sock.onmessage = (e) => gotMsg.resolve(e.data);
          await opened.promise;
          sock.send("ping!");
          out.push(await gotMsg.promise);
          sock.close(1000);
          await srv.stop(true);
          await srv2.stop(true);
          globalThis.__res5 = out.join("|");
        } catch (e) { globalThis.__err5 = "msg=" + String((e && e.message) || e) + " code=" + String((e && e.code) || "") + " at=" + globalThis.__step5; }
        globalThis.__done5 = 1;
      })();
    )MJS");
    expect(setup5.has_value(), "round-5 script evaluates");
    if (setup5.has_value()) {
        pump_event_loop("globalThis.__done5");
        auto err{eval_to_string("globalThis.__err5")};
        expect(err.has_value() && err->empty(),
               "round 5 ran without error: " + err.value_or("<eval failed>"));
        auto res{eval_to_string("globalThis.__res5")};
        expect(res.has_value() &&
                   *res == "https|https://|200|hello-tls:https:|201|s:abc|wss:ping!",
               "tls serve/fetch/wss round-trips (got '" + res.value_or("<eval failed>") + "')");
    }

    expect(setup2.has_value(), "round-2 script evaluates");
    if (setup2.has_value()) {
        pump_event_loop("globalThis.__done2");
        auto err{eval_to_string("globalThis.__err2")};
        expect(err.has_value() && err->empty(),
               "round 2 ran without error: " + err.value_or("<eval failed>"));
        auto res{eval_to_string("globalThis.__res2")};
        expect(res.has_value() &&
                   *res == "chunked|Hello World|chunked|0|AbortError|signaled|v6ok|nullbody|got:streamed!",
               "abort/chunked/HEAD/v6/body-stream behaviors (got '" + res.value_or("<eval failed>") + "')");
    }
#else
    std::println("serve_native smoke: SKIP (non-Linux: bridge is a deferred no-op)");
#endif
    std::println("serve_native smoke: {} failures", gFailed);
    return gFailed == 0 ? 0 : 1;
}
