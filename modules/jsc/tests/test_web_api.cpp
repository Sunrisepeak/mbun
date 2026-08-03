// Tests mbun.jsc.web_api — a NOT-WIRED C++ state model. See the banner in
// modules/jsc/src/web_api.cppm: nothing but this file imports that module, so a
// pass here proves only that the model is self-consistent. It is NOT evidence
// about the Request/Response mbun ships (those are the JS classes in
// modules/jsc/src/builtins/{process_web,markdown_web}.cppm, verified
// differentially against .mbun/bin/bun-rust). Do not read "ok" below as
// coverage of the runtime — historically it was green on status validation the
// runtime did not have.
import std;
import mbun.jsc.web_api;

namespace {

int checks { 0 };
int failures { 0 };

void expect(bool condition, std::string_view message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::println("FAIL: {}", message);
    }
}

void test_request_defaults_and_clone() {
    auto requestResult { mbun::jsc::web::Request::create("https://example.test/data") };
    expect(requestResult.has_value(), "default Request is valid");
    if (!requestResult) return;
    auto& request { *requestResult };
    expect(request.url() == "https://example.test/data", "Request keeps URL");
    expect(request.method() == "GET", "Request defaults to GET");
    expect(request.headers().empty(), "Request starts with empty Headers");
    expect(request.clone().has_value(), "unused Request can be cloned");
}

void test_request_validation_and_body() {
    auto requestResult { mbun::jsc::web::Request::create(
        "https://example.test/upload",
        { .method = "post", .headers = { { "Content-Type", " text/plain\t" } }, .body = "payload" }) };
    expect(requestResult.has_value(), "lowercase known method is accepted");
    if (!requestResult) return;
    auto& request { *requestResult };
    expect(request.method() == "POST", "known method uses canonical wire form");
    expect(request.headers().get("content-type").value_or("") == "text/plain",
           "header name and value are normalized");
    expect(request.text().value_or("") == "payload", "Request text reads body");
    expect(request.body_used(), "Request marks body used");
    expect(!request.text().has_value(), "Request body is single-use");
    expect(!request.clone().has_value(), "used Request cannot be cloned");

    auto mixedMethod { mbun::jsc::web::Request::create("https://example.test", { .method = "pOsT" }) };
    expect(!mixedMethod.has_value(), "mixed-case method rejected like Bun Method::which");
    auto invalidHeader { mbun::jsc::web::Request::create(
        "https://example.test", { .headers = { { "Bad Header", "x" } } }) };
    expect(!invalidHeader.has_value(), "invalid header is propagated instead of dropped");
}

void test_response_contracts() {
    auto responseResult { mbun::jsc::web::Response::create(
        "hello",
        { .status = 201, .status_text = "Created", .headers = { { "X-Test", "yes" } } }) };
    expect(responseResult.has_value(), "valid Response is created");
    if (!responseResult) return;
    auto& response { *responseResult };
    expect(response.status() == 201, "Response keeps status");
    expect(response.status_text() == "Created", "Response keeps status text");
    expect(response.ok(), "2xx Response is ok");
    expect(response.headers().get("x-test").value_or("") == "yes", "Response has Headers");
    auto clone { response.clone() };
    expect(clone.has_value(), "unused Response can be cloned");
    expect(clone && clone->text().value_or("") == "hello", "clone owns independent body state");
    expect(response.text().value_or("") == "hello", "original remains readable");
    expect(!response.clone().has_value(), "consumed Response cannot be cloned");

    expect(mbun::jsc::web::Response::create(std::nullopt, { .status = 101 }).has_value(),
           "Bun's status 101 exception is accepted");
    expect(!mbun::jsc::web::Response::create(std::nullopt, { .status = 199 }).has_value(),
           "status below 200 is rejected");
    expect(!mbun::jsc::web::Response::create(std::nullopt, { .status = 600 }).has_value(),
           "status 600 is rejected");
    expect(!mbun::jsc::web::Response::create(
        std::nullopt, { .headers = { { "x-test", "ok\r\nbad: value" } } }).has_value(),
           "CRLF header injection is rejected");
}

} // namespace

int main() {
    test_request_defaults_and_clone();
    test_request_validation_and_body();
    test_response_contracts();
    std::println("web API seam (NOT WIRED — models only, not the shipped JS "
                 "Request/Response): {} checks, {} failures",
                 checks, failures);
    return failures == 0 ? 0 : 1;
}
