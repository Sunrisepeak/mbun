// dns.cppm — mbun.dns: thin aggregator over the DNS pure-logic sub-modules.
//
// The subsystem is split by concern (see AGENTS.md rule 10): error/type maps,
// address family + lookup options, address formatting, resolve*() record
// shapes, and the injectable resolver seam. Import `mbun.dns` to get all of it.
//
// Scope: pure logic (record/result shapes, address-family & option parsing,
// node:dns/Bun.dns error-code mapping aligned to bun) plus a live POSIX
// getaddrinfo/getnameinfo backend (mbun.dns.system) injected via a DnsBackend.
// The c-ares async channel + structured record queries are DEFERRED(S-net);
// see resolver.cppm / system.cppm.
export module mbun.dns;

export import mbun.dns.error;
export import mbun.dns.options;
export import mbun.dns.address;
export import mbun.dns.records;
export import mbun.dns.wire;
export import mbun.dns.client;
export import mbun.dns.resolver;
export import mbun.dns.system;
