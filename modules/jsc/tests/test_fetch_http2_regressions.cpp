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

}  // namespace

int main() {
#if !defined(_WIN32)
    using namespace mbun::jsc::runtime;

    auto setup { eval(R"MJS(
      globalThis.__h2SecurityDone = 0;
      globalThis.__h2SecurityResult = "";
      globalThis.__h2SecurityError = "";
      (async () => {
        try {
          const http2 = require("node:http2");
          const fs = require("node:fs");
          const harnessPath = fs.realpathSync(process.cwd() + "/compat/bun/test/harness.ts");
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
    if (setup.has_value()) {
        pump_event_loop("globalThis.__h2SecurityDone");
        const auto error { eval_to_string("globalThis.__h2SecurityError") };
        expect(error.has_value() && error->empty(),
               "HTTP2 security regression ran without harness errors: " + error.value_or("<eval failed>"));
        const auto result { eval_to_string("globalThis.__h2SecurityResult") };
        expect(result.has_value() && *result == "strip|identity|mtls-ok",
               "HTTP2 redirect and TLS policies are enforced (got '" + result.value_or("<eval failed>") + "')");
    }
#endif

    if (gFailed != 0) {
        std::println("test_fetch_http2_regressions: {} failed", gFailed);
        return 1;
    }
    std::println("test_fetch_http2_regressions: ok");
    return 0;
}
