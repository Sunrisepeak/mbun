// defines.cppm — mbun.bundler.defines: compile-time identifier substitution.
//
// bun's bundler resolves a table of "defines" — dotted identifier chains mapped
// to a replacement expression — while printing each module. Two user-facing
// surfaces feed that table:
//
//   `bun build --define K=V`     -> one entry per K (ref: src/cli/Arguments.rs
//                                  :1900 `--define`, src/bundler/options.rs
//                                  `create_defines`)
//   `bun build --env inline`     -> one `process.env.<NAME>` entry per variable
//   `Bun.build({ env: "..." })`     in the build-time environment
//                                  (ref: src/bundler/defines.rs
//                                  `copy_env_for_define` :105-180, which literally
//                                  synthesises the key `process.env.` ++ NAME and
//                                  stores the value as a quoted string).
//
// mbun's pipeline is a source-erasure transpiler, not an AST printer, so the
// substitution happens on the source text: the module is tokenized with
// mbun.js_lexer (so strings, comments, regexes and template literals can never
// match), the longest dotted identifier chain at each position is looked up, and
// the matching byte range is rewritten. That gives the same observable result as
// bun's printer-level replacement for the shapes a define can take.
//
// CONSERVATISM RULE (same asymmetry as js_parser/subst.cppm): every "don't know"
// answers NO SUBSTITUTION. A missed define prints the original expression, which
// merely keeps today's runtime behaviour; a wrong one silently changes program
// meaning. Hence the guards below reject assignment targets, property keys,
// declaration names and anything reached through `.`/`?.`.
export module mbun.bundler.defines;

import std;
import mbun.js_lexer;

export namespace mbun::bundler {

// One define: the dotted chain to match, split into segments, and the literal
// text substituted for it. `process.env.FOO` -> {"process","env","FOO"}.
struct DefineEntry {
    std::vector<std::string> path;
    std::string replacement;
};

// The define table. Entries are matched longest-chain-first, so a table holding
// both `process.env` and `process.env.FOO` behaves like bun's (the more specific
// key wins).
class DefineTable {
private:
    std::vector<DefineEntry> entries_;
    std::size_t maxSegments_{0};

public:
    DefineTable() = default;

    [[nodiscard]] bool empty() const { return entries_.empty(); }
    [[nodiscard]] std::size_t size() const { return entries_.size(); }
    [[nodiscard]] const std::vector<DefineEntry>& entries() const { return entries_; }
    [[nodiscard]] std::size_t max_segments() const { return maxSegments_; }

    // Insert `dottedKey` -> `replacement`. A key with an empty segment (e.g. "a..b")
    // is rejected: bun's define parser requires a valid identifier chain.
    // A repeated key overwrites, matching the last-wins order of bun's RawDefines map.
    bool insert(std::string_view dottedKey, std::string_view replacement) {
        std::vector<std::string> path{};
        std::size_t begin{0};
        while (true) {
            const std::size_t dot{dottedKey.find('.', begin)};
            const std::string_view segment{dottedKey.substr(
                begin, dot == std::string_view::npos ? std::string_view::npos : dot - begin)};
            if (segment.empty()) return false;
            path.emplace_back(segment);
            if (dot == std::string_view::npos) break;
            begin = dot + 1;
        }
        for (DefineEntry& existing : entries_) {
            if (existing.path == path) {
                existing.replacement = std::string{replacement};
                return true;
            }
        }
        maxSegments_ = std::max(maxSegments_, path.size());
        entries_.push_back(DefineEntry{std::move(path), std::string{replacement}});
        return true;
    }

    // Longest match for the chain `segments` (already the maximal run at this
    // position). Returns the number of segments consumed plus the replacement.
    [[nodiscard]] std::optional<std::pair<std::size_t, std::string_view>> lookup(
        std::span<const std::string_view> segments) const {
        std::size_t bestLen{0};
        std::string_view best{};
        for (const DefineEntry& entry : entries_) {
            if (entry.path.size() > segments.size() || entry.path.size() <= bestLen) continue;
            bool same{true};
            for (std::size_t i{0}; i < entry.path.size(); ++i) {
                if (entry.path[i] != segments[i]) {
                    same = false;
                    break;
                }
            }
            if (!same) continue;
            bestLen = entry.path.size();
            best = entry.replacement;
        }
        if (bestLen == 0) return std::nullopt;
        return std::pair<std::size_t, std::string_view>{bestLen, best};
    }
};

// Quote `value` as a JS string literal — the form bun stores an inlined
// environment variable in (defines.rs `env_string_store_put` writes the value
// through the JSON string printer).
inline std::string quote_js_string(std::string_view value) {
    std::string out{};
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const unsigned char c : value) {
        switch (c) {
        case '\\': out.append("\\\\"); break;
        case '"': out.append("\\\""); break;
        case '\n': out.append("\\n"); break;
        case '\r': out.append("\\r"); break;
        case '\t': out.append("\\t"); break;
        case '\b': out.append("\\b"); break;
        case '\f': out.append("\\f"); break;
        default:
            if (c < 0x20) {
                out.append(std::format("\\u{:04x}", c));
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
    return out;
}

namespace detail {

// Regex-vs-division table (same one as vertical_slice.cppm's `regex_allowed_after`;
// duplicated rather than exported from there because this module must not depend
// on the bundler driver).
inline bool regex_allowed_after_token(mbun::js_lexer::Token prev) {
    using Token = mbun::js_lexer::Token;
    switch (prev) {
    case Token::Identifier:
    case Token::EscapedKeyword:
    case Token::PrivateIdentifier:
    case Token::NumericLiteral:
    case Token::BigIntegerLiteral:
    case Token::StringLiteral:
    case Token::RegExpLiteral:
    case Token::NoSubstitutionTemplateLiteral:
    case Token::TemplateTail:
    case Token::CloseParen:
    case Token::CloseBracket:
    case Token::This:
    case Token::Super:
    case Token::True:
    case Token::False:
    case Token::Null:
    case Token::PlusPlus:
    case Token::MinusMinus:
        return false;
    default:
        return true;
    }
}

struct Span {
    mbun::js_lexer::Token kind{mbun::js_lexer::Token::EndOfFile};
    std::uint32_t begin{0};
    std::uint32_t end{0};
    std::string_view raw{};
};

// Tokenize with template/regex context, exactly like the bundler's own scanner.
// A lexical error yields nullopt, which the caller turns into "leave the source
// alone" (the parser downstream will report the real diagnostic).
inline std::optional<std::vector<Span>> tokenize(std::string_view code) {
    using Token = mbun::js_lexer::Token;
    mbun::js_lexer::Lexer lexer{code};
    std::vector<Span> tokens{};
    tokens.reserve(code.size() / 4 + 1);
    std::vector<char> braceStack{};
    Token previous{Token::EndOfFile};
    while (true) {
        auto next{lexer.next()};
        if (!next) return std::nullopt;
        Token kind{*next};
        if (kind == Token::EndOfFile) break;
        if ((kind == Token::Slash || kind == Token::SlashEquals) &&
            regex_allowed_after_token(previous)) {
            if (!lexer.scan_regexp()) return std::nullopt;
            kind = lexer.token();
        }
        if (kind == Token::OpenBrace) {
            braceStack.push_back('{');
        } else if (kind == Token::CloseBrace) {
            if (!braceStack.empty() && braceStack.back() == 't') {
                braceStack.pop_back();
                if (!lexer.rescan_close_brace_as_template_token()) return std::nullopt;
                kind = lexer.token();
            } else if (!braceStack.empty()) {
                braceStack.pop_back();
            }
        }
        if (kind == Token::TemplateHead || kind == Token::TemplateMiddle) {
            braceStack.push_back('t');
        }
        tokens.push_back(Span{kind, static_cast<std::uint32_t>(lexer.start()),
                              static_cast<std::uint32_t>(lexer.end()), lexer.raw()});
        previous = kind;
    }
    return tokens;
}

// A chain may not START here when the identifier is reached through a member
// access, is the name being declared/bound, or is an object-literal / class
// member key. Those are the shapes where replacing the text would change what is
// declared rather than what is read.
inline bool chain_start_allowed(std::span<const Span> tokens, std::size_t i) {
    using Token = mbun::js_lexer::Token;
    if (i == 0) return true;
    switch (tokens[i - 1].kind) {
    case Token::Dot:
    case Token::QuestionDot:
    case Token::Var:
    case Token::Const:
    case Token::Function:
    case Token::Class:
    case Token::New:  // `new process.env.Foo()` — a constructor, not a value read
        return false;
    default: break;
    }
    // `let x` / `x:` — `let` is contextual (Identifier), so match on the text.
    if (tokens[i - 1].kind == Token::Identifier &&
        (tokens[i - 1].raw == "let" || tokens[i - 1].raw == "function")) {
        return false;
    }
    return true;
}

// A chain may not END here when what follows writes to it (assignment, update)
// or turns it into a binding/label.
inline bool chain_end_allowed(std::span<const Span> tokens, std::size_t last) {
    using Token = mbun::js_lexer::Token;
    if (last + 1 >= tokens.size()) return true;
    switch (tokens[last + 1].kind) {
    case Token::Equals:
    case Token::PlusEquals:
    case Token::MinusEquals:
    case Token::AsteriskEquals:
    case Token::AsteriskAsteriskEquals:
    case Token::SlashEquals:
    case Token::PercentEquals:
    case Token::AmpersandEquals:
    case Token::AmpersandAmpersandEquals:
    case Token::BarEquals:
    case Token::BarBarEquals:
    case Token::CaretEquals:
    case Token::QuestionQuestionEquals:
    case Token::LessThanLessThanEquals:
    case Token::GreaterThanGreaterThanEquals:
    case Token::GreaterThanGreaterThanGreaterThanEquals:
    case Token::PlusPlus:
    case Token::MinusMinus:
    case Token::Colon:
        return false;
    default:
        return true;
    }
}

}  // namespace detail

// Rewrite every define hit in `source` and return the new text. When the table is
// empty (the common case) or the source does not tokenize, the input is returned
// unchanged — substitution never introduces a build failure of its own.
inline std::string apply_defines(std::string_view source, const DefineTable& table) {
    using Token = mbun::js_lexer::Token;
    if (table.empty() || source.empty()) return std::string{source};

    const auto tokens{detail::tokenize(source)};
    if (!tokens) return std::string{source};
    const std::span<const detail::Span> spans{*tokens};

    std::string out{};
    out.reserve(source.size());
    std::size_t copied{0};

    std::vector<std::string_view> segments{};
    segments.reserve(table.max_segments() + 1);

    for (std::size_t i{0}; i < spans.size(); ++i) {
        if (spans[i].kind != Token::Identifier) continue;
        if (!detail::chain_start_allowed(spans, i)) continue;

        // Collect the maximal `a.b.c` run starting here, capped at the longest key
        // in the table (a longer run cannot match anything).
        segments.clear();
        segments.push_back(spans[i].raw);
        std::size_t last{i};
        while (segments.size() < table.max_segments() && last + 2 < spans.size() &&
               spans[last + 1].kind == Token::Dot && spans[last + 2].kind == Token::Identifier) {
            segments.push_back(spans[last + 2].raw);
            last += 2;
        }

        const auto hit{table.lookup(segments)};
        if (!hit) continue;
        const std::size_t consumed{hit->first};
        const std::size_t endToken{i + (consumed - 1) * 2};
        if (!detail::chain_end_allowed(spans, endToken)) continue;

        const std::size_t begin{spans[i].begin};
        const std::size_t end{spans[endToken].end};
        if (begin < copied) continue;  // overlapping match (cannot happen; be safe)
        out.append(source.substr(copied, begin - copied));
        out.append(hit->second);
        copied = end;
        i = endToken;
    }

    out.append(source.substr(copied));
    return out;
}

}  // namespace mbun::bundler
