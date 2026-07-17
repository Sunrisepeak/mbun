// error.cppm — mbun.dns.error: c-ares status codes, node:dns error-code /
// message mapping, getaddrinfo(3) EAI → ares mapping, and DNS record/NS types.
//
// Mechanical port of bun's `src/cares_sys/c_ares.rs` (Error enum: code()/label()/
// get()/eai_to_error) plus the RecordType/NSType enums used by the resolver.
// Only the pure mapping tables live here; the FFI to libcares stays out.
// PORT-SOURCE: .mbun/bun-ref/src/cares_sys/c_ares.rs (Error, NSType, ARES_*)
//              .mbun/bun-ref/src/runtime/dns_jsc/dns.rs (RecordType)
export module mbun.dns.error;

import std;

namespace mbun::dns {

// ── ARES_* status constants (c-ares ares.h; verbatim discriminants) ──────────
export inline constexpr int ARES_SUCCESS              = 0;
export inline constexpr int ARES_ENODATA              = 1;
export inline constexpr int ARES_EFORMERR             = 2;
export inline constexpr int ARES_ESERVFAIL           = 3;
export inline constexpr int ARES_ENOTFOUND            = 4;
export inline constexpr int ARES_ENOTIMP              = 5;
export inline constexpr int ARES_EREFUSED             = 6;
export inline constexpr int ARES_EBADQUERY            = 7;
export inline constexpr int ARES_EBADNAME             = 8;
export inline constexpr int ARES_EBADFAMILY           = 9;
export inline constexpr int ARES_EBADRESP             = 10;
export inline constexpr int ARES_ECONNREFUSED         = 11;
export inline constexpr int ARES_ETIMEOUT             = 12;
export inline constexpr int ARES_EOF                  = 13;
export inline constexpr int ARES_EFILE                = 14;
export inline constexpr int ARES_ENOMEM               = 15;
export inline constexpr int ARES_EDESTRUCTION         = 16;
export inline constexpr int ARES_EBADSTR              = 17;
export inline constexpr int ARES_EBADFLAGS            = 18;
export inline constexpr int ARES_ENONAME              = 19;
export inline constexpr int ARES_EBADHINTS            = 20;
export inline constexpr int ARES_ENOTINITIALIZED      = 21;
export inline constexpr int ARES_ELOADIPHLPAPI        = 22;
export inline constexpr int ARES_EADDRGETNETWORKPARAMS = 23;
export inline constexpr int ARES_ECANCELLED           = 24;
export inline constexpr int ARES_ESERVICE             = 25;
export inline constexpr int ARES_ENOSERVER            = 26;

// The DNS Error enum (repr = ARES_* discriminant).
export enum class AresError : int {
    ENODATA              = ARES_ENODATA,
    EFORMERR             = ARES_EFORMERR,
    ESERVFAIL            = ARES_ESERVFAIL,
    ENOTFOUND            = ARES_ENOTFOUND,
    ENOTIMP              = ARES_ENOTIMP,
    EREFUSED             = ARES_EREFUSED,
    EBADQUERY            = ARES_EBADQUERY,
    EBADNAME             = ARES_EBADNAME,
    EBADFAMILY           = ARES_EBADFAMILY,
    EBADRESP             = ARES_EBADRESP,
    ECONNREFUSED         = ARES_ECONNREFUSED,
    ETIMEOUT             = ARES_ETIMEOUT,
    EOF                  = ARES_EOF,
    EFILE                = ARES_EFILE,
    ENOMEM               = ARES_ENOMEM,
    EDESTRUCTION         = ARES_EDESTRUCTION,
    EBADSTR              = ARES_EBADSTR,
    EBADFLAGS            = ARES_EBADFLAGS,
    ENONAME              = ARES_ENONAME,
    EBADHINTS            = ARES_EBADHINTS,
    ENOTINITIALIZED      = ARES_ENOTINITIALIZED,
    ELOADIPHLPAPI        = ARES_ELOADIPHLPAPI,
    EADDRGETNETWORKPARAMS = ARES_EADDRGETNETWORKPARAMS,
    ECANCELLED           = ARES_ECANCELLED,
    ESERVICE             = ARES_ESERVICE,
    ENOSERVER            = ARES_ENOSERVER,
};

// node:dns error `.code` string. Note several ares codes deliberately collapse
// to "DNS_ENOTFOUND" (EBADNAME/ENONAME) — that is the Node-visible spec.
export constexpr std::string_view error_code(AresError e) noexcept {
    switch (e) {
    case AresError::ENODATA:              return "DNS_ENODATA";
    case AresError::EFORMERR:             return "DNS_EFORMERR";
    case AresError::ESERVFAIL:            return "DNS_ESERVFAIL";
    case AresError::ENOTFOUND:            return "DNS_ENOTFOUND";
    case AresError::ENOTIMP:              return "DNS_ENOTIMP";
    case AresError::EREFUSED:             return "DNS_EREFUSED";
    case AresError::EBADQUERY:            return "DNS_EBADQUERY";
    case AresError::EBADNAME:             return "DNS_ENOTFOUND";
    case AresError::EBADFAMILY:           return "DNS_EBADFAMILY";
    case AresError::EBADRESP:             return "DNS_EBADRESP";
    case AresError::ECONNREFUSED:         return "DNS_ECONNREFUSED";
    case AresError::ETIMEOUT:             return "DNS_ETIMEOUT";
    case AresError::EOF:                  return "DNS_EOF";
    case AresError::EFILE:                return "DNS_EFILE";
    case AresError::ENOMEM:               return "DNS_ENOMEM";
    case AresError::EDESTRUCTION:         return "DNS_EDESTRUCTION";
    case AresError::EBADSTR:              return "DNS_EBADSTR";
    case AresError::EBADFLAGS:            return "DNS_EBADFLAGS";
    case AresError::ENONAME:              return "DNS_ENOTFOUND";
    case AresError::EBADHINTS:            return "DNS_EBADHINTS";
    case AresError::ENOTINITIALIZED:      return "DNS_ENOTINITIALIZED";
    case AresError::ELOADIPHLPAPI:        return "DNS_ELOADIPHLPAPI";
    case AresError::EADDRGETNETWORKPARAMS: return "DNS_EADDRGETNETWORKPARAMS";
    case AresError::ECANCELLED:           return "DNS_ECANCELLED";
    case AresError::ESERVICE:             return "DNS_ESERVICE";
    case AresError::ENOSERVER:            return "DNS_ENOSERVER";
    }
    return "DNS_ENOTFOUND";
}

// Human-readable message (ares_strerror equivalent, verbatim from bun).
export constexpr std::string_view error_label(AresError e) noexcept {
    switch (e) {
    case AresError::ENODATA:              return "No data record of requested type";
    case AresError::EFORMERR:             return "Malformed DNS query";
    case AresError::ESERVFAIL:            return "Server failed to complete the DNS operation";
    case AresError::ENOTFOUND:            return "Domain name not found";
    case AresError::ENOTIMP:              return "DNS resolver does not implement requested operation";
    case AresError::EREFUSED:             return "DNS operation refused";
    case AresError::EBADQUERY:            return "Misformatted DNS query";
    case AresError::EBADNAME:             return "Misformatted domain name";
    case AresError::EBADFAMILY:           return "Misformatted DNS query (family)";
    case AresError::EBADRESP:             return "Misformatted DNS reply";
    case AresError::ECONNREFUSED:         return "Could not contact DNS servers";
    case AresError::ETIMEOUT:             return "Timeout while contacting DNS servers";
    case AresError::EOF:                  return "End of file";
    case AresError::EFILE:                return "Error reading file";
    case AresError::ENOMEM:               return "Out of memory";
    case AresError::EDESTRUCTION:         return "Channel is being destroyed";
    case AresError::EBADSTR:              return "Misformatted string";
    case AresError::EBADFLAGS:            return "Illegal flags specified";
    case AresError::ENONAME:              return "Given hostname is not numeric";
    case AresError::EBADHINTS:            return "Illegal hints flags specified";
    case AresError::ENOTINITIALIZED:      return "Library initialization not yet performed";
    case AresError::ELOADIPHLPAPI:        return "ELOADIPHLPAPI TODO WHAT DOES THIS MEAN";
    case AresError::EADDRGETNETWORKPARAMS: return "EADDRGETNETWORKPARAMS";
    case AresError::ECANCELLED:           return "DNS query cancelled";
    case AresError::ESERVICE:             return "Service not available";
    case AresError::ENOSERVER:            return "No DNS servers were configured";
    }
    return "Domain name not found";
}

// `Error::get` — map a raw c-ares status code (may be negated by Node) to an
// AresError. `nullopt` == success. Mirrors bun: ENODATA/ENONAME collapse to
// ENOTFOUND, out-of-range positive codes are rejected (returns nullopt).
export constexpr std::optional<AresError> error_from_status(int rc) noexcept {
    // Node collapses "no data" / "no name" onto ENOTFOUND before dispatch.
    if (rc == ARES_ENODATA || rc == ARES_ENONAME) {
        return AresError::ENOTFOUND;
    }
    if (rc == 0) {
        return std::nullopt;
    }
    // c-ares returns positive ARES_* codes; Node's wrapper sometimes negates.
    unsigned int n{rc < 0 ? static_cast<unsigned int>(-(static_cast<long long>(rc)))
                          : static_cast<unsigned int>(rc)};
    if (n < 1u || n > static_cast<unsigned int>(ARES_ENOSERVER)) {
        return std::nullopt;  // out of range — bun asserts; we fail soft here
    }
    return static_cast<AresError>(static_cast<int>(n));
}

// ── getaddrinfo(3) EAI_* → AresError (POSIX branch of bun's eai mapping) ─────
// These EAI_* values are the glibc/Linux numeric codes; the mapping itself is
// platform-independent Node behavior. Windows (UV_EAI_*) is DEFERRED(S-net).
export inline constexpr int EAI_BADFLAGS   = -1;
export inline constexpr int EAI_NONAME     = -2;
export inline constexpr int EAI_AGAIN      = -3;
export inline constexpr int EAI_FAIL       = -4;
export inline constexpr int EAI_FAMILY     = -6;
export inline constexpr int EAI_SOCKTYPE   = -7;
export inline constexpr int EAI_SERVICE    = -8;
export inline constexpr int EAI_MEMORY     = -10;
export inline constexpr int EAI_SYSTEM     = -11;
export inline constexpr int EAI_OVERFLOW   = -12;
export inline constexpr int EAI_NODATA     = -5;
export inline constexpr int EAI_ADDRFAMILY = -9;

// POSIX getaddrinfo error → AresError, per Node's lib/internal/errors.js table.
// `nullopt` == success (rc == 0). DEFERRED(S-net): glibc-only async EAI_* (IDN,
// INPROGRESS, ALLDONE) and the Windows UV_EAI_* branch are not modeled here.
export constexpr std::optional<AresError> eai_to_error(int rc) noexcept {
    if (rc == EAI_NODATA || rc == EAI_NONAME) {
        return AresError::ENOTFOUND;
    }
    if (rc == EAI_SOCKTYPE) {  // linux/android
        return AresError::ECONNREFUSED;
    }
    if (rc == 0) {
        return std::nullopt;
    }
    switch (rc) {
    case EAI_ADDRFAMILY: return AresError::EBADFAMILY;
    case EAI_BADFLAGS:   return AresError::EBADFLAGS;
    case EAI_FAIL:       return AresError::EBADRESP;
    case EAI_FAMILY:     return AresError::EBADFAMILY;
    case EAI_MEMORY:     return AresError::ENOMEM;
    case EAI_SERVICE:    return AresError::ESERVICE;
    case EAI_SYSTEM:     return AresError::ESERVFAIL;
    default:             return AresError::ENOTIMP;
    }
}

// ── RecordType — node:dns query types (wire type numbers) ────────────────────
export enum class RecordType : int {
    A     = 1,
    AAAA  = 28,
    CAA   = 257,
    CNAME = 5,
    MX    = 15,
    NAPTR = 35,
    NS    = 2,
    PTR   = 12,
    SOA   = 6,
    SRV   = 33,
    TXT   = 16,
    ANY   = 255,
};

export inline constexpr RecordType RECORD_TYPE_DEFAULT = RecordType::A;

// Parse a record-type string ("A"/"a"/"AAAA"/... "TXT") → RecordType.
// Mirrors bun's RECORD_TYPE_MAP (case-insensitive over the canonical names).
export constexpr std::optional<RecordType> record_type_from_string(std::string_view s) noexcept {
    struct Entry { std::string_view key; RecordType val; };
    constexpr Entry table[] = {
        {"A", RecordType::A},       {"AAAA", RecordType::AAAA}, {"ANY", RecordType::ANY},
        {"CAA", RecordType::CAA},   {"CNAME", RecordType::CNAME}, {"MX", RecordType::MX},
        {"NAPTR", RecordType::NAPTR},
        {"NS", RecordType::NS},     {"PTR", RecordType::PTR},   {"SOA", RecordType::SOA},
        {"SRV", RecordType::SRV},   {"TXT", RecordType::TXT},
        {"a", RecordType::A},       {"aaaa", RecordType::AAAA}, {"any", RecordType::ANY},
        {"caa", RecordType::CAA},   {"cname", RecordType::CNAME}, {"mx", RecordType::MX},
        {"naptr", RecordType::NAPTR},
        {"ns", RecordType::NS},     {"ptr", RecordType::PTR},   {"soa", RecordType::SOA},
        {"srv", RecordType::SRV},   {"txt", RecordType::TXT},
    };
    for (const auto& e : table) {
        if (e.key == s) {
            return e.val;
        }
    }
    return std::nullopt;
}

}  // namespace mbun::dns
