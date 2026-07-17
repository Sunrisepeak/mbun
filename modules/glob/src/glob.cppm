// mbun.glob — Bun.Glob.match equivalent (T2.9).
//
// Port of bun's glob matcher (src/glob/matcher.rs, itself derived from
// devongovett/glob-match and Stephen Gregoratto's Zig port; MIT). Semantics
// are pinned by test/js/bun/glob/match.test.ts.
//
// Supported syntax (same as bun):
//   "?"      any single (non-separator) codepoint
//   "*"      zero or more codepoints, except path separators
//   "**"     zero or more codepoints including separators; must span a full
//            path segment (i.e. "a/**/b", not "a**b")
//   "[ab]"   character class, ranges ("[a-z]") and negation ("[!ab]"/"[^ab]")
//   "{a,b}"  brace alternation, nested up to 10 levels
//   "!"      negates the whole pattern when leading (repeatable)
//   "\"      escapes any special character
//
// Performance characteristics (mirrors bun, not a mechanical port of an
// NFA/regex approach):
//   - "*" / "**" use the iterative two-pointer backtracking scheme (wildcard +
//     globstar resume points) — no recursion, linear-controlled backtracking,
//     no exponential blowup on adversarial star runs.
//   - Braces use a fixed-depth stack (MAX_BRACE_DEPTH = 10, like bun); the
//     only recursion is per brace branch and is bounded by that same stack
//     (push fails when full), so recursion depth <= 10. A global
//     BRACE_BRANCH_BUDGET caps total branch alternatives explored, so
//     sequential groups ("{a,b}{c,d}"...) cannot multiply without bound;
//     patterns exceeding the budget fail to match (bun semantics).
//   - Zero heap allocation: state is a handful of integers plus a
//     std::array-backed brace stack.
//
// Strings are treated as WTF-8 bytes (lone surrogates allowed), matching what
// bun hands its matcher.
export module mbun.glob;

import std;

namespace mbun::glob {

namespace detail {

constexpr bool IS_WINDOWS{
#ifdef _WIN32
    true
#else
    false
#endif
};

// bun: bun_paths::is_sep_native — '/' everywhere, plus '\\' on windows.
[[nodiscard]] constexpr auto is_separator(char c) -> bool {
    return c == '/' || (IS_WINDOWS && c == '\\');
}

// ── WTF-8 helpers (semantics of bun_core strings) ──────────────────────────

// Length of the byte sequence introduced by `firstByte`; continuation or
// invalid lead bytes count as length 1.
[[nodiscard]] constexpr auto wtf8_sequence_length(unsigned char firstByte) -> std::uint32_t {
    if (firstByte >= 0xC0 && firstByte <= 0xDF) {
        return 2;
    }
    if (firstByte >= 0xE0 && firstByte <= 0xEF) {
        return 3;
    }
    if (firstByte >= 0xF0 && firstByte <= 0xF7) {
        return 4;
    }
    return 1;
}

constexpr std::uint32_t REPLACEMENT{0xFFFD};

struct Rune {
    std::uint32_t codepoint;
    std::uint32_t length;  // from the lead byte, even when invalid
};

// Decodes the WTF-8 codepoint at bytes[idx] (surrogate codepoints are legal;
// malformed sequences yield U+FFFD). Mirrors bun's decode_wtf8_rune_t.
[[nodiscard]] constexpr auto decode_wtf8_at(std::string_view bytes, std::uint32_t idx) -> Rune {
    // ASCII / lead-byte fast path (the overwhelming common case): read only the
    // first byte and return, skipping the 4-byte gather that dominated this
    // function in profiling.
    const unsigned char b0{static_cast<unsigned char>(bytes[idx])};
    const std::uint32_t len{wtf8_sequence_length(b0)};
    if (len == 1) {
        return {b0, 1};
    }

    std::array<unsigned char, 4> p{b0, 0, 0, 0};
    const std::size_t n{std::min<std::size_t>(len, bytes.size() - idx)};
    for (std::size_t i{1}; i < n; ++i) {
        p[i] = static_cast<unsigned char>(bytes[idx + i]);
    }

    if ((p[1] & 0xC0) != 0x80) {
        return {REPLACEMENT, len};
    }
    if (len == 2) {
        const std::uint32_t cp{(static_cast<std::uint32_t>(p[0] & 0x1F) << 6)
                               | static_cast<std::uint32_t>(p[1] & 0x3F)};
        return {cp < 0x80 ? REPLACEMENT : cp, len};
    }
    if ((p[2] & 0xC0) != 0x80) {
        return {REPLACEMENT, len};
    }
    if (len == 3) {
        const std::uint32_t cp{(static_cast<std::uint32_t>(p[0] & 0x0F) << 12)
                               | (static_cast<std::uint32_t>(p[1] & 0x3F) << 6)
                               | static_cast<std::uint32_t>(p[2] & 0x3F)};
        return {cp < 0x800 ? REPLACEMENT : cp, len};
    }
    if ((p[3] & 0xC0) != 0x80) {
        return {REPLACEMENT, len};
    }
    const std::uint32_t cp{(static_cast<std::uint32_t>(p[0] & 0x07) << 18)
                           | (static_cast<std::uint32_t>(p[1] & 0x3F) << 12)
                           | (static_cast<std::uint32_t>(p[2] & 0x3F) << 6)
                           | static_cast<std::uint32_t>(p[3] & 0x3F)};
    return {(cp < 0x10000 || cp > 0x10FFFF) ? REPLACEMENT : cp, len};
}

// ── matcher state ───────────────────────────────────────────────────────────

// Upper bound on brace-branch alternatives explored per match() call.
// Sequential brace groups multiply, so without a cap an adversarial pattern
// of ten sequential 10-way groups would explore 10^10 alternatives.
constexpr std::uint32_t BRACE_BRANCH_BUDGET{10'000};
constexpr std::uint32_t MAX_BRACE_DEPTH{10};

struct Wildcard {
    std::uint32_t globIndex{0};
    std::uint32_t pathIndex{0};
    std::uint8_t braceDepth{0};
};

struct State {
    std::uint32_t pathIndex{0};
    std::uint32_t globIndex{0};
    Wildcard wildcard{};
    Wildcard globstar{};
    std::uint8_t braceDepth{0};

    constexpr void backtrack() {
        pathIndex = wildcard.pathIndex;
        globIndex = wildcard.globIndex;
        braceDepth = wildcard.braceDepth;
    }

    // After a "**" segment: arm the wildcard to resume just past the next
    // path separator, and remember it as the enclosing globstar.
    constexpr void skip_to_separator(std::string_view path, bool isEndInvalid) {
        if (pathIndex == path.size()) {
            wildcard.pathIndex += 1;
            return;
        }
        std::uint32_t index{pathIndex};
        while (index < path.size() && !is_separator(path[index])) {
            ++index;
        }
        if (isEndInvalid || index != path.size()) {
            ++index;
        }
        wildcard.pathIndex = index;
        globstar = wildcard;
    }
};

struct Brace {
    std::uint32_t openBraceIdx{0};
    std::uint32_t branchIdx{0};
};

struct BraceStack {
    std::array<Brace, MAX_BRACE_DEPTH> items{};
    std::uint32_t length{0};
};

struct MatchContext {
    std::string_view glob;
    std::string_view path;
    BraceStack braceStack{};
    std::uint32_t braceBudget{BRACE_BRANCH_BUDGET};
};

// Unescapes a single-byte pattern character in place ("\n" and friends map to
// control characters, anything else maps to itself). Returns false on a
// trailing backslash (invalid pattern).
constexpr auto unescape(unsigned char& c, std::string_view glob, std::uint32_t& globIndex)
    -> bool {
    if (c != '\\') {
        return true;
    }
    ++globIndex;
    if (globIndex >= glob.size()) {
        return false;  // invalid pattern
    }
    switch (glob[globIndex]) {
        case 'a': c = 0x61; break;
        case 'b': c = 0x08; break;
        case 'n': c = '\n'; break;
        case 'r': c = '\r'; break;
        case 't': c = '\t'; break;
        default: c = static_cast<unsigned char>(glob[globIndex]);
    }
    return true;
}

// Character-class variant of unescape: yields the full codepoint (and its
// byte length in `glob`) so classes match non-ASCII codepoints.
constexpr auto get_unicode(std::uint32_t& c, std::uint32_t& cLen, std::string_view glob,
                           std::uint32_t& globIndex) -> bool {
    if (c <= 0x7F && c != '\\') {
        return true;
    }
    if (c == '\\') {
        ++globIndex;
        if (globIndex >= glob.size()) {
            return false;  // invalid pattern
        }
        switch (glob[globIndex]) {
            case 'a': c = 0x61; return true;
            case 'b': c = 0x08; return true;
            case 'n': c = '\n'; return true;
            case 'r': c = '\r'; return true;
            case 't': c = '\t'; return true;
            default: break;
        }
    }
    const Rune rune{decode_wtf8_at(glob, globIndex)};
    c = rune.codepoint;
    cLen = rune.length;
    return true;
}

// Coalesces consecutive "**/" segments ("**/**/**" behaves as one "**").
constexpr void skip_globstars(std::string_view glob, std::uint32_t& globIndex) {
    globIndex += 2;
    while (globIndex + 4 <= glob.size() && glob.substr(globIndex, 4) == "/**/") {
        globIndex += 3;
    }
    if (globIndex + 3 == glob.size() && glob.substr(globIndex, 3) == "/**") {
        globIndex += 3;
    }
    globIndex -= 2;
}

// Skips the rest of the current brace branch: advances past the matching "}"
// of the innermost entered group. Bracket classes hide "{,}" characters, and
// nesting scanned over (not entered) is tracked locally.
constexpr void skip_branch(State& state, std::string_view glob) {
    bool inBrackets{false};
    std::uint32_t nested{0};
    while (state.globIndex < glob.size()) {
        switch (glob[state.globIndex]) {
            case '{':
                if (!inBrackets) {
                    ++nested;
                }
                break;
            case '}':
                if (!inBrackets) {
                    if (nested == 0) {
                        --state.braceDepth;
                        ++state.globIndex;
                        return;
                    }
                    --nested;
                }
                break;
            case '[':
                inBrackets = true;
                break;
            case ']':
                inBrackets = false;
                break;
            case '\\':
                ++state.globIndex;
                break;
            default:
                break;
        }
        ++state.globIndex;
    }
}

constexpr auto match_brace(State& state, MatchContext& ctx) -> bool;

// `globStart` is where the currently matched (sub)pattern begins — used by
// the "**" full-segment check so a branch like "**/b" inside "{**/a,**/b}"
// doesn't peek at bytes before its own start.
constexpr auto match_impl(State& state, MatchContext& ctx, std::uint32_t globStart) -> bool {
    const std::string_view glob{ctx.glob};
    const std::string_view path{ctx.path};

    while (state.globIndex < glob.size() || state.pathIndex < path.size()) {
        if (state.globIndex < glob.size()) {
            const char ch{glob[state.globIndex]};
            // Flow after inspecting one pattern character:
            //   NextIter  — consumed, restart the main loop
            //   Literal   — try to match `ch` as a literal character
            //   Backtrack — mismatch, fall through to wildcard backtracking
            enum class Flow : std::uint8_t { NextIter, Literal, Backtrack };
            Flow flow{Flow::Backtrack};

            switch (ch) {
                case '*': {
                    const bool isGlobstar{state.globIndex + 1 < glob.size()
                                          && glob[state.globIndex + 1] == '*'};
                    if (isGlobstar) {
                        skip_globstars(glob, state.globIndex);
                    }

                    state.wildcard.globIndex = state.globIndex;
                    state.wildcard.pathIndex =
                        state.pathIndex
                        + (state.pathIndex < path.size()
                               ? wtf8_sequence_length(
                                     static_cast<unsigned char>(path[state.pathIndex]))
                               : 1);
                    state.wildcard.braceDepth = state.braceDepth;

                    bool inGlobstar{false};
                    bool skipTail{false};
                    if (isGlobstar) {
                        state.globIndex += 2;
                        const bool isEndInvalid{state.globIndex < glob.size()};

                        // Path exhausted with exactly "/*" left: neither the
                        // separator nor the trailing "*" can consume it, so
                        // skip the globstar arming (bun bug-fix parity).
                        if (isEndInvalid && state.pathIndex == path.size()
                            && glob.size() - state.globIndex == 2
                            && is_separator(glob[state.globIndex])
                            && glob[state.globIndex + 1] == '*') {
                            skipTail = true;
                        } else if ((state.globIndex < globStart + 3
                                    || glob[state.globIndex - 3] == '/')
                                   && (!isEndInvalid || glob[state.globIndex] == '/')) {
                            // "**" spans a full segment: treat as globstar.
                            if (isEndInvalid) {
                                state.globIndex += 1;
                            }
                            state.skip_to_separator(path, isEndInvalid);
                            inGlobstar = true;
                        }
                    } else {
                        state.globIndex += 1;
                    }

                    // A plain "*" can never cross a separator; resume from the
                    // enclosing globstar instead (no early globstar lock-in).
                    if (!skipTail && !inGlobstar && state.pathIndex < path.size()
                        && is_separator(path[state.pathIndex])) {
                        state.wildcard = state.globstar;
                    }
                    flow = Flow::NextIter;
                    break;
                }
                case '?':
                    if (state.pathIndex < path.size()) {
                        if (!is_separator(path[state.pathIndex])) {
                            state.globIndex += 1;
                            state.pathIndex += wtf8_sequence_length(
                                static_cast<unsigned char>(path[state.pathIndex]));
                            flow = Flow::NextIter;
                        }
                        // separator: Backtrack
                    } else {
                        flow = Flow::Literal;
                    }
                    break;
                case '[': {
                    if (state.pathIndex >= path.size()) {
                        flow = Flow::Literal;
                        break;
                    }
                    state.globIndex += 1;

                    bool classNegated{false};
                    if (state.globIndex < glob.size()
                        && (glob[state.globIndex] == '^' || glob[state.globIndex] == '!')) {
                        classNegated = true;
                        state.globIndex += 1;
                    }

                    bool first{true};
                    bool isMatch{false};
                    const Rune target{decode_wtf8_at(path, state.pathIndex)};

                    while (state.globIndex < glob.size()
                           && (first || glob[state.globIndex] != ']')) {
                        std::uint32_t low{
                            static_cast<unsigned char>(glob[state.globIndex])};
                        std::uint32_t lowLen{1};
                        if (!get_unicode(low, lowLen, glob, state.globIndex)) {
                            return false;  // invalid pattern
                        }
                        state.globIndex += lowLen;

                        std::uint32_t high{low};
                        if (state.globIndex + 1 < glob.size() && glob[state.globIndex] == '-'
                            && glob[state.globIndex + 1] != ']') {
                            state.globIndex += 1;
                            high = static_cast<unsigned char>(glob[state.globIndex]);
                            std::uint32_t highLen{1};
                            if (!get_unicode(high, highLen, glob, state.globIndex)) {
                                return false;  // invalid pattern
                            }
                            state.globIndex += highLen;
                        }

                        if (low <= target.codepoint && target.codepoint <= high) {
                            isMatch = true;
                        }
                        first = false;
                    }

                    if (state.globIndex >= glob.size()) {
                        return false;  // invalid pattern: unterminated class
                    }
                    state.globIndex += 1;
                    if (isMatch != classNegated) {
                        state.pathIndex += target.length;
                        flow = Flow::NextIter;
                    }
                    // else: Backtrack
                    break;
                }
                case '{': {
                    // Re-entry via wildcard backtracking: resume the branch
                    // this group is currently exploring instead of restarting.
                    bool reentered{false};
                    for (std::uint32_t i{0}; i < ctx.braceStack.length; ++i) {
                        if (ctx.braceStack.items[i].openBraceIdx == state.globIndex) {
                            state.globIndex = ctx.braceStack.items[i].branchIdx;
                            state.braceDepth += 1;
                            reentered = true;
                            break;
                        }
                    }
                    if (reentered) {
                        flow = Flow::NextIter;
                        break;
                    }
                    return match_brace(state, ctx);
                }
                case ',':
                case '}':
                    if (state.braceDepth > 0) {
                        // Branch matched up to here; continue past the group.
                        skip_branch(state, glob);
                        flow = Flow::NextIter;
                    } else {
                        flow = Flow::Literal;  // plain character outside braces
                    }
                    break;
                default:
                    flow = Flow::Literal;
                    break;
            }

            if (flow == Flow::NextIter) {
                continue;
            }
            if (flow == Flow::Literal && state.pathIndex < path.size()) {
                unsigned char cc{static_cast<unsigned char>(ch)};
                if (!unescape(cc, glob, state.globIndex)) {
                    return false;  // invalid pattern: trailing backslash
                }
                const std::uint32_t ccLen{wtf8_sequence_length(cc)};

                bool isMatch{};
                if (cc == '/') {
                    isMatch = is_separator(path[state.pathIndex]);
                } else if (ccLen > 1) {
                    // Multi-byte literal: compare the whole sequence. Inline the
                    // 2–4 byte compare rather than substr==substr, which lowered
                    // to a memcmp@plt call + AVX2 dispatch (per profiling, the
                    // hottest instruction on the CJK/emoji cases).
                    isMatch = state.pathIndex + ccLen <= path.size()
                              && state.globIndex + ccLen <= glob.size();
                    for (std::uint32_t i{0}; isMatch && i < ccLen; ++i) {
                        isMatch = path[state.pathIndex + i] == glob[state.globIndex + i];
                    }
                } else {
                    isMatch = static_cast<unsigned char>(path[state.pathIndex]) == cc;
                }

                if (isMatch) {
                    state.globIndex += ccLen;
                    state.pathIndex += ccLen;
                    if (cc == '/') {
                        // Crossing a separator re-arms the enclosing globstar.
                        state.wildcard = state.globstar;
                    }
                    continue;
                }
            }
        }

        // Mismatch: resume from the most recent wildcard, if it can still
        // consume more of the path.
        if (state.wildcard.pathIndex > 0 && state.wildcard.pathIndex <= path.size()) {
            state.backtrack();
            continue;
        }
        return false;
    }
    return true;
}

// Tries one alternative of a brace group: matches the branch body plus the
// entire remaining pattern. Bounded by the brace stack (depth) and the branch
// budget (breadth).
constexpr auto match_brace_branch(State& state, MatchContext& ctx, std::uint32_t openBraceIndex,
                                  std::uint32_t branchIndex) -> bool {
    if (ctx.braceBudget == 0) {
        return false;
    }
    --ctx.braceBudget;

    if (ctx.braceStack.length >= MAX_BRACE_DEPTH) {
        return false;  // exceeded brace depth
    }
    ctx.braceStack.items[ctx.braceStack.length] = Brace{openBraceIndex, branchIndex};
    ++ctx.braceStack.length;

    State branchState{state};
    branchState.globIndex = branchIndex;
    branchState.braceDepth = static_cast<std::uint8_t>(ctx.braceStack.length);

    const bool matched{match_impl(branchState, ctx, branchIndex)};

    --ctx.braceStack.length;
    return matched;
}

// Scans the group that opens at state.globIndex and tries each top-level
// branch in order. Commas inside "[...]" classes are class members, not
// branch separators. An unclosed group matches nothing.
constexpr auto match_brace(State& state, MatchContext& ctx) -> bool {
    const std::string_view glob{ctx.glob};
    std::int64_t braceDepth{0};
    bool inBrackets{false};

    const std::uint32_t openBraceIndex{state.globIndex};
    std::uint32_t branchIndex{0};

    while (state.globIndex < glob.size()) {
        switch (glob[state.globIndex]) {
            case '{':
                if (!inBrackets) {
                    ++braceDepth;
                    if (braceDepth == 1) {
                        branchIndex = state.globIndex + 1;
                    }
                }
                break;
            case '}':
                if (!inBrackets) {
                    --braceDepth;
                    if (braceDepth == 0) {
                        return match_brace_branch(state, ctx, openBraceIndex, branchIndex);
                    }
                }
                break;
            case ',':
                if (braceDepth == 1 && !inBrackets) {
                    if (match_brace_branch(state, ctx, openBraceIndex, branchIndex)) {
                        return true;
                    }
                    branchIndex = state.globIndex + 1;
                }
                break;
            case '[':
                inBrackets = true;
                break;
            case ']':
                inBrackets = false;
                break;
            case '\\':
                ++state.globIndex;
                break;
            default:
                break;
        }
        ++state.globIndex;
    }
    return false;
}

}  // namespace detail

// Returns whether `path` matches the glob `pattern` (Bun.Glob.match
// semantics; see module header for the supported syntax).
export [[nodiscard]] constexpr auto match(std::string_view pattern, std::string_view path)
    -> bool {
    detail::State state{};

    bool negated{false};
    while (state.globIndex < pattern.size() && pattern[state.globIndex] == '!') {
        negated = !negated;
        ++state.globIndex;
    }

    detail::MatchContext ctx{pattern, path};
    const bool matched{detail::match_impl(state, ctx, 0)};
    return matched != negated;
}

// ── Bun.Glob.scan — real filesystem traversal (T2.9 scan) ───────────────────
//
// Port of bun's GlobWalker (src/glob/GlobWalker.rs). The full bun walker is an
// fd-based, allocation-tight state machine with symlink-cycle detection and a
// multi-index "active component set" so a single readdir evaluates every live
// pattern position. We keep the same *matching algorithm* — component split on
// path separators (no brace awareness, exactly like build_pattern_components),
// per-component dotfile rule (match_pattern_impl), and the eval_dir /
// eval_file / match_pattern_dir / match_pattern_file logic including the
// `**/X` boundary handling — but drive it over a recursive std::filesystem
// walk instead of raw fds. Directory pruning falls straight out of the active
// set: a subtree with no live component is never entered.
//
// Deviations from bun (documented, not silent):
//   - Braces spanning a separator ("{a,b/c}") are unsupported — same as bun,
//     which splits components on every separator before the matcher sees them.
//   - Symlink cycles are broken by an ancestor canonical-path check rather than
//     bun's (st_dev, st_ino) FollowedLink chain; broken-symlink error reporting
//     and the literal-named-symlink descent nuance are DEFERRED.

export struct ScanOptions {
    std::string cwd{};             // empty → std::filesystem::current_path()
    bool dot{false};               // include entries whose segment starts with '.'
    bool absolute{false};          // return absolute paths (else relative to cwd)
    bool followSymlinks{false};    // descend symlinked directories via wildcards
    bool onlyFiles{true};          // omit directories from results
};

namespace detail {

// One path component of the pattern (split on separators, like bun's
// build_pattern_components). `raw` excludes any trailing separator.
struct Component {
    std::string_view raw;
    bool trailingSep{false};
    enum class Kind : std::uint8_t { Normal, Double, Dot, DotBack } kind{Kind::Normal};
};

[[nodiscard]] constexpr auto starts_with_dot(std::string_view s) -> bool {
    return !s.empty() && s[0] == '.';
}

// Classifies a raw component slice. "**" is the globstar; "." / ".." are the
// relative-path markers bun tracks as Dot / DotBack.
[[nodiscard]] constexpr auto classify_component(std::string_view raw) -> Component::Kind {
    if (raw == "**") {
        return Component::Kind::Double;
    }
    if (raw == ".") {
        return Component::Kind::Dot;
    }
    if (raw == "..") {
        return Component::Kind::DotBack;
    }
    return Component::Kind::Normal;
}

// Splits `pattern` into components on native separators, honoring "\\" escapes
// exactly like build_pattern_components (a backslash escapes the next byte, so
// an escaped separator does not split). Empty components (leading/duplicate
// separators) are dropped, as are leading "." markers (a "./" prefix refers to
// the walk root and consumes no path segment).
[[nodiscard]] inline auto split_components(std::string_view pattern) -> std::vector<Component> {
    std::vector<Component> out;
    std::size_t start{0};
    bool prevBackslash{false};
    for (std::size_t i{0}; i < pattern.size(); ++i) {
        const char c{pattern[i]};
        if (is_separator(c) && !prevBackslash) {
            // trailing_sep is set only for a component whose separator ends the
            // whole pattern ("src/" → directory-only), matching make_component.
            const bool endsPattern{i + 1 == pattern.size()};
            std::string_view raw{pattern.substr(start, i - start)};
            if (!raw.empty()) {
                out.push_back({raw, /*trailingSep=*/endsPattern, classify_component(raw)});
            }
            start = i + 1;
            prevBackslash = false;
            continue;
        }
        prevBackslash = (c == '\\') && !prevBackslash;
    }
    std::string_view tail{pattern.substr(start)};
    if (!tail.empty()) {
        out.push_back({tail, /*trailingSep=*/false, classify_component(tail)});
    }

    // Drop leading Dot markers: "./x" ≡ "x" for the walk root.
    std::size_t lead{0};
    while (lead < out.size() && out[lead].kind == Component::Kind::Dot) {
        ++lead;
    }
    if (lead > 0) {
        out.erase(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(lead));
    }
    return out;
}

// Bounded traversal state: the pattern components plus the resolved scan
// options. Matching mirrors GlobWalker's eval_* methods; the recursion walks
// the real filesystem.
class Walker {
private:
    const std::vector<Component>& comps_;
    const ScanOptions& opts_;
    std::filesystem::path root_;
    std::vector<std::string>& results_;
    std::unordered_set<std::string>& seen_;
    std::vector<std::filesystem::path> ancestors_;  // canonical dirs on the stack

public:
    Walker(const std::vector<Component>& comps, const ScanOptions& opts,
           std::filesystem::path root, std::vector<std::string>& results,
           std::unordered_set<std::string>& seen)
        : comps_{comps}, opts_{opts}, root_{std::move(root)}, results_{results}, seen_{seen} {}

    void run() {
        if (comps_.empty()) {
            return;
        }
        std::vector<std::uint32_t> active{normalize_idx_(0)};
        walk_(root_, "", active);
    }

private:
    [[nodiscard]] auto last_idx_() const -> std::uint32_t {
        return static_cast<std::uint32_t>(comps_.size() - 1);
    }

    // Collapse a run of successive "**" so the active index lands on the last
    // globstar of the chain (collapse_successive_double_wildcards).
    [[nodiscard]] auto normalize_idx_(std::uint32_t idx) const -> std::uint32_t {
        if (idx < comps_.size() && comps_[idx].kind == Component::Kind::Double) {
            std::uint32_t i{idx};
            while (i + 1 < comps_.size() && comps_[i + 1].kind == Component::Kind::Double) {
                ++i;
            }
            return i;
        }
        return idx;
    }

    static void set_insert_(std::vector<std::uint32_t>& set, std::uint32_t v) {
        for (const std::uint32_t e : set) {
            if (e == v) {
                return;
            }
        }
        set.push_back(v);
    }

    // match_pattern_impl: the per-component matcher with the dotfile rule. A
    // segment starting with '.' only matches when dot is enabled or the pattern
    // component itself starts with a literal '.'.
    [[nodiscard]] auto match_impl_(const Component& c, std::string_view name) const -> bool {
        if (!opts_.dot && starts_with_dot(name) && !starts_with_dot(c.raw)) {
            return false;
        }
        return match(c.raw, name);
    }

    // match_pattern_dir: does the directory `name` match component `idx`? On a
    // match returns the index bump to apply (child = normalize(idx+bump)); an
    // empty optional means no descent for this index. `add` is set when the
    // directory itself is a terminal match.
    [[nodiscard]] auto match_dir_(std::uint32_t idx, std::string_view name, bool hidden,
                                  bool& add) const -> std::optional<std::uint32_t> {
        const Component& c{comps_[idx]};
        const bool isLast{idx == last_idx_()};

        if (c.kind == Component::Kind::Double) {
            if (!isLast && match_impl_(comps_[idx + 1], name)) {
                if (idx + 1 == last_idx_()) {
                    add = true;
                    if (hidden) {
                        return std::nullopt;
                    }
                    return 0;
                }
                return 2;
            }
            if (hidden) {
                return std::nullopt;
            }
            if (isLast) {
                add = true;
            }
            return 0;
        }

        if (match_impl_(c, name)) {
            if (isLast) {
                add = true;
                return std::nullopt;  // terminal literal dir: matched, don't descend
            }
            return 1;
        }
        return std::nullopt;
    }

    // eval_dir: fold every active index over `name`, returning the child active
    // set and whether the directory is a terminal match.
    [[nodiscard]] auto eval_dir_(const std::vector<std::uint32_t>& active, std::string_view name,
                                 bool hidden, bool& add) const -> std::vector<std::uint32_t> {
        std::vector<std::uint32_t> child;
        for (const std::uint32_t idx : active) {
            bool addThis{false};
            const std::optional<std::uint32_t> bump{match_dir_(idx, name, hidden, addThis)};
            if (bump) {
                set_insert_(child, normalize_idx_(idx + *bump));
                // At a `**/X` boundary keep the outer `**` alive unless idx+2 is
                // itself `**` or the entry is hidden (a `**` never enters a dotdir).
                if (*bump == 2 && !hidden
                    && comps_[idx + 2].kind != Component::Kind::Double) {
                    set_insert_(child, idx);
                }
            }
            if (addThis) {
                add = true;
            }
        }
        return child;
    }

    // match_pattern_file / eval_file: a file matches when it satisfies the last
    // component, or the `**`+next-is-last shortcut (case b).
    [[nodiscard]] auto eval_file_(const std::vector<std::uint32_t>& active,
                                  std::string_view name) const -> bool {
        for (const std::uint32_t idx : active) {
            const Component& c{comps_[idx]};
            if (c.trailingSep) {
                continue;
            }
            const bool isLast{idx == last_idx_()};
            if (!isLast) {
                if (c.kind == Component::Kind::Double && idx + 1 == last_idx_()
                    && comps_[idx + 1].kind != Component::Kind::Double
                    && match_impl_(comps_[idx + 1], name)) {
                    return true;
                }
                continue;
            }
            if (match_impl_(c, name)) {
                return true;
            }
        }
        return false;
    }

    void emit_(std::string_view rel) {
        std::string out;
        if (opts_.absolute) {
            std::filesystem::path p{root_};
            p /= std::filesystem::path{rel};
            out = p.generic_string();
        } else {
            out = std::string{rel};
        }
        if (seen_.insert(out).second) {
            results_.push_back(std::move(out));
        }
    }

    void walk_(const std::filesystem::path& dirAbs, const std::string& rel,
               const std::vector<std::uint32_t>& active) {
        std::error_code ec;
        std::filesystem::directory_iterator it{dirAbs, ec};
        if (ec) {
            return;  // unreadable dir: skip (bun logs + continues)
        }
        const std::filesystem::directory_iterator end{};
        for (; it != end; it.increment(ec)) {
            if (ec) {
                break;
            }
            const std::filesystem::directory_entry& entry{*it};
            const std::string name{entry.path().filename().string()};
            const bool hidden{!opts_.dot && starts_with_dot(name)};

            std::error_code sec;
            const std::filesystem::file_status lst{entry.symlink_status(sec)};
            if (sec) {
                continue;
            }
            const bool isSymlink{std::filesystem::is_symlink(lst)};
            bool isDir{false};
            if (isSymlink) {
                if (opts_.followSymlinks) {
                    const std::filesystem::file_status st{entry.status(sec)};
                    isDir = !sec && std::filesystem::is_directory(st);
                }
            } else {
                isDir = std::filesystem::is_directory(lst);
            }

            const std::string childRel{rel.empty() ? name : rel + "/" + name};

            if (isDir) {
                bool add{false};
                const std::vector<std::uint32_t> child{eval_dir_(active, name, hidden, add)};
                if (add && !opts_.onlyFiles) {
                    emit_(childRel);
                }
                if (!child.empty() && descend_ok_(entry, isSymlink)) {
                    walk_(entry.path(), childRel, child);
                    if (isSymlink && opts_.followSymlinks) {
                        ancestors_.pop_back();
                    }
                }
            } else {
                if (eval_file_(active, name)) {
                    emit_(childRel);
                }
            }
        }
    }

    // Symlink-cycle guard: before descending a followed symlink, canonicalize
    // its target and refuse if it is already an ancestor on the current branch.
    // Non-symlink dirs (or when not following) always descend.
    [[nodiscard]] auto descend_ok_(const std::filesystem::directory_entry& entry, bool isSymlink)
        -> bool {
        if (!isSymlink || !opts_.followSymlinks) {
            return true;
        }
        std::error_code ec;
        std::filesystem::path canon{std::filesystem::canonical(entry.path(), ec)};
        if (ec) {
            return false;
        }
        for (const std::filesystem::path& a : ancestors_) {
            if (a == canon) {
                return false;
            }
        }
        ancestors_.push_back(std::move(canon));
        return true;
    }
};

}  // namespace detail

// Walks the filesystem under `opts.cwd` (default: the process cwd) and returns
// every entry matching `pattern` (Bun.Glob.scan semantics; see ScanOptions and
// the detail::Walker header for supported syntax and documented deviations).
// Paths use '/' separators and are relative to cwd unless `opts.absolute`.
export [[nodiscard]] inline auto scan(std::string_view pattern, const ScanOptions& opts = {})
    -> std::vector<std::string> {
    std::vector<std::string> results;
    const std::vector<detail::Component> comps{detail::split_components(pattern)};
    if (comps.empty()) {
        return results;
    }

    std::error_code ec;
    std::filesystem::path root{opts.cwd.empty() ? std::filesystem::current_path(ec)
                                                : std::filesystem::path{opts.cwd}};
    if (ec) {
        return results;
    }

    std::unordered_set<std::string> seen;
    detail::Walker walker{comps, opts, std::move(root), results, seen};
    walker.run();
    return results;
}

}  // namespace mbun::glob
