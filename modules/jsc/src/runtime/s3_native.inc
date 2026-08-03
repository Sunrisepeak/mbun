// ── __mbunS3Native: AWS SigV4 request signing for Bun.S3Client ──────────────
// PORT-SOURCE: bun src/s3_signing/{credentials,acl,storage_class,error}.rs +
// src/runtime/webcore/s3/credentials_jsc.rs (sign / signQuery). The pure signing
// algorithm lives in mbun.s3_signing (a 1:1 translation of bun's Rust rewrite of
// the original Zig signer); this bridge only marshals a JS options bag into the
// Credentials/SignOptions structs, calls sign_request with the mbun.crypto-backed
// primitives, and returns the signed request data. Transport (node:http/https)
// and the S3Client/S3File JS surface live one layer up (builtins/s3.cppm).

namespace {

// Read a string property; undefined/null → empty string (bun treats empty and
// unset credentials/options identically at the signing boundary).
std::string s3_prop_str(JSContextRef ctx, JSObjectRef o, const char* name) {
    JSValueRef v{get_prop(ctx, o, name)};
    if (v == nullptr || JSValueIsUndefined(ctx, v) || JSValueIsNull(ctx, v)) return {};
    return val_to_string(ctx, v);
}

bool s3_prop_bool(JSContextRef ctx, JSObjectRef o, const char* name) {
    JSValueRef v{get_prop(ctx, o, name)};
    if (v == nullptr) return false;
    return JSValueToBoolean(ctx, v);
}

double s3_prop_num(JSContextRef ctx, JSObjectRef o, const char* name) {
    JSValueRef v{get_prop(ctx, o, name)};
    if (v == nullptr || JSValueIsUndefined(ctx, v) || JSValueIsNull(ctx, v)) return 0;
    return JSValueToNumber(ctx, v, nullptr);
}

std::optional<mbun::s3_signing::ACL> s3_parse_acl(std::string_view s) {
    using A = mbun::s3_signing::ACL;
    for (int i{0}; i < 8; ++i) {
        if (mbun::s3_signing::to_string(static_cast<A>(i)) == s) return static_cast<A>(i);
    }
    return std::nullopt;
}

std::optional<mbun::s3_signing::StorageClass> s3_parse_storage_class(std::string_view s) {
    using S = mbun::s3_signing::StorageClass;
    for (int i{0}; i < 11; ++i) {
        if (mbun::s3_signing::to_string(static_cast<S>(i)) == s) return static_cast<S>(i);
    }
    return std::nullopt;
}

// ERR_S3_* code for a SignError, matching bun s3_signing/error.rs
// get_sign_error_code_and_message.
const char* s3_error_code(mbun::s3_signing::SignError e) {
    using E = mbun::s3_signing::SignError;
    switch (e) {
        case E::MissingCredentials: return "ERR_S3_MISSING_CREDENTIALS";
        case E::InvalidMethod:      return "ERR_S3_INVALID_METHOD";
        case E::InvalidPath:        return "ERR_S3_INVALID_PATH";
        case E::InvalidEndpoint:    return "ERR_S3_INVALID_ENDPOINT";
        case E::InvalidSessionToken:return "ERR_S3_INVALID_SESSION_TOKEN";
        default:                    return "ERR_S3_INVALID_SIGNATURE";
    }
}

const char* s3_error_message(mbun::s3_signing::SignError e) {
    using E = mbun::s3_signing::SignError;
    switch (e) {
        case E::MissingCredentials:
            return "Missing S3 credentials. 'accessKeyId', 'secretAccessKey', "
                   "'bucket', and 'endpoint' are required";
        case E::InvalidMethod:
            return "Method must be GET, PUT, DELETE or HEAD when using s3:// protocol";
        case E::InvalidPath:      return "Invalid S3 bucket, key combination";
        case E::InvalidEndpoint:  return "Invalid S3 endpoint";
        case E::InvalidSessionToken: return "Invalid session token";
        default: return "Failed to retrieve S3 content. Are the credentials correct?";
    }
}

}  // namespace

// __mbunS3Native.sign(options) -> { url, authorization, host, amzDate, signedHeaders }
// options is a plain object; see builtins/s3.cppm __mbunS3Native.sign callers for
// the field contract. On a signing failure this throws an Error whose `.code` is
// the matching ERR_S3_* string (bun surfaces these verbatim to user code).
JSValueRef s3_sign_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                      const JSValueRef args[], JSValueRef* exception) {
    namespace s3 = mbun::s3_signing;
    if (argc < 1 || !JSValueIsObject(ctx, args[0])) {
        if (exception != nullptr) {
            JSValueRef m{make_string(ctx, "S3 sign: options object required")};
            *exception = JSObjectMakeError(ctx, 1, &m, nullptr);
        }
        return JSValueMakeUndefined(ctx);
    }
    JSObjectRef o{JSValueToObject(ctx, args[0], nullptr)};

    // Own every backing string so the string_views handed to sign_request stay
    // valid for the whole call.
    const std::string access_key{s3_prop_str(ctx, o, "accessKeyId")};
    const std::string secret_key{s3_prop_str(ctx, o, "secretAccessKey")};
    const std::string region{s3_prop_str(ctx, o, "region")};
    const std::string endpoint{s3_prop_str(ctx, o, "endpoint")};
    const std::string bucket{s3_prop_str(ctx, o, "bucket")};
    const std::string session_token{s3_prop_str(ctx, o, "sessionToken")};
    const std::string path{s3_prop_str(ctx, o, "path")};
    const std::string method{s3_prop_str(ctx, o, "method")};
    const std::string content_hash{s3_prop_str(ctx, o, "contentHash")};
    const std::string search_params{s3_prop_str(ctx, o, "searchParams")};
    const std::string content_type{s3_prop_str(ctx, o, "contentType")};
    const std::string content_disposition{s3_prop_str(ctx, o, "contentDisposition")};
    const std::string content_encoding{s3_prop_str(ctx, o, "contentEncoding")};
    const std::string content_md5{s3_prop_str(ctx, o, "contentMd5")};
    const std::string acl_str{s3_prop_str(ctx, o, "acl")};
    const std::string storage_class_str{s3_prop_str(ctx, o, "storageClass")};
    const std::string amz_date{s3_prop_str(ctx, o, "amzDate")};

    s3::Credentials cred{
        .access_key_id = access_key,
        .secret_access_key = secret_key,
        .region = region,
        .endpoint = endpoint,
        .bucket = bucket,
        .session_token = session_token,
        .insecure_http = s3_prop_bool(ctx, o, "insecureHttp"),
        .virtual_hosted_style = s3_prop_bool(ctx, o, "virtualHostedStyle"),
    };

    s3::SignOptions opts{};
    opts.path = path;
    opts.method = method.empty() ? std::string_view{"GET"} : std::string_view{method};
    opts.content_hash = content_hash.empty() ? std::string_view{"UNSIGNED-PAYLOAD"}
                                             : std::string_view{content_hash};
    opts.search_params = search_params;
    opts.content_disposition = content_disposition;
    opts.content_type = content_type;
    opts.content_encoding = content_encoding;
    opts.content_md5 = content_md5;
    if (!acl_str.empty()) opts.acl = s3_parse_acl(acl_str);
    if (!storage_class_str.empty()) opts.storage_class = s3_parse_storage_class(storage_class_str);
    opts.request_payer = s3_prop_bool(ctx, o, "requestPayer");
    opts.allow_empty_path = s3_prop_bool(ctx, o, "allowEmptyPath");

    std::optional<s3::SignQueryOptions> query{};
    const double expires{s3_prop_num(ctx, o, "expires")};
    if (expires > 0) { query = s3::SignQueryOptions{.expires = static_cast<std::uint32_t>(expires)}; }

    auto result{s3::sign_request(cred, opts, amz_date, s3::default_primitives(), query)};
    if (!result) {
        if (exception != nullptr) {
            JSValueRef m{make_string(ctx, s3_error_message(result.error()))};
            JSObjectRef err{JSObjectMakeError(ctx, 1, &m, nullptr)};
            set_prop(ctx, err, "code", make_string(ctx, s3_error_code(result.error())));
            *exception = err;
        }
        return JSValueMakeUndefined(ctx);
    }

    JSObjectRef out{JSObjectMake(ctx, nullptr, nullptr)};
    set_prop(ctx, out, "url", make_string(ctx, result->url));
    set_prop(ctx, out, "authorization", make_string(ctx, result->authorization));
    set_prop(ctx, out, "host", make_string(ctx, result->host));
    set_prop(ctx, out, "amzDate", make_string(ctx, result->amz_date));
    set_prop(ctx, out, "signedHeaders", make_string(ctx, result->signed_headers));
    return static_cast<JSValueRef>(out);
}
