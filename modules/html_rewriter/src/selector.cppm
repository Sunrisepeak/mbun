// selector.cppm — mbun.html_rewriter.selector: the CSS selector subset the
// HTMLRewriter engine matches against open elements.
//
// Supported grammar (the subset bun's HTMLRewriter test-suite exercises):
//   list       : complex ("," complex)*
//   complex    : compound ((">" | " ") compound)*        child / descendant
//   compound   : ("*" | tag)? simple*
//   simple     : "#" ident | "." ident | "[" attr "]" | ":" pseudo
//   attr       : name (("=" | "~=" | "|=" | "^=" | "$=" | "*=") value flag?)?
//   flag       : "i" | "s"                               value case sensitivity
//   pseudo     : first-child | nth-child(N) | first-of-type | nth-of-type(N)
//              | not(compound)
//
// Matching is done against a snapshot ancestry (document root … candidate), so
// the engine can evaluate descendant/child combinators and the structural
// pseudo-classes from its open-element stack without a DOM.
export module mbun.html_rewriter.selector;

import std;

export namespace mbun::html_rewriter {

// One parsed HTML attribute. Names are ASCII-lowercased at parse time (the
// HTML rule); values keep their original bytes. `hasValue` distinguishes a
// bare boolean attribute (`<div b>`) from an explicit empty one (`<div a="">`).
struct Attribute {
    std::string name;
    std::string value;
    bool hasValue { true };
};

// Snapshot of one open element as seen by the matcher. `nthChild` and
// `nthOfType` are 1-based positions among the parent's element children
// (all elements / same-tag elements respectively).
struct MatchContext {
    std::string_view tagName;
    const std::vector<Attribute>* attributes { nullptr };
    int nthChild { 0 };
    int nthOfType { 0 };
};

class Selector {
private:
    enum class AttrOp_ { PRESENT, EQUALS, INCLUDES, DASH_MATCH, PREFIX, SUFFIX, SUBSTRING };

    struct AttrCond_ {
        std::string name;
        AttrOp_ op { AttrOp_::PRESENT };
        std::string value;
        bool ignoreCase { false };
    };

    enum class PseudoKind_ { FIRST_CHILD, NTH_CHILD, FIRST_OF_TYPE, NTH_OF_TYPE, NOT };

    struct Compound_;

    struct PseudoCond_ {
        PseudoKind_ kind { PseudoKind_::FIRST_CHILD };
        int index { 0 };
        std::unique_ptr<Compound_> negated;
    };

    struct Compound_ {
        std::string tagName;  // empty = any
        std::vector<std::string> ids;
        std::vector<std::string> classes;
        std::vector<AttrCond_> attrConds;
        std::vector<PseudoCond_> pseudos;
    };

    enum class Combinator_ { DESCENDANT, CHILD };

    struct Step_ {
        Compound_ compound;
        Combinator_ next { Combinator_::DESCENDANT };  // relation to the following step
    };

    struct Complex_ {
        std::vector<Step_> steps;  // source order; steps.back() matches the candidate
    };

    std::vector<Complex_> alternatives_;

public:  // Big Five — parse trees hold unique_ptrs, so Selector is move-only.
    Selector() = default;
    Selector(const Selector&) = delete;
    Selector& operator=(const Selector&) = delete;
    Selector(Selector&&) = default;
    Selector& operator=(Selector&&) = default;
    ~Selector() = default;

public:
    static std::expected<Selector, std::string> parse(std::string_view text);

    // `ancestry` is the open-element chain, outermost first; the candidate
    // element is `ancestry.back()`.
    bool matches(std::span<const MatchContext> ancestry) const {
        if (ancestry.empty()) return false;
        for (const auto& complex : alternatives_) {
            if (complex_matches_(complex, ancestry)) return true;
        }
        return false;
    }

private:
    static bool ascii_ws_(char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
    }
    static char ascii_lower_(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }
    static std::string lower_copy_(std::string_view s) {
        std::string r { s };
        for (char& c : r) c = ascii_lower_(c);
        return r;
    }
    static bool value_eq_(std::string_view a, std::string_view b, bool ignoreCase) {
        if (a.size() != b.size()) return false;
        if (!ignoreCase) return a == b;
        for (std::size_t i { 0 }; i < a.size(); ++i) {
            if (ascii_lower_(a[i]) != ascii_lower_(b[i])) return false;
        }
        return true;
    }
    static bool value_has_prefix_(std::string_view s, std::string_view prefix, bool ignoreCase) {
        return s.size() >= prefix.size() && value_eq_(s.substr(0, prefix.size()), prefix, ignoreCase);
    }
    static bool value_has_suffix_(std::string_view s, std::string_view suffix, bool ignoreCase) {
        return s.size() >= suffix.size() &&
               value_eq_(s.substr(s.size() - suffix.size()), suffix, ignoreCase);
    }
    static bool value_contains_(std::string_view s, std::string_view needle, bool ignoreCase) {
        if (needle.empty()) return false;
        if (s.size() < needle.size()) return false;
        for (std::size_t i { 0 }; i + needle.size() <= s.size(); ++i) {
            if (value_eq_(s.substr(i, needle.size()), needle, ignoreCase)) return true;
        }
        return false;
    }

    static const Attribute* find_attr_(const MatchContext& e, std::string_view name) {
        if (e.attributes == nullptr) return nullptr;
        for (const auto& a : *e.attributes) {
            if (a.name == name) return &a;  // duplicates: the first one wins
        }
        return nullptr;
    }

    static bool attr_cond_matches_(const AttrCond_& cond, const MatchContext& e) {
        const Attribute* attr { find_attr_(e, cond.name) };
        if (attr == nullptr) return false;
        std::string_view v { attr->value };
        switch (cond.op) {
        case AttrOp_::PRESENT: return true;
        case AttrOp_::EQUALS: return value_eq_(v, cond.value, cond.ignoreCase);
        case AttrOp_::INCLUDES: {
            if (cond.value.empty()) return false;
            std::size_t i { 0 };
            while (i < v.size()) {
                while (i < v.size() && ascii_ws_(v[i])) ++i;
                std::size_t b { i };
                while (i < v.size() && !ascii_ws_(v[i])) ++i;
                if (b < i && value_eq_(v.substr(b, i - b), cond.value, cond.ignoreCase)) return true;
            }
            return false;
        }
        case AttrOp_::DASH_MATCH:
            return value_eq_(v, cond.value, cond.ignoreCase) ||
                   (value_has_prefix_(v, cond.value, cond.ignoreCase) &&
                    v.size() > cond.value.size() && v[cond.value.size()] == '-');
        case AttrOp_::PREFIX: return !cond.value.empty() && value_has_prefix_(v, cond.value, cond.ignoreCase);
        case AttrOp_::SUFFIX: return !cond.value.empty() && value_has_suffix_(v, cond.value, cond.ignoreCase);
        case AttrOp_::SUBSTRING: return value_contains_(v, cond.value, cond.ignoreCase);
        }
        return false;
    }

    static bool compound_matches_(const Compound_& c, const MatchContext& e) {
        if (!c.tagName.empty() && c.tagName != e.tagName) return false;
        for (const auto& id : c.ids) {
            const Attribute* attr { find_attr_(e, "id") };
            if (attr == nullptr || attr->value != id) return false;
        }
        for (const auto& cls : c.classes) {
            const Attribute* attr { find_attr_(e, "class") };
            if (attr == nullptr) return false;
            std::string_view v { attr->value };
            bool found { false };
            std::size_t i { 0 };
            while (i < v.size() && !found) {
                while (i < v.size() && ascii_ws_(v[i])) ++i;
                std::size_t b { i };
                while (i < v.size() && !ascii_ws_(v[i])) ++i;
                found = b < i && v.substr(b, i - b) == cls;
            }
            if (!found) return false;
        }
        for (const auto& cond : c.attrConds) {
            if (!attr_cond_matches_(cond, e)) return false;
        }
        for (const auto& p : c.pseudos) {
            switch (p.kind) {
            case PseudoKind_::FIRST_CHILD:
                if (e.nthChild != 1) return false;
                break;
            case PseudoKind_::NTH_CHILD:
                if (e.nthChild != p.index) return false;
                break;
            case PseudoKind_::FIRST_OF_TYPE:
                if (e.nthOfType != 1) return false;
                break;
            case PseudoKind_::NTH_OF_TYPE:
                if (e.nthOfType != p.index) return false;
                break;
            case PseudoKind_::NOT:
                if (p.negated != nullptr && compound_matches_(*p.negated, e)) return false;
                break;
            }
        }
        return true;
    }

    // steps[si] already matched ancestry[ai]; try to anchor the earlier steps.
    static bool match_rest_(const std::vector<Step_>& steps, std::size_t si,
                            std::span<const MatchContext> ancestry, std::size_t ai) {
        if (si == 0) return true;
        const Step_& prev { steps[si - 1] };
        if (prev.next == Combinator_::CHILD) {
            return ai > 0 && compound_matches_(prev.compound, ancestry[ai - 1]) &&
                   match_rest_(steps, si - 1, ancestry, ai - 1);
        }
        for (std::size_t j { ai }; j > 0; --j) {
            if (compound_matches_(prev.compound, ancestry[j - 1]) &&
                match_rest_(steps, si - 1, ancestry, j - 1)) {
                return true;
            }
        }
        return false;
    }

    static bool complex_matches_(const Complex_& complex, std::span<const MatchContext> ancestry) {
        if (complex.steps.empty()) return false;
        if (!compound_matches_(complex.steps.back().compound, ancestry.back())) return false;
        return match_rest_(complex.steps, complex.steps.size() - 1, ancestry, ancestry.size() - 1);
    }

    class Parser_;
};

class Selector::Parser_ {
private:
    std::string_view text_;
    std::size_t pos_ { 0 };

public:
    explicit Parser_(std::string_view text) : text_ { text } {}

    std::expected<std::vector<Complex_>, std::string> parse_list() {
        std::vector<Complex_> list;
        skip_ws_();
        if (at_end_()) return std::unexpected("Empty selector is not allowed.");
        for (;;) {
            auto complex { parse_complex_() };
            if (!complex) return std::unexpected(complex.error());
            list.push_back(std::move(*complex));
            skip_ws_();
            if (at_end_()) break;
            if (peek_() != ',') return std::unexpected(unexpected_input_());
            ++pos_;
            skip_ws_();
            if (at_end_()) return std::unexpected("The selector is missing after the comma.");
        }
        return list;
    }

private:
    bool at_end_() const { return pos_ >= text_.size(); }
    char peek_() const { return text_[pos_]; }
    void skip_ws_() {
        while (!at_end_() && ascii_ws_(peek_())) ++pos_;
    }
    std::string unexpected_input_() const {
        return std::format("Unexpected input in the selector at position {}.", pos_);
    }

    static bool ident_char_(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '-' || c == '_' || static_cast<unsigned char>(c) >= 0x80;
    }

    std::string read_ident_() {
        std::size_t b { pos_ };
        while (!at_end_() && ident_char_(peek_())) ++pos_;
        return std::string { text_.substr(b, pos_ - b) };
    }

    std::expected<Complex_, std::string> parse_complex_() {
        Complex_ complex;
        for (;;) {
            auto compound { parse_compound_() };
            if (!compound) return std::unexpected(compound.error());
            complex.steps.push_back(Step_ { std::move(*compound), Combinator_::DESCENDANT });
            // Combinator lookahead: whitespace alone is a descendant combinator
            // only when another compound follows (not before ',' or the end).
            std::size_t mark { pos_ };
            bool sawWs { !at_end_() && ascii_ws_(peek_()) };
            skip_ws_();
            if (at_end_() || peek_() == ',' || peek_() == ')') {
                pos_ = mark;
                return complex;
            }
            if (peek_() == '>') {
                ++pos_;
                skip_ws_();
                complex.steps.back().next = Combinator_::CHILD;
                continue;
            }
            if (!sawWs) return std::unexpected(unexpected_input_());
            complex.steps.back().next = Combinator_::DESCENDANT;
        }
    }

    std::expected<Compound_, std::string> parse_compound_() {
        Compound_ compound;
        bool any { false };
        if (!at_end_() && peek_() == '*') {
            ++pos_;
            any = true;
        } else if (!at_end_() && ident_char_(peek_()) && peek_() != '-') {
            compound.tagName = lower_copy_(read_ident_());
            any = !compound.tagName.empty();
        }
        for (;;) {
            if (at_end_()) break;
            char c { peek_() };
            if (c == '#') {
                ++pos_;
                std::string id { read_ident_() };
                if (id.empty()) return std::unexpected(unexpected_input_());
                compound.ids.push_back(std::move(id));
            } else if (c == '.') {
                ++pos_;
                std::string cls { read_ident_() };
                if (cls.empty()) return std::unexpected(unexpected_input_());
                compound.classes.push_back(std::move(cls));
            } else if (c == '[') {
                auto cond { parse_attr_cond_() };
                if (!cond) return std::unexpected(cond.error());
                compound.attrConds.push_back(std::move(*cond));
            } else if (c == ':') {
                auto pseudo { parse_pseudo_() };
                if (!pseudo) return std::unexpected(pseudo.error());
                compound.pseudos.push_back(std::move(*pseudo));
            } else {
                break;
            }
            any = true;
        }
        if (!any) return std::unexpected(unexpected_input_());
        return compound;
    }

    std::expected<AttrCond_, std::string> parse_attr_cond_() {
        ++pos_;  // '['
        skip_ws_();
        AttrCond_ cond;
        cond.name = lower_copy_(read_ident_());
        if (cond.name.empty()) return std::unexpected(unexpected_input_());
        skip_ws_();
        if (at_end_()) return std::unexpected("Unclosed attribute selector.");
        if (peek_() == ']') {
            ++pos_;
            return cond;
        }
        char c { peek_() };
        if (c == '=') {
            cond.op = AttrOp_::EQUALS;
            ++pos_;
        } else if ((c == '~' || c == '|' || c == '^' || c == '$' || c == '*') &&
                   pos_ + 1 < text_.size() && text_[pos_ + 1] == '=') {
            cond.op = c == '~'   ? AttrOp_::INCLUDES
                      : c == '|' ? AttrOp_::DASH_MATCH
                      : c == '^' ? AttrOp_::PREFIX
                      : c == '$' ? AttrOp_::SUFFIX
                                 : AttrOp_::SUBSTRING;
            pos_ += 2;
        } else {
            return std::unexpected(unexpected_input_());
        }
        skip_ws_();
        if (at_end_()) return std::unexpected("Unclosed attribute selector.");
        if (peek_() == '"' || peek_() == '\'') {
            char quote { peek_() };
            ++pos_;
            while (!at_end_() && peek_() != quote) {
                if (peek_() == '\\' && pos_ + 1 < text_.size()) ++pos_;  // CSS escape: take next char literally
                cond.value.push_back(peek_());
                ++pos_;
            }
            if (at_end_()) return std::unexpected("Unterminated string in the attribute selector.");
            ++pos_;
        } else {
            cond.value = read_ident_();
        }
        skip_ws_();
        if (!at_end_() && (peek_() == 'i' || peek_() == 'I' || peek_() == 's' || peek_() == 'S')) {
            cond.ignoreCase = peek_() == 'i' || peek_() == 'I';
            ++pos_;
            skip_ws_();
        }
        if (at_end_() || peek_() != ']') return std::unexpected("Unclosed attribute selector.");
        ++pos_;
        return cond;
    }

    std::expected<PseudoCond_, std::string> parse_pseudo_() {
        ++pos_;  // ':'
        std::string name { lower_copy_(read_ident_()) };
        PseudoCond_ pseudo;
        if (name == "first-child" || name == "first-of-type") {
            pseudo.kind = name == "first-child" ? PseudoKind_::FIRST_CHILD : PseudoKind_::FIRST_OF_TYPE;
            return pseudo;
        }
        if (name == "nth-child" || name == "nth-of-type") {
            pseudo.kind = name == "nth-child" ? PseudoKind_::NTH_CHILD : PseudoKind_::NTH_OF_TYPE;
            if (at_end_() || peek_() != '(') return std::unexpected(unexpected_input_());
            ++pos_;
            skip_ws_();
            std::size_t b { pos_ };
            while (!at_end_() && peek_() >= '0' && peek_() <= '9') ++pos_;
            if (b == pos_) {
                return std::unexpected(
                    std::format("Unsupported :{}() argument (only a plain index is supported).", name));
            }
            int value { 0 };
            std::from_chars(text_.data() + b, text_.data() + pos_, value);
            pseudo.index = value;
            skip_ws_();
            if (at_end_() || peek_() != ')') return std::unexpected(unexpected_input_());
            ++pos_;
            return pseudo;
        }
        if (name == "not") {
            pseudo.kind = PseudoKind_::NOT;
            if (at_end_() || peek_() != '(') return std::unexpected(unexpected_input_());
            ++pos_;
            skip_ws_();
            auto inner { parse_compound_() };
            if (!inner) return std::unexpected(inner.error());
            pseudo.negated = std::make_unique<Compound_>(std::move(*inner));
            skip_ws_();
            if (at_end_() || peek_() != ')') return std::unexpected(unexpected_input_());
            ++pos_;
            return pseudo;
        }
        return std::unexpected(
            std::format("Unsupported pseudo-class or pseudo-element :{} in the selector.", name));
    }
};

inline std::expected<Selector, std::string> Selector::parse(std::string_view text) {
    Parser_ parser { text };
    auto list { parser.parse_list() };
    if (!list) return std::unexpected(list.error());
    Selector selector;
    selector.alternatives_ = std::move(*list);
    return selector;
}

}  // namespace mbun::html_rewriter
