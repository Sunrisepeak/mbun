// npm/negatable.cppm — mbun.install.npm.negatable
//
// Port of bun's os/cpu/libc allow/block-list bitsets from the npm manifest
// (`os`, `cpu`, `libc` fields) — reference:
//   .mbun/bun-ref/src/install_types/resolver_hooks.rs
//     (OperatingSystem / Architecture / Libc newtypes + `Negatable<T>` with
//      `apply()` / `combine()`), and .mbun/bun-ref/src/install/npm.rs
//     (`negatable_from_json_value`).
//
// The bit positions are load-bearing in bun (they round-trip through bun.lock
// and the on-disk manifest cache); we keep the same layout (bit 0 unused, first
// value = 1 << 1). This is a re-implementation over std:: — no arena / no
// on-disk ABI, just the pure allow/block-list collapse logic.
export module mbun.install.npm.negatable;

import std;

namespace mbun::install::npm {

// ── OperatingSystem ─────────────────────────────────────────────────────────
// package.json `os` uses process.platform values ("win32", "darwin", ...).
export enum class OperatingSystem : std::uint16_t {
    NONE = 0,
    AIX = 1 << 1,
    DARWIN = 1 << 2,
    FREEBSD = 1 << 3,
    LINUX = 1 << 4,
    OPENBSD = 1 << 5,
    SUNOS = 1 << 6,
    WIN32 = 1 << 7,
    ANDROID = 1 << 8,
    ALL = AIX | DARWIN | FREEBSD | LINUX | OPENBSD | SUNOS | WIN32 | ANDROID,
};

// ── Architecture (package.json `cpu`) ───────────────────────────────────────
export enum class Architecture : std::uint16_t {
    NONE = 0,
    ARM = 1 << 1,
    ARM64 = 1 << 2,
    IA32 = 1 << 3,
    MIPS = 1 << 4,
    MIPSEL = 1 << 5,
    PPC = 1 << 6,
    PPC64 = 1 << 7,
    S390 = 1 << 8,
    S390X = 1 << 9,
    X32 = 1 << 10,
    X64 = 1 << 11,
    ALL = ARM | ARM64 | IA32 | MIPS | MIPSEL | PPC | PPC64 | S390 | S390X | X32 | X64,
};

// ── Libc (package.json `libc`) ──────────────────────────────────────────────
export enum class Libc : std::uint8_t {
    NONE = 0,
    GLIBC = 1 << 1,
    MUSL = 1 << 2,
    ALL = GLIBC | MUSL,
};

// ── the host platform ───────────────────────────────────────────────────────
// bun's `cfg`-gated `CURRENT` constants: resolver_hooks.rs:839-847 (os) and
// :959-961 (cpu). bun defines these only for the targets it ships, so an
// unlisted target is a hard error there; we mirror that instead of silently
// defaulting to a platform we are not on (which would filter the wrong set).
//
// `__ANDROID__` must be tested before `__linux__`: Android is also `__linux__`,
// and bun distinguishes the two (ANDROID vs LINUX are separate bits).
export inline constexpr OperatingSystem CURRENT_OS{
#if defined(__ANDROID__)
    OperatingSystem::ANDROID
#elif defined(__linux__)
    OperatingSystem::LINUX
#elif defined(__APPLE__)
    OperatingSystem::DARWIN
#elif defined(_WIN32)
    OperatingSystem::WIN32
#elif defined(__FreeBSD__)
    OperatingSystem::FREEBSD
#else
#error "mbun.install.npm.negatable: unsupported target OS (no OperatingSystem::CURRENT)"
#endif
};

export inline constexpr Architecture CURRENT_ARCH{
#if defined(__aarch64__) || defined(_M_ARM64)
    Architecture::ARM64
#elif defined(__x86_64__) || defined(_M_X64)
    Architecture::X64
#else
#error "mbun.install.npm.negatable: unsupported target architecture (no Architecture::CURRENT)"
#endif
};

namespace detail {

// Name → bit tables. Order mirrors bun's `negatable_names!` lists (the byte
// order is part of bun.lock output, kept here for parity even though we don't
// serialize yet).
struct OsKv {
    std::string_view name;
    std::uint16_t bit;
};
constexpr std::array<OsKv, 8> OS_NAMES{{
    {"aix", static_cast<std::uint16_t>(OperatingSystem::AIX)},
    {"linux", static_cast<std::uint16_t>(OperatingSystem::LINUX)},
    {"sunos", static_cast<std::uint16_t>(OperatingSystem::SUNOS)},
    {"win32", static_cast<std::uint16_t>(OperatingSystem::WIN32)},
    {"darwin", static_cast<std::uint16_t>(OperatingSystem::DARWIN)},
    {"android", static_cast<std::uint16_t>(OperatingSystem::ANDROID)},
    {"freebsd", static_cast<std::uint16_t>(OperatingSystem::FREEBSD)},
    {"openbsd", static_cast<std::uint16_t>(OperatingSystem::OPENBSD)},
}};

struct ArchKv {
    std::string_view name;
    std::uint16_t bit;
};
constexpr std::array<ArchKv, 11> ARCH_NAMES{{
    {"arm", static_cast<std::uint16_t>(Architecture::ARM)},
    {"arm64", static_cast<std::uint16_t>(Architecture::ARM64)},
    {"ia32", static_cast<std::uint16_t>(Architecture::IA32)},
    {"mips", static_cast<std::uint16_t>(Architecture::MIPS)},
    {"mipsel", static_cast<std::uint16_t>(Architecture::MIPSEL)},
    {"ppc", static_cast<std::uint16_t>(Architecture::PPC)},
    {"ppc64", static_cast<std::uint16_t>(Architecture::PPC64)},
    {"s390", static_cast<std::uint16_t>(Architecture::S390)},
    {"s390x", static_cast<std::uint16_t>(Architecture::S390X)},
    {"x32", static_cast<std::uint16_t>(Architecture::X32)},
    {"x64", static_cast<std::uint16_t>(Architecture::X64)},
}};

struct LibcKv {
    std::string_view name;
    std::uint8_t bit;
};
constexpr std::array<LibcKv, 2> LIBC_NAMES{{
    {"musl", static_cast<std::uint8_t>(Libc::MUSL)},
    {"glibc", static_cast<std::uint8_t>(Libc::GLIBC)},
}};

}  // namespace detail

// Traits mapping an enum to its name table + int type. Specialized per enum so
// the `combine`/`apply` logic below is written once.
template <class T>
struct NegatableTraits;

template <>
struct NegatableTraits<OperatingSystem> {
    using Int = std::uint16_t;
    static constexpr Int ALL_VALUE{static_cast<Int>(OperatingSystem::ALL)};
    static std::optional<Int> lookup(std::string_view key) {
        for (const auto& kv : detail::OS_NAMES) {
            if (kv.name == key) {
                return kv.bit;
            }
        }
        return std::nullopt;
    }
};

template <>
struct NegatableTraits<Architecture> {
    using Int = std::uint16_t;
    static constexpr Int ALL_VALUE{static_cast<Int>(Architecture::ALL)};
    static std::optional<Int> lookup(std::string_view key) {
        for (const auto& kv : detail::ARCH_NAMES) {
            if (kv.name == key) {
                return kv.bit;
            }
        }
        return std::nullopt;
    }
};

template <>
struct NegatableTraits<Libc> {
    using Int = std::uint8_t;
    static constexpr Int ALL_VALUE{static_cast<Int>(Libc::ALL)};
    static std::optional<Int> lookup(std::string_view key) {
        for (const auto& kv : detail::LIBC_NAMES) {
            if (kv.name == key) {
                return kv.bit;
            }
        }
        return std::nullopt;
    }
};

// Accumulates the allow/block-list from a JSON string array, then collapses to
// a single bitset via `combine()`. Mirrors `Negatable<T>` in resolver_hooks.rs.
template <class T>
struct Negatable {
    using Int = typename NegatableTraits<T>::Int;
    Int added{0};
    Int removed{0};
    bool hadWildcard{false};
    bool hadUnrecognizedValues{false};

    void apply(std::string_view str) {
        if (str.empty()) {
            return;
        }
        if (str == "any") {
            hadWildcard = true;
            return;
        }
        if (str == "none") {
            hadUnrecognizedValues = true;
            return;
        }
        bool isNot{str.front() == '!'};
        std::string_view name{isNot ? str.substr(1) : str};
        std::optional<Int> field{NegatableTraits<T>::lookup(name)};
        if (!field) {
            if (!isNot) {
                hadUnrecognizedValues = true;
            }
            return;
        }
        // Applying a recognised token resets the wildcard/unrecognized flags to
        // their defaults (so `["any","linux"]` collapses to LINUX).
        if (isNot) {
            Int rm{static_cast<Int>(removed | *field)};
            *this = Negatable{};
            removed = rm;
        } else {
            Int ad{static_cast<Int>(added | *field)};
            *this = Negatable{};
            added = ad;
        }
    }

    T combine() const {
        constexpr Int allValue{NegatableTraits<T>::ALL_VALUE};
        Int addedV{hadWildcard ? allValue : added};
        Int removedV{removed};
        if (addedV == 0 && removedV == 0) {
            if (hadUnrecognizedValues) {
                return T::NONE;
            }
            return T::ALL;
        }
        if (addedV == 0 && removedV != 0) {
            return static_cast<T>(static_cast<Int>(allValue & ~removedV));
        }
        if (removedV == 0) {
            return static_cast<T>(addedV);
        }
        return static_cast<T>(static_cast<Int>(addedV & ~removedV));
    }
};

// `(self.0 & target.0) != 0` — whether an installable platform intersects.
export template <class T>
bool is_match(T self, T target) {
    using Int = typename NegatableTraits<T>::Int;
    return (static_cast<Int>(self) & static_cast<Int>(target)) != 0;
}

// `Meta::is_disabled` — .mbun/bun-ref/src/install/lockfile/Package/Meta.rs:68:
//     !self.arch.is_match(cpu) || !self.os.is_match(os)
// `cpu`/`os` are the package's manifest fields; `targetCpu`/`targetOs` are what
// we are installing for (`options.cpu` / `options.os`, PackageManagerOptions.rs:
// 88-90, defaulted to CURRENT at :153-154 and overridable via `--cpu`/`--os`,
// CommandLineArguments.rs:120/:123).
//
// os/cpu ONLY, deliberately: bun's `Meta` carries no `libc` field, bun.lock's
// libc round-trip is still commented out (bun.lock.rs:1164-1168, :2571-2572) and
// Tree.rs:416 marks libc a TODO. The manifest `libc` field is parsed (npm.rs:
// 2394-2395) but never enforced, so enforcing it here would over-filter relative
// to bun (e.g. musl-only packages would vanish on a glibc host).
//
// Note this is independent of `optional` — Meta.rs:67 says so outright, and bun
// 1.3.14 confirms it: a *required* `fsevents` (os: darwin) on linux exits 0 with
// the package simply absent, where npm would raise EBADPLATFORM.
export bool is_disabled(Architecture cpu, OperatingSystem os, Architecture targetCpu = CURRENT_ARCH,
                        OperatingSystem targetOs = CURRENT_OS) {
    return !is_match(cpu, targetCpu) || !is_match(os, targetOs);
}

// Collapse a list of allow/block tokens into a single bitset. Empty list =>
// ALL (matches everything), matching `negatable_from_json_value`.
export template <class T>
T negatable_from_tokens(std::span<const std::string_view> tokens) {
    Negatable<T> acc{};
    for (std::string_view tok : tokens) {
        acc.apply(tok);
    }
    return acc.combine();
}

// Single string token (npm allows `"os": "linux"` as a bare string).
export template <class T>
T negatable_from_string(std::string_view token) {
    Negatable<T> acc{};
    acc.apply(token);
    return acc.combine();
}

}  // namespace mbun::install::npm
