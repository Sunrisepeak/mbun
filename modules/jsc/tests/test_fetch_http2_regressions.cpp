// Focused runtime regressions for Bun fetch({ protocol: "h2" }).
// Uses real TLS/HTTP2 servers and the pinned Bun harness certificate.
import std;
import mbun.jsc.runtime;

namespace {

int gFailed { 0 };

void expect(bool condition, std::string_view message) {
    if (!condition) {
        ++gFailed;
        std::println("  FAIL: {}", message);
    }
}

// The pinned Bun harness is located from THIS file, not from the working
// directory. `mcpp test` runs a member's tests with the cwd set to the member
// directory, so the previous `process.cwd() + "/compat/..."` only resolved when
// the suite happened to be launched from the repo root -- everywhere else
// realpathSync raised ENOENT on `<member>/compat` and killed the whole eval
// before any HTTP/2 assertion ran. test_runtime_structure.cpp already locates
// its inputs this way.
std::string harness_source_path() {
    const auto testFile { std::filesystem::path { __FILE__ } };
    const auto repoRoot { testFile.parent_path()   // tests
                              .parent_path()       // jsc
                              .parent_path()       // modules
                              .parent_path() };    // repo root
    return (repoRoot / "compat" / "bun" / "test" / "harness.ts").string();
}

// The harness lives in the compat/bun SUBMODULE, and the CI job that runs
// `mcpp test --workspace` checks out WITHOUT submodules (only the corpus job
// uses `submodules: recursive`). So the file is legitimately absent there, and a
// member test must not fail for a fixture the checkout was never given: with the
// cwd bug above fixed, CI still failed with the same ENOENT for this second,
// unrelated reason. Absent fixture -> announce and skip; present -> assert fully.
// This is not a corpus skip and buys no compatibility number -- the two sections
// it guards are runtime assertions, not corpus files.
bool harness_available() {
    std::error_code ec;
    return std::filesystem::exists(harness_source_path(), ec);
}

// The path as a JS string literal, prepended to the eval so the script can use
// it without depending on the process working directory.
std::string harness_path_decl() {
    std::string quoted;
    for (const char c : harness_source_path()) {
        if (c == '\\' || c == '"') quoted.push_back('\\');
        quoted.push_back(c);
    }
    return "globalThis.__h2HarnessPath = \"" + quoted + "\";\n";
}

}  // namespace

int main() {
#if !defined(_WIN32)
    using namespace mbun::jsc::runtime;

    const bool haveHarness { harness_available() };
    if (!haveHarness) {
        std::println("  SKIP: HTTP/2 sections need {} (compat/bun submodule not in this checkout)",
                     harness_source_path());
    }

    auto setup { eval(harness_path_decl() + R"MJS(
      globalThis.__h2SecurityDone = 0;
      globalThis.__h2SecurityResult = "";
      globalThis.__h2SecurityError = "";
      (async () => {
        try {
          const http2 = require("node:http2");
          const fs = require("node:fs");
          const harnessPath = fs.realpathSync(globalThis.__h2HarnessPath);
          const harness = fs.readFileSync(harnessPath, "utf8");
          const tlsBlock = harness.slice(harness.indexOf("export const tls"), harness.indexOf("export const invalidTls"));
          const tls = {
            cert: JSON.parse(/cert: ("[^"]+")/.exec(tlsBlock)[1]),
            key: JSON.parse(/key: ("[^"]+")/.exec(tlsBlock)[1]),
          };
          const listen = (server) => new Promise((resolve, reject) => {
            server.once("error", reject);
            server.listen(0, "127.0.0.1", resolve);
          });
          const close = (server) => new Promise((resolve) => server.close(resolve));
          const origin = (server) => `https://localhost:${server.address().port}`;
          const destination = http2.createSecureServer({ key: tls.key, cert: tls.cert });
          let destinationHeaders;
          destination.on("stream", (stream, headers) => {
            destinationHeaders = headers;
            stream.respond({ ":status": 200 });
            stream.end("redirect-ok");
          });
          await listen(destination);

          const source = http2.createSecureServer({ key: tls.key, cert: tls.cert });
          source.on("stream", (stream) => {
            stream.respond({ ":status": 302, location: origin(destination) + "/final" });
            stream.end("redirect-body");
          });
          await listen(source);
          const redirected = await fetch(origin(source) + "/start", {
            protocol: "h2",
            tls: { rejectUnauthorized: false },
            headers: {
              authorization: "Bearer secret",
              "proxy-authorization": "Basic secret",
              cookie: "session=secret",
              host: "old-origin.invalid",
            },
          });
          const redirectText = await redirected.text();
          const stripped = redirectText === "redirect-ok" && destinationHeaders &&
            destinationHeaders.authorization === undefined &&
            destinationHeaders["proxy-authorization"] === undefined &&
            destinationHeaders.cookie === undefined &&
            destinationHeaders[":authority"] === `localhost:${destination.address().port}`;
          await close(source);
          await close(destination);

          const identityServer = http2.createSecureServer({ key: tls.key, cert: tls.cert });
          let identityRequests = 0;
          identityServer.on("stream", (stream) => {
            ++identityRequests;
            stream.respond({ ":status": 200 });
            stream.end("must-not-resolve");
          });
          await listen(identityServer);
          let identityRejected = false;
          try {
            await fetch(origin(identityServer), {
              protocol: "h2",
              tls: {
                rejectUnauthorized: false,
                checkServerIdentity() { return new Error("blocked-by-callback"); },
              },
            });
          } catch (error) {
            identityRejected = error && /checkServerIdentity|HTTP\/2/i.test(String(error.message || error));
          }
          await close(identityServer);

          const mtlsServer = http2.createSecureServer({
            key: tls.key,
            cert: tls.cert,
            ca: tls.cert,
            requestCert: true,
            rejectUnauthorized: true,
          });
          mtlsServer.on("stream", (stream) => {
            stream.respond({ ":status": 200 });
            stream.end("mtls-ok");
          });
          await listen(mtlsServer);
          let mtlsText = "";
          try {
            const response = await fetch(origin(mtlsServer), {
              protocol: "h2",
              tls: { ca: tls.cert, cert: tls.cert, key: tls.key },
            });
            mtlsText = await response.text();
          } catch (error) {
            mtlsText = "rejected:" + String(error && (error.code || error.message || error));
          }
          await close(mtlsServer);

          globalThis.__h2SecurityResult = [
            stripped ? "strip" : "leak",
            identityRejected && identityRequests === 0 ? "identity" : "identity-bypass",
            mtlsText,
          ].join("|");
        } catch (error) {
          globalThis.__h2SecurityError = String((error && error.stack) || error);
        }
        globalThis.__h2SecurityDone = 1;
      })();
    )MJS") };
    expect(setup.has_value(), "HTTP2 security regression script evaluates");
    if (haveHarness && setup.has_value()) {
        pump_event_loop("globalThis.__h2SecurityDone");
        const auto error { eval_to_string("globalThis.__h2SecurityError") };
        expect(error.has_value() && error->empty(),
               "HTTP2 security regression ran without harness errors: " + error.value_or("<eval failed>"));
        const auto result { eval_to_string("globalThis.__h2SecurityResult") };
        expect(result.has_value() && *result == "strip|identity|mtls-ok",
               "HTTP2 redirect and TLS policies are enforced (got '" + result.value_or("<eval failed>") + "')");
    }

    auto streamingSetup { eval(harness_path_decl() + R"MJS(
      globalThis.__h2StreamingDone = 0;
      globalThis.__h2StreamingResult = "";
      globalThis.__h2StreamingError = "";
      (async () => {
        try {
          const http2 = require("node:http2");
          const fs = require("node:fs");
          const zlib = require("node:zlib");
          const harnessPath = fs.realpathSync(globalThis.__h2HarnessPath);
          const harness = fs.readFileSync(harnessPath, "utf8");
          const tlsBlock = harness.slice(harness.indexOf("export const tls"), harness.indexOf("export const invalidTls"));
          const tls = {
            cert: JSON.parse(/cert: ("[^"]+")/.exec(tlsBlock)[1]),
            key: JSON.parse(/key: ("[^"]+")/.exec(tlsBlock)[1]),
          };
          const listen = (server) => new Promise((resolve, reject) => {
            server.once("error", reject);
            server.listen(0, "127.0.0.1", resolve);
          });
          const close = (server) => new Promise((resolve) => server.close(resolve));
          const origin = (server) => `https://localhost:${server.address().port}`;
          const delay = (ms, value) => new Promise((resolve) => setTimeout(resolve, ms, value));
          const fetchH2 = (url, init) => fetch(url, {
            ...(init || {}), protocol: "h2", tls: { rejectUnauthorized: false },
          });
          const out = [];

          const delayed = http2.createSecureServer({ key: tls.key, cert: tls.cert });
          delayed.on("stream", (stream) => {
            stream.respond({ ":status": 200, "content-type": "text/event-stream" });
            stream.write("a");
            setTimeout(() => stream.end("b"), 350);
          });
          await listen(delayed);
          const delayedFetch = fetchH2(origin(delayed));
          const headWinner = await Promise.race([
            delayedFetch.then(() => "head"),
            delay(150, "late"),
          ]);
          const delayedResponse = await delayedFetch;
          const delayedText = await delayedResponse.text();
          out.push(headWinner, delayedText);
          await close(delayed);

          const nullBody = http2.createSecureServer({ key: tls.key, cert: tls.cert });
          const nullBodyClosed = Promise.withResolvers();
          let nullBodyCloseCount = 0;
          nullBody.on("stream", (stream, headers) => {
            stream.on("error", () => {});
            stream.on("close", () => {
              if (++nullBodyCloseCount === 2) nullBodyClosed.resolve("closed");
            });
            stream.respond({ ":status": headers[":path"] === "/no-content" ? 204 : 200 });
          });
          await listen(nullBody);
          const noContentResponse = await fetchH2(origin(nullBody) + "/no-content");
          const noContentBodyIsNull = noContentResponse.body === null;
          const noContentText = noContentBodyIsNull ? await noContentResponse.text() : "non-null";
          if (!noContentBodyIsNull) await noContentResponse.body.cancel();
          const headResponse = await fetchH2(origin(nullBody) + "/head", { method: "HEAD" });
          const headBodyIsNull = headResponse.body === null;
          const headText = headBodyIsNull ? await headResponse.text() : "non-null";
          if (!headBodyIsNull) await headResponse.body.cancel();
          const nullBodyTransport = await Promise.race([
            nullBodyClosed.promise,
            delay(250, "close-timeout"),
          ]);
          out.push(noContentBodyIsNull && noContentText === "" &&
            headBodyIsNull && headText === "" && nullBodyTransport === "closed"
            ? "null-bodies" : "non-null-bodies");
          await close(nullBody);

          const redirectDestination = http2.createSecureServer({ key: tls.key, cert: tls.cert });
          redirectDestination.on("stream", (stream) => {
            stream.respond({ ":status": 200 });
            stream.end("redirect-ok");
          });
          await listen(redirectDestination);
          const redirectSource = http2.createSecureServer({ key: tls.key, cert: tls.cert });
          let hangingRedirectStream;
          redirectSource.on("stream", (stream) => {
            hangingRedirectStream = stream;
            stream.on("error", () => {});
            stream.respond({ ":status": 302, location: origin(redirectDestination) });
            stream.write("body-that-never-ends");
          });
          await listen(redirectSource);
          const followed = fetchH2(origin(redirectSource)).then((response) => response.text(), () => "redirect-error");
          const redirectWinner = await Promise.race([followed, delay(250, "redirect-timeout")]);
          out.push(redirectWinner);
          if (hangingRedirectStream && !hangingRedirectStream.destroyed) hangingRedirectStream.close(http2.constants.NGHTTP2_CANCEL);
          await close(redirectSource);
          await close(redirectDestination);

          const gzip = http2.createSecureServer({ key: tls.key, cert: tls.cert });
          gzip.on("stream", (stream) => {
            const body = zlib.gzipSync("hello-h2");
            stream.respond({
              ":status": 200,
              "content-encoding": "gzip",
              "content-length": String(body.length),
            });
            stream.end(body);
          });
          await listen(gzip);
          const gzipResponse = await fetchH2(origin(gzip));
          const gzipText = await gzipResponse.text();
          out.push(gzipText === "hello-h2" &&
            gzipResponse.headers.get("content-encoding") === null &&
            gzipResponse.headers.get("content-length") === null ? "gzip" : "raw-gzip");
          await close(gzip);

          const cancelServer = http2.createSecureServer({ key: tls.key, cert: tls.cert });
          let cancelObserved = Promise.withResolvers();
          let cancelStream;
          cancelServer.on("stream", (stream) => {
            cancelStream = stream;
            stream.on("error", () => {});
            stream.on("close", () => cancelObserved.resolve("cancel"));
            stream.respond({ ":status": 200 });
            stream.write("event");
          });
          await listen(cancelServer);
          const cancelResponse = await Promise.race([fetchH2(origin(cancelServer)), delay(250, null)]);
          if (cancelResponse) {
            await cancelResponse.body.cancel("stop");
            out.push(await Promise.race([cancelObserved.promise, delay(250, "cancel-timeout")]));
          } else {
            out.push("cancel-head-timeout");
            if (cancelStream && !cancelStream.destroyed) cancelStream.close(http2.constants.NGHTTP2_CANCEL);
          }
          await close(cancelServer);

          const errorServer = http2.createSecureServer({ key: tls.key, cert: tls.cert });
          errorServer.on("stream", (stream) => {
            stream.on("error", () => {});
            stream.respond({ ":status": 200 });
            stream.write("partial");
            setTimeout(() => stream.close(http2.constants.NGHTTP2_INTERNAL_ERROR), 20);
          });
          await listen(errorServer);
          let bodyError = "body-resolved";
          try {
            const errorResponse = await fetchH2(origin(errorServer));
            try { await errorResponse.text(); }
            catch (error) { bodyError = "body-error"; }
          } catch (error) {
            bodyError = "fetch-error";
          }
          out.push(bodyError);
          await close(errorServer);

          globalThis.__h2StreamingResult = out.join("|");
        } catch (error) {
          globalThis.__h2StreamingError = String((error && error.stack) || error);
        }
        globalThis.__h2StreamingDone = 1;
      })();
    )MJS") };
    expect(streamingSetup.has_value(), "HTTP2 streaming regression script evaluates");
    if (haveHarness && streamingSetup.has_value()) {
        pump_event_loop("globalThis.__h2StreamingDone");
        const auto error { eval_to_string("globalThis.__h2StreamingError") };
        expect(error.has_value() && error->empty(),
               "HTTP2 streaming regression ran without harness errors: " + error.value_or("<eval failed>"));
        const auto result { eval_to_string("globalThis.__h2StreamingResult") };
        expect(result.has_value() && *result == "head|ab|null-bodies|redirect-ok|gzip|cancel|body-error",
               "HTTP2 fetch resolves on headers and streams decoded bodies (got '" + result.value_or("<eval failed>") + "')");
    }

    auto h1FramingSetup { eval(R"MJS(
      globalThis.__h1FramingDone = 0;
      globalThis.__h1FramingResult = "";
      globalThis.__h1FramingError = "";
      (async () => {
        try {
          const net = require("node:net");
          let wire = Buffer.alloc(0);
          const server = net.createServer((socket) => {
            socket.on("data", (chunk) => {
              wire = Buffer.concat([wire, chunk]);
              if (wire.indexOf("\r\n\r\n") !== -1) {
                socket.end("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok");
              }
            });
          });
          await new Promise((resolve, reject) => {
            server.once("error", reject);
            server.listen(0, "127.0.0.1", resolve);
          });
          const response = await fetch(`http://127.0.0.1:${server.address().port}/`, {
            method: "POST",
            headers: { "transfer-encoding": "chunked" },
          });
          const text = await response.text();
          await new Promise((resolve) => server.close(resolve));
          const split = wire.indexOf("\r\n\r\n");
          const head = wire.subarray(0, split).toString("latin1").toLowerCase();
          const body = wire.subarray(split + 4);
          globalThis.__h1FramingResult = text === "ok" &&
            !head.includes("transfer-encoding:") &&
            head.includes("content-length: 0") && body.length === 0
            ? "framed" : `bad:${head.replaceAll("\r\n", "|")}:body=${body.length}`;
        } catch (error) {
          globalThis.__h1FramingError = String((error && error.stack) || error);
        }
        globalThis.__h1FramingDone = 1;
      })();
    )MJS") };
    expect(h1FramingSetup.has_value(), "H1 request framing regression script evaluates");
    if (h1FramingSetup.has_value()) {
        pump_event_loop("globalThis.__h1FramingDone");
        const auto error { eval_to_string("globalThis.__h1FramingError") };
        expect(error.has_value() && error->empty(),
               "H1 request framing regression ran without harness errors: " + error.value_or("<eval failed>"));
        const auto result { eval_to_string("globalThis.__h1FramingResult") };
        expect(result.has_value() && *result == "framed",
               "H1 buffered request emits one coherent framing mode (got '" + result.value_or("<eval failed>") + "')");
    }
#endif

    if (gFailed != 0) {
        std::println("test_fetch_http2_regressions: {} failed", gFailed);
        return 1;
    }
    std::println("test_fetch_http2_regressions: ok");
    return 0;
}
