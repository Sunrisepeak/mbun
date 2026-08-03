// test_s3_signing.cpp — AWS Signature Version 4 known-answer vectors for
// mbun.s3_signing.
//
// Crypto KATs (SHA-256 / HMAC-SHA256 / SigV4 signing-key derivation) come from
// FIPS 180-4, RFC 4231, and the AWS "deriving a signing key" example. The
// end-to-end S3 sign/presign expectations (canonical request, Authorization
// header, presigned URL) were computed by an INDEPENDENT reference
// implementation (Python hashlib/hmac) mirroring the bun-ref Rust signer, then
// baked in verbatim — a genuine cross-implementation cross-check.

import std;
import mbun.s3_signing;
import mbun.s3_signing.backend;
import mbun.crypto;

namespace {

using namespace mbun::s3_signing;

int g_failed { 0 };

void check(std::string_view name, std::string_view got, std::string_view want) {
    if (got != want) {
        ++g_failed;
        std::println("FAIL {}:\n  got:  {}\n  want: {}", name, got, want);
    }
}

void check_ok(std::string_view name, bool cond) {
    if (!cond) {
        ++g_failed;
        std::println("FAIL {}", name);
    }
}

const SigningPrimitives& prims() {
    static const SigningPrimitives p { default_primitives() };
    return p;
}

// Fixed test identity (AWS documentation example credentials + date).
constexpr std::string_view AK { "AKIAIOSFODNN7EXAMPLE" };
constexpr std::string_view SK { "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY" };
constexpr std::string_view DATE { "20130524T000000Z" };

}  // namespace

int main() {
    // ── Crypto primitive KATs ─────────────────────────────────────────────
    {
        auto sha = prims().sha256;
        check("sha256(abc)", to_hex(sha("abc")),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        check("sha256(empty)", to_hex(sha("")),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        // RFC 4231 HMAC-SHA256 test case 2 (key "Jefe", data "what do ya want ...").
        check("hmac-sha256 rfc4231-2",
              to_hex(prims().hmac_sha256(bytes_("Jefe"), "what do ya want for nothing?")),
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
        // SigV4 signing-key derivation (AWS docs example: 20120215/us-east-1/iam).
        auto k = prims().hmac_sha256(bytes_("AWS4wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"), "20120215");
        k = prims().hmac_sha256(k, "us-east-1");
        k = prims().hmac_sha256(k, "iam");
        k = prims().hmac_sha256(k, "aws4_request");
        check("sigv4 signing-key derivation", to_hex(k),
              "004aa806e13dae88b9032d9261bcb04c67d023afadd221e6b0d206e1760e0b5e");
    }

    // ── A: header GET, path-style ─────────────────────────────────────────
    {
        Credentials c { .access_key_id = AK, .secret_access_key = SK, .region = "us-east-1", .bucket = "examplebucket" };
        SignOptions o { .path = "test.txt", .method = "GET" };
        auto r = sign_request(c, o, DATE, prims());
        check_ok("A ok", r.has_value());
        check("A canonical", r->canonical_request,
              "GET\n/examplebucket/test.txt\n\nhost:s3.us-east-1.amazonaws.com\n"
              "x-amz-content-sha256:UNSIGNED-PAYLOAD\nx-amz-date:20130524T000000Z\n\n"
              "host;x-amz-content-sha256;x-amz-date\nUNSIGNED-PAYLOAD");
        check("A authorization", r->authorization,
              "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request, "
              "SignedHeaders=host;x-amz-content-sha256;x-amz-date, "
              "Signature=cac8d06190b72754d7a1c4045065a6b19b87031fc96dac06c9ad402bcc8a6db6");
        check("A host", r->host, "s3.us-east-1.amazonaws.com");
        check("A url", r->url, "https://s3.us-east-1.amazonaws.com/examplebucket/test.txt");
    }

    // ── B: presigned GET ──────────────────────────────────────────────────
    {
        Credentials c { .access_key_id = AK, .secret_access_key = SK, .region = "us-east-1", .bucket = "examplebucket" };
        SignOptions o { .path = "test.txt", .method = "GET" };
        auto r = sign_request(c, o, DATE, prims(), SignQueryOptions { .expires = 86400 });
        check_ok("B ok", r.has_value());
        check("B url", r->url,
              "https://s3.us-east-1.amazonaws.com/examplebucket/test.txt?"
              "X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIAIOSFODNN7EXAMPLE%2F20130524%2Fus-east-1%2Fs3%2Faws4_request&"
              "X-Amz-Date=20130524T000000Z&X-Amz-Expires=86400&"
              "X-Amz-Signature=82a9e14d32bf328c615ee54f73419659fdf15ae409b9832190c98d88a00297d8&X-Amz-SignedHeaders=host");
    }

    // ── C: header PUT with acl + storage class + request-payer + session token ─
    {
        Credentials c { .access_key_id = AK, .secret_access_key = SK, .region = "us-east-1",
                        .bucket = "examplebucket", .session_token = "FQoGZTOKEN" };
        SignOptions o { .path = "dir/obj.bin", .method = "PUT", .acl = ACL::PublicRead,
                        .storage_class = StorageClass::StandardIa, .request_payer = true };
        auto r = sign_request(c, o, DATE, prims());
        check_ok("C ok", r.has_value());
        check("C canonical", r->canonical_request,
              "PUT\n/examplebucket/dir/obj.bin\n\nhost:s3.us-east-1.amazonaws.com\nx-amz-acl:public-read\n"
              "x-amz-content-sha256:UNSIGNED-PAYLOAD\nx-amz-date:20130524T000000Z\nx-amz-request-payer:requester\n"
              "x-amz-security-token:FQoGZTOKEN\nx-amz-storage-class:STANDARD_IA\n\n"
              "host;x-amz-acl;x-amz-content-sha256;x-amz-date;x-amz-request-payer;x-amz-security-token;x-amz-storage-class\n"
              "UNSIGNED-PAYLOAD");
        check("C authorization", r->authorization,
              "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request, "
              "SignedHeaders=host;x-amz-acl;x-amz-content-sha256;x-amz-date;x-amz-request-payer;x-amz-security-token;x-amz-storage-class, "
              "Signature=eb1bd8ce9e2e1473ea0137d2322fc169ff77e6b465f36e62e793f6f31d03f809");
    }

    // ── D: presigned with content-md5 + response-content-* + session token ─
    {
        Credentials c { .access_key_id = AK, .secret_access_key = SK, .region = "us-east-1",
                        .bucket = "examplebucket", .session_token = "sess+tok/en" };
        // Raw MD5 of "abc" = 900150983cd24fb0d6963f7d28e17f72.
        const std::string md5_raw {
            "\x90\x01\x50\x98\x3c\xd2\x4f\xb0\xd6\x96\x3f\x7d\x28\xe1\x7f\x72", 16 };
        SignOptions o { .path = "my key.txt", .method = "GET",
                        .content_type = "text/plain; charset=utf-8",
                        .content_md5 = md5_raw };
        o.content_disposition = "attachment; filename=\"x.txt\"";
        auto r = sign_request(c, o, DATE, prims(), SignQueryOptions { .expires = 3600 });
        check_ok("D ok", r.has_value());
        check("D url", r->url,
              "https://s3.us-east-1.amazonaws.com/examplebucket/my%20key.txt?"
              "Content-MD5=kAFQmDzST7DWlj99KOF%2Fcg%3D%3D&X-Amz-Algorithm=AWS4-HMAC-SHA256&"
              "X-Amz-Credential=AKIAIOSFODNN7EXAMPLE%2F20130524%2Fus-east-1%2Fs3%2Faws4_request&"
              "X-Amz-Date=20130524T000000Z&X-Amz-Expires=3600&X-Amz-Security-Token=sess%2Btok%2Fen&"
              "X-Amz-Signature=cc108900feaacca15fc7442cbc06121b4da3dd477a782a147fda28643a51928e&"
              "X-Amz-SignedHeaders=host&response-content-disposition=attachment%3B%20filename%3D%22x.txt%22&"
              "response-content-type=text%2Fplain%3B%20charset%3Dutf-8");
    }

    // ── E: virtual hosted style ───────────────────────────────────────────
    {
        Credentials c { .access_key_id = AK, .secret_access_key = SK, .region = "us-east-1",
                        .bucket = "examplebucket", .virtual_hosted_style = true };
        SignOptions o { .path = "test.txt", .method = "GET" };
        auto r = sign_request(c, o, DATE, prims());
        check_ok("E ok", r.has_value());
        check("E host", r->host, "examplebucket.s3.us-east-1.amazonaws.com");
        check("E authorization", r->authorization,
              "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request, "
              "SignedHeaders=host;x-amz-content-sha256;x-amz-date, "
              "Signature=071e0fbbb6cf12c8e611e36ed9ee45a49c5fbf50952551d1b46ceff59cc61b56");
    }

    // ── F: content-disposition + content-encoding + search params ─────────
    {
        Credentials c { .access_key_id = AK, .secret_access_key = SK, .region = "us-east-1", .bucket = "examplebucket" };
        SignOptions o { .path = "test.txt", .method = "GET", .search_params = "?versionId=abc",
                        .content_disposition = "inline", .content_encoding = "gzip" };
        auto r = sign_request(c, o, DATE, prims());
        check_ok("F ok", r.has_value());
        check("F canonical", r->canonical_request,
              "GET\n/examplebucket/test.txt\nversionId=abc\ncontent-disposition:inline\ncontent-encoding:gzip\n"
              "host:s3.us-east-1.amazonaws.com\nx-amz-content-sha256:UNSIGNED-PAYLOAD\nx-amz-date:20130524T000000Z\n\n"
              "content-disposition;content-encoding;host;x-amz-content-sha256;x-amz-date\nUNSIGNED-PAYLOAD");
        check("F authorization", r->authorization,
              "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request, "
              "SignedHeaders=content-disposition;content-encoding;host;x-amz-content-sha256;x-amz-date, "
              "Signature=a48490d109f3e5761cb75b11835ddc0261b31e3edf6ae378f4e46a4efe0f40b9");
        check("F url", r->url, "https://s3.us-east-1.amazonaws.com/examplebucket/test.txt?versionId=abc");
    }

    // ── G: endpoint-based (MinIO-like), region guessed, insecure http ─────
    {
        Credentials c { .access_key_id = AK, .secret_access_key = SK, .endpoint = "play.min.io:9000",
                        .bucket = "mybucket", .insecure_http = true };
        SignOptions o { .path = "obj", .method = "GET" };
        auto r = sign_request(c, o, DATE, prims());
        check_ok("G ok", r.has_value());
        check("G host", r->host, "play.min.io:9000");
        check("G authorization", r->authorization,
              "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/auto/s3/aws4_request, "
              "SignedHeaders=host;x-amz-content-sha256;x-amz-date, "
              "Signature=484754456567937e7747bec3498b0fc0b26454f71fd9abebd67451420e7e0e44");
        check("G url", r->url, "http://play.min.io:9000/mybucket/obj");
    }

    // ── H: bucket guessed from the path (path-style, empty credentials.bucket) ─
    {
        Credentials c { .access_key_id = AK, .secret_access_key = SK, .region = "us-east-1" };
        SignOptions o { .path = "examplebucket/test.txt", .method = "GET" };
        auto r = sign_request(c, o, DATE, prims());
        check_ok("H ok", r.has_value());
        // Identical to case A once bucket is split out of the path.
        check("H authorization", r->authorization,
              "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request, "
              "SignedHeaders=host;x-amz-content-sha256;x-amz-date, "
              "Signature=cac8d06190b72754d7a1c4045065a6b19b87031fc96dac06c9ad402bcc8a6db6");
    }

    // ── Boundary / error cases ────────────────────────────────────────────
    {
        // Missing credentials.
        Credentials c0 { .region = "us-east-1", .bucket = "b" };
        SignOptions o0 { .path = "k", .method = "GET" };
        auto r0 = sign_request(c0, o0, DATE, prims());
        check_ok("missing creds", !r0 && r0.error() == SignError::MissingCredentials);

        Credentials c { .access_key_id = AK, .secret_access_key = SK, .region = "us-east-1", .bucket = "b" };
        // Invalid method.
        auto r1 = sign_request(c, SignOptions { .path = "k", .method = "PATCH" }, DATE, prims());
        check_ok("invalid method", !r1 && r1.error() == SignError::InvalidMethod);
        // Empty path.
        auto r2 = sign_request(c, SignOptions { .path = "///", .method = "GET" }, DATE, prims());
        check_ok("empty path", !r2 && r2.error() == SignError::InvalidPath);
        // Bad date.
        auto r3 = sign_request(c, SignOptions { .path = "k", .method = "GET" }, "2013-05-24", prims());
        check_ok("invalid date", !r3 && r3.error() == SignError::InvalidDate);
        // CRLF header injection via search params (header mode).
        auto r4 = sign_request(c, SignOptions { .path = "k", .method = "GET", .search_params = "?a=b\r\nHost: evil" }, DATE, prims());
        check_ok("crlf injection", !r4 && r4.error() == SignError::InvalidHeaderValue);
        // Deferred crypto (no primitives).
        auto r5 = sign_request(c, SignOptions { .path = "k", .method = "GET" }, DATE, SigningPrimitives {});
        check_ok("deferred crypto", !r5 && r5.error() == SignError::DeferredCrypto);
        // Virtual-hosted style with bucket containing '/' is rejected (host injection guard).
        Credentials cv { .access_key_id = AK, .secret_access_key = SK, .region = "us-east-1",
                         .bucket = "bad/bucket", .virtual_hosted_style = true };
        auto r6 = sign_request(cv, SignOptions { .path = "k", .method = "GET" }, DATE, prims());
        check_ok("vhost slash bucket", !r6 && r6.error() == SignError::InvalidEndpoint);
        // All valid HTTP methods accepted.
        for (auto m : { "GET", "PUT", "POST", "DELETE", "HEAD" }) {
            auto rm = sign_request(c, SignOptions { .path = "k", .method = m }, DATE, prims());
            check_ok(std::string { "method " } + m, rm.has_value());
        }
    }

    if (g_failed == 0) {
        std::println("all s3_signing vectors passed");
        return 0;
    }
    std::println("{} s3_signing checks FAILED", g_failed);
    return 1;
}
