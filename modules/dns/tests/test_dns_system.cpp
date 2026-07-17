// test_dns_system.cpp — exercises the live getaddrinfo/getnameinfo DnsBackend
// (mbun.dns.system) through the pure Resolver facade.
//
// Deterministic / offline-safe cases only in the pass count: numeric IP
// literals and `localhost` resolve from /etc/hosts + nss without any network,
// so they run everywhere. Real-domain resolution is attempted best-effort and
// only asserted when DNS egress is present (printed as a note otherwise) so the
// suite stays green in a no-network sandbox.

import std;
import mbun.dns;

namespace {

int failed{0};

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failed;
    }
}

using namespace mbun::dns;

// Does the resolved list contain the given textual address?
bool has_address(const ResultList& list, std::string_view want) {
    for (const auto& row : list) {
        if (address_to_string(row.address) == want) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main() {
    expect(system_backend_available(), "system backend available on this platform");

    Resolver resolver{system_backend()};

    // ── numeric IPv4 literal → itself (Inet), offline ────────────────────────
    {
        GetAddrInfo req;
        req.name = "127.0.0.1";
        auto r = resolver.lookup(req);
        expect(r.is_ok(), "lookup 127.0.0.1 ok");
        if (r.is_ok()) {
            expect(has_address(*r.value, "127.0.0.1"), "127.0.0.1 present");
            expect((*r.value)[0].address.family == Family::Inet, "127.0.0.1 is IPv4");
        }
    }

    // ── numeric IPv6 literal → itself (Inet6), offline ───────────────────────
    {
        GetAddrInfo req;
        req.name = "::1";
        auto r = resolver.lookup(req);
        expect(r.is_ok(), "lookup ::1 ok");
        if (r.is_ok()) {
            expect(has_address(*r.value, "::1"), "::1 present");
            expect((*r.value)[0].address.family == Family::Inet6, "::1 is IPv6");
        }
    }

    // ── family filter: request IPv4 only for a numeric IPv6 → ENOTFOUND ───────
    {
        GetAddrInfo req;
        req.name = "::1";
        req.options.family = Family::Inet;  // ask for A only
        auto r = resolver.lookup(req);
        expect(!r.is_ok(), "IPv4-only lookup of ::1 fails");
        // exact code is family-mismatch dependent (EBADFAMILY/ENOTFOUND per libc);
        // only require that it is a mapped error string.
        expect(!r.code().empty(), "IPv4-only ::1 has an error code");
    }

    // ── localhost resolves from /etc/hosts, offline ──────────────────────────
    {
        GetAddrInfo req;
        req.name = "localhost";
        auto r = resolver.lookup(req);
        expect(r.is_ok(), "lookup localhost ok");
        if (r.is_ok()) {
            expect(!r.value->empty(), "localhost has >=1 address");
        }
    }

    // ── result ordering policy applies over the live result set ──────────────
    {
        GetAddrInfo req;
        req.name = "localhost";  // typically both ::1 and 127.0.0.1
        resolver.set_order(Order::Ipv4first);
        auto r = resolver.lookup(req);
        if (r.is_ok() && r.value->size() > 1) {
            // ipv4first must place an Inet row first when one exists.
            bool has_v4 = false;
            for (const auto& row : *r.value) {
                if (row.address.family == Family::Inet) { has_v4 = true; break; }
            }
            if (has_v4) {
                expect((*r.value)[0].address.family == Family::Inet,
                       "ipv4first puts IPv4 first for localhost");
            }
        }
        resolver.set_order(Order::Verbatim);
    }

    // ── resolve4/resolve6 over getaddrinfo (ttl=0) ───────────────────────────
    {
        auto r = resolver.resolve_addresses("127.0.0.1", RecordType::A);
        expect(r.is_ok(), "resolve4 127.0.0.1 ok");
        if (r.is_ok()) {
            expect(r.value->size() == 1 && (*r.value)[0].address == "127.0.0.1",
                   "resolve4 127.0.0.1 == 127.0.0.1");
        }
        // record types needing c-ares are DEFERRED → ENOTIMP.
        auto mx = resolver.resolve_addresses("localhost", RecordType::MX);
        expect(!mx.is_ok() && mx.code() == "DNS_ENOTIMP", "resolve_addr MX → ENOTIMP (DEFERRED)");
    }

    // ── reverse() PTR for a numeric IP (getnameinfo) ─────────────────────────
    {
        auto r = resolver.reverse("127.0.0.1");
        // 127.0.0.1 usually maps to "localhost", but the exact PTR is host
        // config dependent, so only require a non-empty hostname on success.
        if (r.is_ok()) {
            expect(!r.value->empty() && !(*r.value)[0].empty(),
                   "reverse 127.0.0.1 returns a hostname");
        }
        // a non-IP reverse target is a bad string.
        auto bad = resolver.reverse("not-an-ip");
        expect(!bad.is_ok() && bad.code() == "DNS_EBADSTR", "reverse of non-IP → EBADSTR");
    }

    // ── best-effort real-domain resolution (only asserted when DNS egress) ───
    {
        GetAddrInfo req;
        req.name = "example.com";
        auto r = resolver.lookup(req);
        if (r.is_ok()) {
            expect(!r.value->empty(), "example.com resolved to >=1 address (network present)");
            std::cout << "note: DNS egress present, example.com resolved ("
                      << r.value->size() << " addr)\n";
        } else {
            std::cout << "note: no DNS egress (example.com: " << r.code()
                      << ") — real-domain assertion skipped\n";
        }
    }

    if (failed != 0) {
        std::cerr << failed << " check(s) failed\n";
        return 1;
    }
    std::cout << "test_dns_system ... ok\n";
    return 0;
}
