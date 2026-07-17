// mbun.s3_signing — pure AWS Signature Version 4 signing for S3.
//
// References: bun-ref/src/s3_signing/{credentials,acl,storage_class}.rs (the
// Rust rewrite of bun-v1.3.14's Zig S3 signer). The API deliberately stops at
// signed request data; transport and JavaScript bindings belong to higher
// layers. Crypto (sha256/hmac) is injected via SigningPrimitives so this core
// carries no dependency; mbun.s3_signing.backend wires the real mbun.crypto.
export module mbun.s3_signing;

import std;

export namespace mbun::s3_signing {

enum class ACL {
    Private,
    PublicRead,
    PublicReadWrite,
    AwsExecRead,
    AuthenticatedRead,
    BucketOwnerRead,
    BucketOwnerFullControl,
    LogDeliveryWrite,
};

inline std::string_view to_string(ACL value) {
    constexpr std::array<std::string_view, 8> names {
        "private", "public-read", "public-read-write", "aws-exec-read",
        "authenticated-read", "bucket-owner-read", "bucket-owner-full-control",
        "log-delivery-write",
    };
    return names[static_cast<std::size_t>(value)];
}

enum class StorageClass {
    Standard,
    StandardIa,
    IntelligentTiering,
    ExpressOnezone,
    OnezoneIa,
    Glacier,
    GlacierIr,
    ReducedRedundancy,
    Outposts,
    DeepArchive,
    Snow,
};

inline std::string_view to_string(StorageClass value) {
    constexpr std::array<std::string_view, 11> names {
        "STANDARD", "STANDARD_IA", "INTELLIGENT_TIERING", "EXPRESS_ONEZONE",
        "ONEZONE_IA", "GLACIER", "GLACIER_IR", "REDUCED_REDUNDANCY", "OUTPOSTS",
        "DEEP_ARCHIVE", "SNOW",
    };
    return names[static_cast<std::size_t>(value)];
}

struct Credentials {
    std::string_view access_key_id;
    std::string_view secret_access_key;
    std::string_view region;
    std::string_view endpoint;
    std::string_view bucket;
    std::string_view session_token;
    bool insecure_http { false };
    bool virtual_hosted_style { false };
};

struct SignOptions {
    std::string_view path;
    std::string_view method { "GET" };
    std::string_view content_hash { "UNSIGNED-PAYLOAD" };
    std::string_view search_params;
    std::string_view content_disposition;
    std::string_view content_type;
    std::string_view content_encoding;
    // Raw (un-encoded) bytes; base64-encoded internally per SigV4.
    std::string_view content_md5;
    std::optional<ACL> acl;
    std::optional<StorageClass> storage_class;
    bool request_payer { false };
    // bun sign_request<ALLOW_EMPTY_PATH>: ListObjects signs the bucket root with
    // an empty key ("/bucket/"); every other op requires a non-empty key.
    bool allow_empty_path { false };
};

struct SignQueryOptions {
    std::uint32_t expires { 86400 };
};

enum class SignError {
    MissingCredentials,
    InvalidMethod,
    InvalidPath,
    InvalidEndpoint,
    InvalidSessionToken,
    InvalidHeaderValue,
    InvalidDate,
    DeferredCrypto,
};

struct SignResult {
    std::string amz_date;
    std::string host;
    std::string canonical_request;
    std::string authorization;
    std::string url;
    std::string signed_headers;
};

inline std::string_view error_name(SignError error) {
    constexpr std::array<std::string_view, 8> names {
        "MissingCredentials", "InvalidMethod", "InvalidPath", "InvalidEndpoint",
        "InvalidSessionToken", "InvalidHeaderValue", "InvalidDate", "DeferredCrypto",
    };
    return names[static_cast<std::size_t>(error)];
}

inline std::string to_hex(std::span<const std::uint8_t> bytes) {
    static constexpr char digits[] { "0123456789abcdef" };
    std::string result;
    result.resize(bytes.size() * 2);
    for (std::size_t i { 0 }; i < bytes.size(); ++i) {
        result[i * 2] = digits[bytes[i] >> 4];
        result[i * 2 + 1] = digits[bytes[i] & 0x0f];
    }
    return result;
}

inline std::span<const std::uint8_t> bytes_(std::string_view value) {
    return { reinterpret_cast<const std::uint8_t*>(value.data()), value.size() };
}

// Standard RFC 4648 base64 with padding.
inline std::string base64_encode(std::string_view input) {
    static constexpr char table[] { "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/" };
    std::string out;
    out.reserve((input.size() + 2) / 3 * 4);
    std::size_t i { 0 };
    const auto* data = reinterpret_cast<const unsigned char*>(input.data());
    for (; i + 3 <= input.size(); i += 3) {
        const std::uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(table[(n >> 18) & 0x3f]);
        out.push_back(table[(n >> 12) & 0x3f]);
        out.push_back(table[(n >> 6) & 0x3f]);
        out.push_back(table[n & 0x3f]);
    }
    const std::size_t rem { input.size() - i };
    if (rem == 1) {
        const std::uint32_t n = data[i] << 16;
        out.push_back(table[(n >> 18) & 0x3f]);
        out.push_back(table[(n >> 12) & 0x3f]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        const std::uint32_t n = (data[i] << 16) | (data[i + 1] << 8);
        out.push_back(table[(n >> 18) & 0x3f]);
        out.push_back(table[(n >> 12) & 0x3f]);
        out.push_back(table[(n >> 6) & 0x3f]);
        out.push_back('=');
    }
    return out;
}

using Digest256 = std::array<std::uint8_t, 32>;
using Sha256Fn = std::function<Digest256(std::string_view)>;
using HmacSha256Fn = std::function<Digest256(std::span<const std::uint8_t>, std::string_view)>;

struct SigningPrimitives {
    Sha256Fn sha256;
    HmacSha256Fn hmac_sha256;

    bool ready() const { return static_cast<bool>(sha256) && static_cast<bool>(hmac_sha256); }
};

inline std::string encode_uri_component(std::string_view input, bool encode_slash) {
    static constexpr char hex[] { "0123456789ABCDEF" };
    std::string result;
    result.reserve(input.size());
    for (unsigned char ch : input) {
        const bool unreserved = std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~';
        if (unreserved || (!encode_slash && (ch == '/' || ch == '\\'))) {
            result.push_back(ch == '\\' ? '/' : static_cast<char>(ch));
        } else {
            result.push_back('%');
            result.push_back(hex[ch >> 4]);
            result.push_back(hex[ch & 0x0f]);
        }
    }
    return result;
}

inline std::string trim_slashes_(std::string_view value) {
    while (!value.empty() && (value.front() == '/' || value.front() == '\\')) { value.remove_prefix(1); }
    while (!value.empty() && (value.back() == '/' || value.back() == '\\')) { value.remove_suffix(1); }
    return std::string { value };
}

inline bool contains_crlf_(std::string_view value) {
    return value.find_first_of("\r\n") != std::string_view::npos;
}

inline bool valid_host_(std::string_view value) {
    return !value.empty() && std::ranges::all_of(value, [](unsigned char ch) {
        return std::isalnum(ch) || ch == '-' || ch == '.' || ch == '_';
    });
}

// SigV4 signing region derivation from the endpoint host. 1:1 with bun
// src/s3_signing/credentials.rs guess_region: for an "s3.<region>.amazonaws.com"
// host the region is the slice between "s3." and ".amazonaws.com"; a region-less
// "s3.amazonaws.com" (start+3 > end) falls back to us-east-1, any other informed
// endpoint to "auto", and an empty endpoint to us-east-1.
inline std::string_view guess_region(std::string_view endpoint) {
    if (!endpoint.empty()) {
        if (endpoint.ends_with(".r2.cloudflarestorage.com")) { return "auto"; }
        const auto amazon = endpoint.find(".amazonaws.com");
        if (amazon != std::string_view::npos) {
            const auto s3 = endpoint.find("s3.");
            if (s3 != std::string_view::npos) {
                if (s3 + 3 <= amazon) { return endpoint.substr(s3 + 3, amazon - (s3 + 3)); }
                return "us-east-1";
            }
        }
        return "auto";
    }
    return "us-east-1";
}

// signed_headers list, alphabetical per SigV4 (canonical order).
inline std::string signed_headers_(const SignOptions& options, bool has_md5, bool has_token) {
    std::string result;
    if (!options.content_disposition.empty()) { result += "content-disposition;"; }
    if (!options.content_encoding.empty()) { result += "content-encoding;"; }
    if (has_md5) { result += "content-md5;"; }
    result += "host;";
    if (options.acl) { result += "x-amz-acl;"; }
    result += "x-amz-content-sha256;x-amz-date";
    if (options.request_payer) { result += ";x-amz-request-payer"; }
    if (has_token) { result += ";x-amz-security-token"; }
    if (options.storage_class) { result += ";x-amz-storage-class"; }
    return result;
}

inline std::string canonical_request_headers_(const SignOptions& options, std::string_view path,
                                              std::string_view query, std::string_view host,
                                              std::string_view date, std::string_view content_md5,
                                              std::string_view session_token,
                                              std::string_view signed_headers) {
    std::string r;
    r.reserve(256 + path.size() + query.size());
    r += options.method;
    r += '\n';
    r += path;
    r += '\n';
    r += query;
    r += '\n';
    if (!options.content_disposition.empty()) { r += "content-disposition:"; r += options.content_disposition; r += '\n'; }
    if (!options.content_encoding.empty()) { r += "content-encoding:"; r += options.content_encoding; r += '\n'; }
    if (!content_md5.empty()) { r += "content-md5:"; r += content_md5; r += '\n'; }
    r += "host:"; r += host; r += '\n';
    if (options.acl) { r += "x-amz-acl:"; r += to_string(*options.acl); r += '\n'; }
    r += "x-amz-content-sha256:"; r += options.content_hash; r += '\n';
    r += "x-amz-date:"; r += date; r += '\n';
    if (options.request_payer) { r += "x-amz-request-payer:requester\n"; }
    if (!session_token.empty()) { r += "x-amz-security-token:"; r += session_token; r += '\n'; }
    if (options.storage_class) { r += "x-amz-storage-class:"; r += to_string(*options.storage_class); r += '\n'; }
    r += '\n';
    r += signed_headers;
    r += '\n';
    r += options.content_hash;
    return r;
}

inline std::expected<SignResult, SignError> sign_request(const Credentials& credentials,
                                                          const SignOptions& options,
                                                          std::string_view amz_date,
                                                          const SigningPrimitives& primitives,
                                                          std::optional<SignQueryOptions> query_options = std::nullopt) {
    if (!primitives.ready()) { return std::unexpected(SignError::DeferredCrypto); }
    if (credentials.access_key_id.empty() || credentials.secret_access_key.empty()) {
        return std::unexpected(SignError::MissingCredentials);
    }
    if (options.method != "GET" && options.method != "PUT" && options.method != "DELETE" &&
        options.method != "HEAD" && options.method != "POST") {
        return std::unexpected(SignError::InvalidMethod);
    }
    if (amz_date.size() != 16 || amz_date[8] != 'T' || amz_date.back() != 'Z') {
        return std::unexpected(SignError::InvalidDate);
    }
    const bool sign_query = query_options.has_value();
    const bool has_token = !credentials.session_token.empty();

    // content-md5: base64-encode the raw bytes (SigV4 requirement).
    std::string content_md5;
    if (!options.content_md5.empty()) { content_md5 = base64_encode(options.content_md5); }
    const bool has_md5 = !content_md5.empty();

    std::string region { credentials.region.empty() ? std::string { guess_region(credentials.endpoint) }
                                                     : std::string { credentials.region } };

    // Split bucket / key out of the path when the bucket is not given (path-style).
    std::string full_path { options.path };
    if (!full_path.empty() && (full_path.front() == '/' || full_path.front() == '\\')) {
        full_path.erase(full_path.begin());
    }
    std::string bucket_src { credentials.bucket };
    std::string key_src { full_path };
    if (!credentials.virtual_hosted_style && bucket_src.empty()) {
        auto slash = full_path.find_first_of("/\\");
        if (slash == std::string::npos) { return std::unexpected(SignError::InvalidPath); }
        bucket_src = full_path.substr(0, slash);
        key_src = full_path.substr(slash + 1);
    }
    std::string bucket { trim_slashes_(bucket_src) };
    std::string key { trim_slashes_(key_src) };
    if (key.empty() && !options.allow_empty_path) { return std::unexpected(SignError::InvalidPath); }

    std::string encoded_bucket { encode_uri_component(bucket, false) };
    std::string encoded_key { encode_uri_component(key, false) };

    std::string host;
    std::string extra_path;
    if (!credentials.endpoint.empty()) {
        if (credentials.endpoint.size() >= 2048) { return std::unexpected(SignError::InvalidEndpoint); }
        auto slash = credentials.endpoint.find('/');
        host = std::string { credentials.endpoint.substr(0, slash) };
        if (slash != std::string_view::npos) { extra_path = std::string { credentials.endpoint.substr(slash) }; }
    } else {
        if (!valid_host_(region)) { return std::unexpected(SignError::InvalidEndpoint); }
        if (credentials.virtual_hosted_style) {
            if (encoded_bucket.empty() || encoded_bucket.find('/') != std::string::npos) {
                return std::unexpected(SignError::InvalidEndpoint);
            }
            host = encoded_bucket + ".s3." + region + ".amazonaws.com";
        } else {
            host = "s3." + region + ".amazonaws.com";
        }
    }
    if (host.empty()) { return std::unexpected(SignError::InvalidEndpoint); }

    std::string request_path { extra_path };
    request_path += '/';
    if (!credentials.virtual_hosted_style) { request_path += encoded_bucket + '/'; }
    request_path += encoded_key;

    const std::string day { amz_date.substr(0, 8) };
    const std::string scope = day + '/' + region + "/s3/aws4_request";
    const std::string protocol = credentials.insecure_http ? "http://" : "https://";

    // Derive the SigV4 signing key: k_signing = HMAC(HMAC(HMAC(HMAC("AWS4"+secret, day), region), "s3"), "aws4_request").
    const std::string aws4_secret = "AWS4" + std::string { credentials.secret_access_key };
    const Digest256 k_date { primitives.hmac_sha256(bytes_(aws4_secret), day) };
    const Digest256 k_region { primitives.hmac_sha256(k_date, region) };
    const Digest256 k_service { primitives.hmac_sha256(k_region, "s3") };
    const Digest256 signing_key { primitives.hmac_sha256(k_service, "aws4_request") };

    SignResult result { .amz_date = std::string { amz_date }, .host = host };

    if (sign_query) {
        // Presigned URL: only `host` is signed; everything else rides in the query.
        std::string encoded_token { has_token ? encode_uri_component(credentials.session_token, true) : std::string {} };
        std::string encoded_md5 { has_md5 ? encode_uri_component(content_md5, true) : std::string {} };
        std::string encoded_cd { options.content_disposition.empty() ? std::string {}
                                                                      : encode_uri_component(options.content_disposition, true) };
        std::string encoded_ct { options.content_type.empty() ? std::string {}
                                                               : encode_uri_component(options.content_type, true) };
        const std::string credential = std::string { credentials.access_key_id } + "%2F" + day + "%2F" + region + "%2Fs3%2Faws4_request";

        // Canonical query parameters, alphabetical (byte) order.
        auto build_query = [&](std::string_view signature) {
            std::string q;
            auto add = [&](std::string_view kv) { if (!q.empty()) { q += '&'; } q += kv; };
            if (has_md5) { add("Content-MD5=" + encoded_md5); }
            if (options.acl) { add(std::string { "X-Amz-Acl=" } + std::string { to_string(*options.acl) }); }
            add("X-Amz-Algorithm=AWS4-HMAC-SHA256");
            add("X-Amz-Credential=" + credential);
            add("X-Amz-Date=" + std::string { amz_date });
            add("X-Amz-Expires=" + std::to_string(query_options->expires));
            if (has_token) { add("X-Amz-Security-Token=" + encoded_token); }
            if (!signature.empty()) { add("X-Amz-Signature=" + std::string { signature }); }
            add("X-Amz-SignedHeaders=host");
            if (!encoded_cd.empty()) { add("response-content-disposition=" + encoded_cd); }
            if (!encoded_ct.empty()) { add("response-content-type=" + encoded_ct); }
            if (options.request_payer) { add("x-amz-request-payer=requester"); }
            if (options.storage_class) { add(std::string { "x-amz-storage-class=" } + std::string { to_string(*options.storage_class) }); }
            return q;
        };

        const std::string canonical_query = build_query({});
        std::string canonical;
        canonical.reserve(128 + request_path.size() + canonical_query.size());
        canonical += options.method; canonical += '\n';
        canonical += request_path; canonical += '\n';
        canonical += canonical_query; canonical += '\n';
        canonical += "host:"; canonical += host; canonical += "\n\nhost\n";
        canonical += options.content_hash;

        const std::string canonical_hash = to_hex(primitives.sha256(canonical));
        const std::string string_to_sign = "AWS4-HMAC-SHA256\n" + std::string { amz_date } + '\n' + scope + '\n' + canonical_hash;
        const std::string signature = to_hex(primitives.hmac_sha256(signing_key, string_to_sign));

        result.canonical_request = canonical;
        result.signed_headers = "host";
        result.url = protocol + host + request_path + '?' + build_query(signature);
        result.authorization = result.url;
        return result;
    }

    // Header authorization mode.
    if (contains_crlf_(options.content_hash) || contains_crlf_(options.search_params) ||
        contains_crlf_(options.content_disposition) || contains_crlf_(options.content_encoding) ||
        contains_crlf_(options.content_type) || contains_crlf_(content_md5) ||
        contains_crlf_(credentials.region) || contains_crlf_(credentials.access_key_id) ||
        contains_crlf_(credentials.session_token) || contains_crlf_(host)) {
        return std::unexpected(SignError::InvalidHeaderValue);
    }

    std::string query;
    if (!options.search_params.empty()) {
        query = options.search_params.front() == '?' ? std::string { options.search_params.substr(1) }
                                                      : std::string { options.search_params };
    }
    const std::string signed_headers { signed_headers_(options, has_md5, has_token) };
    const std::string canonical = canonical_request_headers_(options, request_path, query, host, amz_date,
                                                             content_md5, credentials.session_token, signed_headers);
    const std::string canonical_hash = to_hex(primitives.sha256(canonical));
    const std::string string_to_sign = "AWS4-HMAC-SHA256\n" + std::string { amz_date } + '\n' + scope + '\n' + canonical_hash;
    const std::string signature = to_hex(primitives.hmac_sha256(signing_key, string_to_sign));

    result.canonical_request = canonical;
    result.signed_headers = signed_headers;
    result.authorization = "AWS4-HMAC-SHA256 Credential=" + std::string { credentials.access_key_id } + '/' + scope +
                           ", SignedHeaders=" + signed_headers + ", Signature=" + signature;
    result.url = protocol + host + request_path + std::string { options.search_params };
    return result;
}

}  // namespace mbun::s3_signing
