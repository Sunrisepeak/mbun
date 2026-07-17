// semver.cppm — mbun.semver: Bun.semver equivalent (order / satisfies).
//
// Behavior is pinned by bun's original test suite (see tests/test_semver.cpp).
// Design goals (this is a re-implementation, not a port):
//   - single-pass parsing over std::string_view, zero heap allocation
//   - ranges are evaluated streaming: no comparator lists are materialized,
//     so arbitrarily long "||" / AND chains run in linear time with O(1)
//     extra memory and no recursion
//   - pre-release identifiers are compared by slicing string_views in place
export module mbun.semver;

import std;

namespace mbun::semver {

namespace {

// Numeric components are clamped instead of overflowing on absurd digit runs.
constexpr std::uint64_t NUM_CLAMP{std::numeric_limits<std::uint64_t>::max() / 16};

// A (possibly partial) version. A "wild" component is either missing or an
// explicit wildcard ('x', 'X', '*'). For order() a wild component sorts
// higher than any number (bun's loose-compare semantics).
struct Version {
    std::uint64_t major{0};
    std::uint64_t minor{0};
    std::uint64_t patch{0};
    bool majorWild{true};
    bool minorWild{true};
    bool patchWild{true};
    std::string_view pre{};  // empty => no pre-release (build metadata is dropped)

    bool has_pre() const {
        return !pre.empty();
    }

    bool any_wild() const {
        return majorWild || minorWild || patchWild;
    }
};

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool is_wildcard(char c) {
    return c == 'x' || c == 'X' || c == '*';
}

// Characters allowed in pre-release / build identifier runs.
bool is_ident(char c) {
    return is_digit(c) || is_alpha(c) || c == '-' || c == '.';
}

std::uint64_t read_number(std::string_view s, std::size_t& pos) {
    std::uint64_t value{0};
    while (pos < s.size() && is_digit(s[pos])) {
        if (value < NUM_CLAMP) {
            value = value * 10 + static_cast<std::uint64_t>(s[pos] - '0');
        }
        ++pos;
    }
    return value;
}

// Loose prefix accepted before a version: whitespace, 'v'/'V' and '='
// in any combination (" v 1.2.3", "= 1.2.3", "  v1.2.3+build", ...).
void skip_loose_prefix(std::string_view s, std::size_t& pos) {
    while (pos < s.size()) {
        char c{s[pos]};
        if (is_space(c) || c == 'v' || c == 'V' || c == '=') {
            ++pos;
        } else {
            break;
        }
    }
}

enum class Component : std::uint8_t { NUMBER, WILD, ABSENT };

Component parse_component(std::string_view s, std::size_t& pos, std::uint64_t& value) {
    if (pos < s.size() && is_digit(s[pos])) {
        value = read_number(s, pos);
        return Component::NUMBER;
    }
    if (pos < s.size() && is_wildcard(s[pos])) {
        ++pos;
        return Component::WILD;
    }
    return Component::ABSENT;
}

// Parses a (possibly partial) version at `pos`, consuming a loose prefix,
// "major[.minor[.patch]]", an optional pre-release ("-pre" always; a bare
// alphanumeric tail like "1.2.3pre" only after an explicit patch) and
// optional build metadata (dropped). Returns false if no leading component
// exists; `pos` is left where parsing stopped (never rewound), which keeps
// repeated parse attempts over adversarial inputs linear overall.
bool parse_version(std::string_view s, std::size_t& pos, Version& out) {
    out = Version{};
    skip_loose_prefix(s, pos);

    std::uint64_t value{0};
    Component majorKind{parse_component(s, pos, value)};
    if (majorKind == Component::ABSENT) {
        return false;
    }
    if (majorKind == Component::NUMBER) {
        out.major = value;
        out.majorWild = false;
    }
    if (pos < s.size() && s[pos] == '.') {
        ++pos;
        Component minorKind{parse_component(s, pos, value)};
        if (minorKind == Component::NUMBER) {
            out.minor = value;
            out.minorWild = false;
        }
        if (minorKind != Component::ABSENT && pos < s.size() && s[pos] == '.') {
            ++pos;
            Component patchKind{parse_component(s, pos, value)};
            if (patchKind == Component::NUMBER) {
                out.patch = value;
                out.patchWild = false;
            }
        }
    }

    if (pos < s.size() && s[pos] == '-') {
        ++pos;
        std::size_t start{pos};
        while (pos < s.size() && is_ident(s[pos])) {
            ++pos;
        }
        out.pre = s.substr(start, pos - start);
    } else if (pos < s.size() && is_alpha(s[pos]) && !out.patchWild) {
        std::size_t start{pos};
        while (pos < s.size() && is_ident(s[pos])) {
            ++pos;
        }
        out.pre = s.substr(start, pos - start);
    }
    if (pos < s.size() && s[pos] == '+') {
        ++pos;
        while (pos < s.size() && is_ident(s[pos])) {
            ++pos;
        }
    }
    return true;
}

int cmp_u64(std::uint64_t a, std::uint64_t b) {
    if (a != b) {
        return a < b ? -1 : 1;
    }
    return 0;
}

std::string_view strip_leading_zeros(std::string_view s) {
    std::size_t i{0};
    while (i + 1 < s.size() && s[i] == '0') {
        ++i;
    }
    return s.substr(i);
}

// npm pre-release identifier ordering: numeric identifiers compare
// numerically and sort lower than alphanumeric ones; alphanumeric
// identifiers compare byte-wise (ASCII).
int compare_identifier(std::string_view a, std::string_view b) {
    auto numeric = [](std::string_view s) {
        if (s.empty()) {
            return false;
        }
        for (char c : s) {
            if (!is_digit(c)) {
                return false;
            }
        }
        return true;
    };
    bool aNum{numeric(a)};
    bool bNum{numeric(b)};
    if (aNum != bNum) {
        return aNum ? -1 : 1;
    }
    if (aNum) {
        // length-then-lexicographic on zero-stripped digits: numeric compare
        // without overflow, for identifiers of any length
        std::string_view sa{strip_leading_zeros(a)};
        std::string_view sb{strip_leading_zeros(b)};
        if (sa.size() != sb.size()) {
            return sa.size() < sb.size() ? -1 : 1;
        }
        int c{sa.compare(sb)};
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    int c{a.compare(b)};
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

// Compares dot-separated pre-release tags; an empty tag means "no
// pre-release" and sorts higher than any pre-release. A tag that is a
// strict segment prefix of the other sorts lower ("a" < "a.b").
int compare_pre(std::string_view a, std::string_view b) {
    if (a.empty() || b.empty()) {
        if (a.empty() == b.empty()) {
            return 0;
        }
        return a.empty() ? 1 : -1;
    }
    constexpr std::size_t npos{std::string_view::npos};
    std::size_t i{0};
    std::size_t j{0};
    while (i != npos || j != npos) {
        if (i == npos) {
            return -1;
        }
        if (j == npos) {
            return 1;
        }
        std::size_t iEnd{a.find('.', i)};
        std::size_t jEnd{b.find('.', j)};
        std::string_view sa{a.substr(i, (iEnd == npos ? a.size() : iEnd) - i)};
        std::string_view sb{b.substr(j, (jEnd == npos ? b.size() : jEnd) - j)};
        if (int c{compare_identifier(sa, sb)}) {
            return c;
        }
        i = (iEnd == npos) ? npos : iEnd + 1;
        j = (jEnd == npos) ? npos : jEnd + 1;
    }
    return 0;
}

// Full comparison of two concrete versions (wildcards must be resolved).
int compare_concrete(const Version& a, const Version& b) {
    if (int c{cmp_u64(a.major, b.major)}) {
        return c;
    }
    if (int c{cmp_u64(a.minor, b.minor)}) {
        return c;
    }
    if (int c{cmp_u64(a.patch, b.patch)}) {
        return c;
    }
    return compare_pre(a.pre, b.pre);
}

Version make_bound(std::uint64_t major, std::uint64_t minor, std::uint64_t patch) {
    Version v{};
    v.major = major;
    v.minor = minor;
    v.patch = patch;
    v.majorWild = false;
    v.minorWild = false;
    v.patchWild = false;
    return v;
}

// Wildcard components replaced by 0 (pre-release kept): the lower bound a
// partial comparator desugars to.
Version zeroed(const Version& v) {
    Version r{make_bound(v.majorWild ? 0 : v.major, v.minorWild ? 0 : v.minor,
                         v.patchWild ? 0 : v.patch)};
    r.pre = v.pre;
    return r;
}

enum class Op : std::uint8_t { EXACT, GT, GTE, LT, LTE, CARET, TILDE };

// Evaluates one comparator against a concrete version, desugaring partial
// versions npm-style (e.g. ">1.3" => ">=1.4.0", "<=2" => "<3.0.0",
// "^0.2.3" => ">=0.2.3 <0.3.0"). The pre-release gate is handled separately.
bool eval_comparator(const Version& v, Op op, const Version& c) {
    switch (op) {
        case Op::EXACT:
            if (c.majorWild) {
                return true;
            }
            if (v.major != c.major) {
                return false;
            }
            if (c.minorWild) {
                return true;
            }
            if (v.minor != c.minor) {
                return false;
            }
            if (c.patchWild) {
                return true;
            }
            return v.patch == c.patch && compare_pre(v.pre, c.pre) == 0;
        case Op::GTE:
            if (c.majorWild) {
                return true;
            }
            return compare_concrete(v, zeroed(c)) >= 0;
        case Op::GT:
            if (c.majorWild) {
                return false;
            }
            if (c.minorWild) {
                return compare_concrete(v, make_bound(c.major + 1, 0, 0)) >= 0;
            }
            if (c.patchWild) {
                return compare_concrete(v, make_bound(c.major, c.minor + 1, 0)) >= 0;
            }
            return compare_concrete(v, c) > 0;
        case Op::LT:
            if (c.majorWild) {
                return false;
            }
            return compare_concrete(v, zeroed(c)) < 0;
        case Op::LTE:
            if (c.majorWild) {
                return true;
            }
            if (c.minorWild) {
                return compare_concrete(v, make_bound(c.major + 1, 0, 0)) < 0;
            }
            if (c.patchWild) {
                return compare_concrete(v, make_bound(c.major, c.minor + 1, 0)) < 0;
            }
            return compare_concrete(v, c) <= 0;
        case Op::CARET:
            {
                if (c.majorWild) {
                    return true;
                }
                if (compare_concrete(v, zeroed(c)) < 0) {
                    return false;
                }
                Version upper{};
                if (c.major > 0) {
                    upper = make_bound(c.major + 1, 0, 0);
                } else if (c.minorWild) {
                    upper = make_bound(1, 0, 0);
                } else if (c.minor > 0 || c.patchWild) {
                    upper = make_bound(0, c.minor + 1, 0);
                } else {
                    upper = make_bound(0, 0, c.patch + 1);
                }
                return compare_concrete(v, upper) < 0;
            }
        case Op::TILDE:
            {
                if (c.majorWild) {
                    return true;
                }
                if (compare_concrete(v, zeroed(c)) < 0) {
                    return false;
                }
                Version upper{c.minorWild ? make_bound(c.major + 1, 0, 0)
                                          : make_bound(c.major, c.minor + 1, 0)};
                return compare_concrete(v, upper) < 0;
            }
    }
    return false;
}

// npm pre-release gate: a pre-release version only matches a group that
// contains a comparator carrying a pre-release on the same [major, minor,
// patch] tuple.
bool allows_pre(const Version& v, const Version& c) {
    if (!c.has_pre()) {
        return false;
    }
    return v.major == (c.majorWild ? 0 : c.major) && v.minor == (c.minorWild ? 0 : c.minor) &&
           v.patch == (c.patchWild ? 0 : c.patch);
}

// order()-style component comparison: wildcard/missing sorts higher than
// any number ("1.x" > "1.0.x", "*" > "4294967295...").
int compare_loose_component(std::uint64_t a, bool aWild, std::uint64_t b, bool bWild) {
    if (aWild || bWild) {
        if (aWild && bWild) {
            return 0;
        }
        return aWild ? 1 : -1;
    }
    return cmp_u64(a, b);
}

}  // namespace

// Whether `v` parses as a version at all — the equivalent of bun's
// `Version::parse(...).valid` (semver/Version.rs), which SemverObject.rs's
// `order` host function tests before comparing and throws "Invalid SemVer: {}"
// on. Wildcards and partials ("*", "1.x", "1.2") are valid; only input with no
// leading version component at all ("", "junk") is not. Kept here rather than in
// the binding so mbun.semver stays the single owner of the grammar.
export bool is_valid(std::string_view v) {
    Version parsed{};
    std::size_t pos{0};
    return parse_version(v, pos, parsed);
}

// Total order over loosely parsed versions (Bun.semver.order): returns
// -1, 0 or 1. Build metadata is ignored; wildcard/missing components sort
// higher than any number; unparsable input degrades to "*". Callers that need
// bun's throw-on-invalid behaviour gate on is_valid() first.
export int order(std::string_view a, std::string_view b) {
    Version va{};
    Version vb{};
    std::size_t pa{0};
    std::size_t pb{0};
    parse_version(a, pa, va);
    parse_version(b, pb, vb);
    if (int c{compare_loose_component(va.major, va.majorWild, vb.major, vb.majorWild)}) {
        return c;
    }
    if (int c{compare_loose_component(va.minor, va.minorWild, vb.minor, vb.minorWild)}) {
        return c;
    }
    if (int c{compare_loose_component(va.patch, va.patchWild, vb.patch, vb.patchWild)}) {
        return c;
    }
    return compare_pre(va.pre, vb.pre);
}

// Bun.semver.satisfies(version, range). Range grammar: comparators
// (">=", ">", "<=", "<", "=", "^", "~", "~>"), exact/partial/x-range
// versions, hyphen ranges "a - b", whitespace = AND, "||" = OR. Unparsable
// chunks are skipped character-wise (a version surfacing later in a chunk
// still counts, matching bun); a dangling "-" swallows the following chunk.
// A range that yields no comparator at all matches anything.
export bool satisfies(std::string_view version, std::string_view range) {
    Version v{};
    std::size_t vPos{0};
    bool vOk{parse_version(version, vPos, v)};
    if (vOk) {
        while (vPos < version.size() && is_space(version[vPos])) {
            ++vPos;
        }
        vOk = vPos == version.size() && !v.any_wild();
    }

    const std::size_t n{range.size()};
    std::size_t pos{0};
    bool anyComparator{false};
    bool groupHasComparator{false};
    bool groupMatched{true};
    bool groupPreOk{false};

    auto skip_ws = [&] {
        while (pos < n && is_space(range[pos])) {
            ++pos;
        }
    };
    auto skip_chunk = [&] {  // to next whitespace or "||" boundary
        while (pos < n && !is_space(range[pos]) && range[pos] != '|') {
            ++pos;
        }
    };
    auto group_satisfied = [&] {
        return vOk && groupHasComparator && groupMatched && (!v.has_pre() || groupPreOk);
    };
    // Returns false when scanning can stop early (invalid version can never
    // satisfy a range that has at least one comparator).
    auto apply = [&](Op op, const Version& c) {
        anyComparator = true;
        groupHasComparator = true;
        if (!vOk) {
            return false;
        }
        if (groupMatched && !eval_comparator(v, op, c)) {
            groupMatched = false;
        }
        if (v.has_pre() && allows_pre(v, c)) {
            groupPreOk = true;
        }
        return true;
    };

    while (true) {
        skip_ws();
        if (pos >= n) {
            break;
        }
        char c{range[pos]};
        if (c == '|') {
            if (pos + 1 < n && range[pos + 1] == '|') {
                if (group_satisfied()) {
                    return true;
                }
                groupHasComparator = false;
                groupMatched = true;
                groupPreOk = false;
                pos += 2;
            } else {
                ++pos;  // stray single '|': skip
            }
            continue;
        }
        if (vOk && !groupMatched) {
            // group already failed: fast-forward to the next "||"
            std::size_t next{range.find("||", pos)};
            pos = (next == std::string_view::npos) ? n : next;
            continue;
        }
        if (c == '>' || c == '<' || c == '=' || c == '^' || c == '~') {
            Op op{Op::EXACT};
            if (c == '>') {
                ++pos;
                op = Op::GT;
                if (pos < n && range[pos] == '=') {
                    ++pos;
                    op = Op::GTE;
                }
            } else if (c == '<') {
                ++pos;
                op = Op::LT;
                if (pos < n && range[pos] == '=') {
                    ++pos;
                    op = Op::LTE;
                }
            } else if (c == '^') {
                ++pos;
                op = Op::CARET;
            } else if (c == '~') {
                ++pos;
                op = Op::TILDE;
                if (pos < n && range[pos] == '>') {
                    ++pos;  // "~>" is an alias of "~"
                }
            } else {
                ++pos;  // '='
            }
            skip_ws();
            Version bound{};
            if (parse_version(range, pos, bound)) {
                skip_chunk();  // drop any trailing junk glued to the version
                if (!apply(op, bound)) {
                    return false;
                }
            }
            // on failure parse_version already consumed only skippable
            // prefix characters; the offending character is handled by the
            // next loop iteration (character-wise garbage skipping)
            continue;
        }
        if (c == '-') {
            if (pos + 1 < n && !is_space(range[pos + 1]) && range[pos + 1] != '|') {
                skip_chunk();  // "-q"-style junk: skip the chunk, never past "||"
                continue;
            }
            // dangling hyphen (no left-hand version): swallow the would-be
            // right-hand side chunk, producing no comparator
            ++pos;
            skip_ws();
            if (pos < n && range[pos] != '|') {
                Version discarded{};
                parse_version(range, pos, discarded);
                skip_chunk();
            }
            continue;
        }
        // bare (possibly partial / wildcard) version, or garbage
        Version left{};
        if (!parse_version(range, pos, left)) {
            ++pos;  // unrecognized character: skip it and rescan
            continue;
        }
        skip_chunk();
        std::size_t afterLeft{pos};
        skip_ws();
        if (pos < n && range[pos] == '-' &&
            (pos + 1 >= n || is_space(range[pos + 1]) || range[pos + 1] == '|')) {
            // hyphen range "left - right"
            ++pos;
            skip_ws();
            Version right{};
            if (pos < n && range[pos] != '|' && parse_version(range, pos, right)) {
                skip_chunk();
                if (!apply(Op::GTE, left) || !apply(Op::LTE, right)) {
                    return false;
                }
            } else {
                // dangling/garbage right-hand side: drop the whole construct
                skip_chunk();
            }
            continue;
        }
        pos = afterLeft;
        if (!apply(Op::EXACT, left)) {
            return false;
        }
    }

    if (group_satisfied()) {
        return true;
    }
    return !anyComparator;  // an empty range matches anything
}

}  // namespace mbun::semver
