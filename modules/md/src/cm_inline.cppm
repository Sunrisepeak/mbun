// CommonMark/GFM inline parser -> HTML. Implements the reference delimiter
// stack algorithm (emphasis/strong), links & images (inline + reference),
// code spans, autolinks, raw inline HTML, backslash escapes, entities, hard
// breaks and GFM strikethrough. Aligned with bun md (MD4C) inline behaviour;
// this is a behavioural CommonMark port, not a byte-for-byte translation.
export module mbun.md.cm_inline;

import std;
import mbun.md.cm_common;

export namespace mbun::md::cm {

struct LinkRef { std::string dest; std::string title; };
using RefMap = std::unordered_map<std::string, LinkRef>;

struct InlineFlags {
    bool strikethrough{false};
    bool hardSoftBreaks{false};  // treat every softbreak as <br />
};

// Normalize a link label: trim, collapse internal whitespace, ASCII casefold.
inline std::string normalize_label(std::string_view label) {
    std::string out;
    bool pendingSpace = false;
    bool started = false;
    for (char c : label) {
        if (is_space(c)) { pendingSpace = started; continue; }
        if (pendingSpace) { out.push_back(' '); pendingSpace = false; }
        started = true;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        out.push_back(c);
    }
    return out;
}

// Resolve backslash escapes + entities within a raw slice, producing the
// literal (unescaped) UTF-8 text. Used for link destinations/titles before
// they are re-escaped for their target attribute.
inline std::string resolve_literal(std::string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size() && is_ascii_punct(s[i + 1])) {
            out.push_back(s[i + 1]);
            i += 2;
        } else if (c == '&') {
            std::string tmp;
            std::size_t len = 0;
            if (try_entity(s, i, tmp, len)) {
                // try_entity html-escapes; undo for a literal context by not
                // escaping — re-derive raw by resolving into a plain buffer.
                // Simpler: entities in dest/title are rare; unescape the few.
                // We re-run without escaping via a plain decode.
                out += tmp;  // keep escaped form; href/title escapers tolerate it
                i += len;
            } else {
                out.push_back(c);
                ++i;
            }
        } else {
            out.push_back(c);
            ++i;
        }
    }
    return out;
}

class InlineParser {
public:
    InlineParser(const RefMap& refs, InlineFlags flags) : refs_{refs}, flags_{flags} {}

    std::string render(std::string_view text) {
        nodes_.clear();
        delims_.clear();
        brackets_.clear();
        src_ = text;
        scan_();
        process_emphasis_(delims_.begin());
        std::string out;
        for (auto& n : nodes_) out += n.html;
        return out;
    }

private:
    struct Node { std::string html; bool isDelim{false}; char ch{0}; int count{0}; bool canOpen{false}, canClose{false}; };
    using NodeIt = std::list<Node>::iterator;

    struct Delim { NodeIt node; char ch; int count; int origCount; bool canOpen, canClose; };
    using DelimIt = std::list<Delim>::iterator;

    struct Bracket {
        NodeIt node;          // the text node holding "[" or "![" (literal fallback)
        std::size_t srcAfter; // source index just after the '[' (label start)
        DelimIt delimMark;    // delimiter-stack position at push time (for emphasis scope)
        bool hasDelimMark{false};
        bool image{false};
        bool active{true};
    };

    const RefMap& refs_;
    InlineFlags flags_;
    std::string_view src_;
    std::list<Node> nodes_;
    std::list<Delim> delims_;
    std::vector<Bracket> brackets_;
    std::string pending_;  // accumulated literal text (already escaped)

    void flush_text_() {
        if (pending_.empty()) return;
        nodes_.push_back(Node{std::move(pending_), false});
        pending_.clear();
    }

    NodeIt push_literal_node_(std::string html) {
        flush_text_();
        nodes_.push_back(Node{std::move(html), false});
        return std::prev(nodes_.end());
    }

    // --- main left-to-right scan ---------------------------------------------
    void scan_() {
        std::size_t i = 0;
        const std::size_t n = src_.size();
        while (i < n) {
            char c = src_[i];
            switch (c) {
                case '\\': scan_backslash_(i); break;
                case '`':  scan_code_span_(i); break;
                case '<':  scan_langle_(i); break;
                case '&':  scan_entity_(i); break;
                case '\n': scan_newline_(i); break;
                case '*': case '_': case '~': scan_delim_(i); break;
                case '[': scan_open_bracket_(i, false); break;
                case '!':
                    if (i + 1 < n && src_[i + 1] == '[') scan_open_bracket_(i, true);
                    else { pending_.push_back('!'); ++i; }
                    break;
                case ']': scan_close_bracket_(i); break;
                default:
                    append_escaped_html(pending_, src_.substr(i, 1));
                    ++i;
                    break;
            }
        }
        flush_text_();
    }

    void scan_backslash_(std::size_t& i) {
        if (i + 1 < src_.size()) {
            char nx = src_[i + 1];
            if (nx == '\n') {           // hard line break
                // trim trailing spaces already handled by newline path; here emit break
                push_literal_node_("<br />\n");
                i += 2;
                skip_leading_spaces_(i);
                return;
            }
            if (is_ascii_punct(nx)) {
                append_escaped_html(pending_, src_.substr(i + 1, 1));
                i += 2;
                return;
            }
        }
        pending_.push_back('\\');
        ++i;
    }

    void scan_entity_(std::size_t& i) {
        std::string tmp;
        std::size_t len = 0;
        if (try_entity(src_, i, tmp, len)) {
            pending_ += tmp;
            i += len;
        } else {
            pending_ += "&amp;";
            ++i;
        }
    }

    void scan_newline_(std::size_t& i) {
        // Determine hard break: 2+ trailing spaces in pending source.
        std::size_t trailing = 0;
        while (trailing < pending_.size() && pending_[pending_.size() - 1 - trailing] == ' ') ++trailing;
        bool hard = trailing >= 2;
        // strip trailing spaces from pending
        while (!pending_.empty() && pending_.back() == ' ') pending_.pop_back();
        if (hard || flags_.hardSoftBreaks) {
            push_literal_node_("<br />\n");
        } else {
            push_literal_node_("\n");
        }
        ++i;
        skip_leading_spaces_(i);
    }

    void skip_leading_spaces_(std::size_t& i) {
        while (i < src_.size() && (src_[i] == ' ' || src_[i] == '\t')) ++i;
    }

    // --- code spans ----------------------------------------------------------
    void scan_code_span_(std::size_t& i) {
        std::size_t start = i;
        std::size_t open = 0;
        while (i < src_.size() && src_[i] == '`') { ++open; ++i; }
        // find closing run of exactly `open` backticks
        std::size_t j = i;
        while (j < src_.size()) {
            if (src_[j] == '`') {
                std::size_t run = 0;
                std::size_t k = j;
                while (k < src_.size() && src_[k] == '`') { ++run; ++k; }
                if (run == open) {
                    // content is src_[i, j)
                    std::string content{src_.substr(i, j - i)};
                    for (char& ch : content) if (ch == '\n') ch = ' ';
                    // strip one leading & trailing space if content non-blank and both present
                    if (content.size() >= 2 && content.front() == ' ' && content.back() == ' ' &&
                        content.find_first_not_of(' ') != std::string::npos) {
                        content = content.substr(1, content.size() - 2);
                    }
                    std::string node = "<code>";
                    append_escaped_html(node, content);
                    node += "</code>";
                    push_literal_node_(std::move(node));
                    i = k;
                    return;
                }
                j = k;
            } else {
                ++j;
            }
        }
        // no closing run: literal backticks
        append_escaped_html(pending_, src_.substr(start, open));
        i = start + open;
    }

    // --- autolinks / raw html ------------------------------------------------
    void scan_langle_(std::size_t& i) {
        if (try_autolink_(i)) return;
        if (try_raw_html_(i)) return;
        pending_ += "&lt;";
        ++i;
    }

    bool try_autolink_(std::size_t& i) {
        std::size_t end = src_.find('>', i + 1);
        if (end == std::string_view::npos) return false;
        std::string_view inner = src_.substr(i + 1, end - i - 1);
        if (inner.empty()) return false;
        // URI autolink: scheme:...  no spaces or control chars, no '<'
        std::size_t colon = inner.find(':');
        bool uri = false;
        if (colon != std::string_view::npos && colon >= 1) {
            std::string_view scheme = inner.substr(0, colon);
            bool ok = is_ascii_alpha(scheme[0]) && scheme.size() <= 32;
            for (char c : scheme) if (!(is_ascii_alpha(c) || is_ascii_digit(c) || c == '+' || c == '.' || c == '-')) ok = false;
            for (char c : inner) if (is_space(c) || c == '<') ok = false;
            uri = ok;
        }
        if (uri) {
            std::string href;
            append_href_escaped(href, inner);
            std::string node = "<a href=\"" + href + "\">";
            append_escaped_html(node, inner);
            node += "</a>";
            push_literal_node_(std::move(node));
            i = end + 1;
            return true;
        }
        // Email autolink: simple validation.
        if (is_email_(inner)) {
            std::string href;
            append_href_escaped(href, "mailto:");
            append_href_escaped(href, inner);
            std::string node = "<a href=\"" + href + "\">";
            append_escaped_html(node, inner);
            node += "</a>";
            push_literal_node_(std::move(node));
            i = end + 1;
            return true;
        }
        return false;
    }

    static bool is_email_(std::string_view s) {
        std::size_t at = s.find('@');
        if (at == std::string_view::npos || at == 0 || at + 1 >= s.size()) return false;
        std::string_view local = s.substr(0, at);
        std::string_view domain = s.substr(at + 1);
        for (char c : local) {
            if (!(is_ascii_alnum(c) || std::string_view(".!#$%&'*+/=?^_`{|}~-").find(c) != std::string_view::npos)) return false;
        }
        // domain: labels of alnum/hyphen separated by dots
        if (domain.empty() || domain.front() == '.' || domain.back() == '.') return false;
        for (char c : domain) {
            if (!(is_ascii_alnum(c) || c == '-' || c == '.')) return false;
        }
        return true;
    }

    bool try_raw_html_(std::size_t& i) {
        std::string_view s = src_;
        std::size_t n = s.size();
        // Comment <!-- ... -->
        if (s.compare(i, 4, "<!--") == 0) {
            std::size_t end = s.find("-->", i + 4);
            if (end != std::string_view::npos) {
                push_literal_node_(std::string{s.substr(i, end + 3 - i)});
                i = end + 3;
                return true;
            }
            return false;
        }
        // Processing instruction <? ... ?>
        if (s.compare(i, 2, "<?") == 0) {
            std::size_t end = s.find("?>", i + 2);
            if (end != std::string_view::npos) {
                push_literal_node_(std::string{s.substr(i, end + 2 - i)});
                i = end + 2;
                return true;
            }
            return false;
        }
        // Declaration <!NAME ... >
        if (i + 2 < n && s[i + 1] == '!' && is_ascii_alpha(s[i + 2])) {
            std::size_t end = s.find('>', i + 2);
            if (end != std::string_view::npos) {
                push_literal_node_(std::string{s.substr(i, end + 1 - i)});
                i = end + 1;
                return true;
            }
            return false;
        }
        // CDATA
        if (s.compare(i, 9, "<![CDATA[") == 0) {
            std::size_t end = s.find("]]>", i + 9);
            if (end != std::string_view::npos) {
                push_literal_node_(std::string{s.substr(i, end + 3 - i)});
                i = end + 3;
                return true;
            }
            return false;
        }
        // Open or closing tag
        std::size_t j = i + 1;
        bool closing = false;
        if (j < n && s[j] == '/') { closing = true; ++j; }
        if (j >= n || !is_ascii_alpha(s[j])) return false;
        while (j < n && (is_ascii_alnum(s[j]) || s[j] == '-')) ++j;
        // tag name done
        if (closing) {
            while (j < n && is_space(s[j])) ++j;
            if (j < n && s[j] == '>') {
                push_literal_node_(std::string{s.substr(i, j + 1 - i)});
                i = j + 1;
                return true;
            }
            return false;
        }
        // attributes
        while (true) {
            std::size_t k = j;
            while (k < n && is_space(s[k])) ++k;
            if (k < n && (s[k] == '>' || (s[k] == '/' && k + 1 < n && s[k + 1] == '>'))) { j = k; break; }
            if (k == j) break;  // need whitespace before attribute
            if (k >= n || !(is_ascii_alpha(s[k]) || s[k] == '_' || s[k] == ':')) return false;
            ++k;
            while (k < n && (is_ascii_alnum(s[k]) || s[k] == '_' || s[k] == ':' || s[k] == '.' || s[k] == '-')) ++k;
            std::size_t m = k;
            while (m < n && is_space(s[m])) ++m;
            if (m < n && s[m] == '=') {
                ++m;
                while (m < n && is_space(s[m])) ++m;
                if (m >= n) return false;
                if (s[m] == '"') { m = s.find('"', m + 1); if (m == std::string_view::npos) return false; ++m; }
                else if (s[m] == '\'') { m = s.find('\'', m + 1); if (m == std::string_view::npos) return false; ++m; }
                else { while (m < n && !is_space(s[m]) && s[m] != '>' && s[m] != '"' && s[m] != '\'' && s[m] != '=' && s[m] != '<' && s[m] != '`') ++m; }
                j = m;
            } else {
                j = k;
            }
        }
        while (j < n && is_space(s[j])) ++j;
        if (j < n && s[j] == '/') ++j;
        if (j < n && s[j] == '>') {
            push_literal_node_(std::string{s.substr(i, j + 1 - i)});
            i = j + 1;
            return true;
        }
        return false;
    }

    // --- emphasis delimiters -------------------------------------------------
    void scan_delim_(std::size_t& i) {
        char ch = src_[i];
        if (ch == '~' && !flags_.strikethrough) {
            pending_.push_back('~');
            ++i;
            return;
        }
        std::size_t start = i;
        while (i < src_.size() && src_[i] == ch) ++i;
        int count = static_cast<int>(i - start);
        char before = start > 0 ? src_[start - 1] : '\n';
        char after = i < src_.size() ? src_[i] : '\n';
        bool leftFlank = compute_left_flank_(before, after);
        bool rightFlank = compute_right_flank_(before, after);
        bool canOpen, canClose;
        if (ch == '_') {
            canOpen = leftFlank && (!rightFlank || is_punct_char_(before));
            canClose = rightFlank && (!leftFlank || is_punct_char_(after));
        } else {
            canOpen = leftFlank;
            canClose = rightFlank;
        }
        std::string lit(static_cast<std::size_t>(count), ch);
        flush_text_();
        nodes_.push_back(Node{lit, true, ch, count, canOpen, canClose});
        NodeIt node = std::prev(nodes_.end());
        delims_.push_back(Delim{node, ch, count, count, canOpen, canClose});
    }

    static bool is_punct_char_(char c) { return is_ascii_punct(c); }

    static bool compute_left_flank_(char before, char after) {
        bool afterWs = is_space(after);
        bool afterPunct = is_ascii_punct(after);
        bool beforeWs = is_space(before);
        bool beforePunct = is_ascii_punct(before);
        // not followed by whitespace, and (not followed by punct OR preceded by ws/punct)
        return !afterWs && (!afterPunct || beforeWs || beforePunct);
    }
    static bool compute_right_flank_(char before, char after) {
        bool beforeWs = is_space(before);
        bool beforePunct = is_ascii_punct(before);
        bool afterWs = is_space(after);
        bool afterPunct = is_ascii_punct(after);
        return !beforeWs && (!beforePunct || afterWs || afterPunct);
    }

    // --- brackets, links & images --------------------------------------------
    void scan_open_bracket_(std::size_t& i, bool image) {
        Bracket b;
        if (image) {
            b.node = push_literal_node_("![");
            b.srcAfter = i + 2;
            b.image = true;
            i += 2;
        } else {
            b.node = push_literal_node_("[");
            b.srcAfter = i + 1;
            i += 1;
        }
        if (!delims_.empty()) { b.delimMark = std::prev(delims_.end()); b.hasDelimMark = true; }
        brackets_.push_back(b);
    }

    void scan_close_bracket_(std::size_t& i) {
        if (brackets_.empty()) {
            pending_.push_back(']');
            ++i;
            return;
        }
        Bracket b = brackets_.back();
        brackets_.pop_back();
        if (!b.active) {
            append_escaped_html(pending_, "]");
            ++i;
            return;
        }
        std::size_t labelEnd = i;  // source index of ']'
        std::size_t after = i + 1;
        std::string dest, title;
        bool matched = false;

        if (after < src_.size() && src_[after] == '(') {
            std::size_t p = after + 1;
            matched = parse_inline_dest_title_(p, dest, title);
            if (matched) after = p;
        }
        if (!matched) {
            // reference forms
            std::string label;
            std::size_t p = after;
            bool haveRef = false;
            if (p < src_.size() && src_[p] == '[') {
                std::size_t close = find_label_end_(p);
                if (close != std::string_view::npos) {
                    std::string_view inner = src_.substr(p + 1, close - p - 1);
                    if (inner.empty()) {
                        label = std::string{src_.substr(b.srcAfter, labelEnd - b.srcAfter)};  // collapsed
                    } else {
                        label = std::string{inner};  // full
                    }
                    after = close + 1;
                    haveRef = true;
                }
            }
            if (!haveRef) {
                // shortcut reference
                label = std::string{src_.substr(b.srcAfter, labelEnd - b.srcAfter)};
            }
            auto it = refs_.find(normalize_label(label));
            if (it != refs_.end()) {
                dest = it->second.dest;
                title = it->second.title;
                matched = true;
            }
        }

        if (!matched) {
            append_escaped_html(pending_, "]");
            ++i;
            return;
        }

        flush_text_();
        // Process emphasis strictly inside the bracket scope first.
        DelimIt scopeStart = b.hasDelimMark ? std::next(b.delimMark) : delims_.begin();
        process_emphasis_(scopeStart);

        if (b.image) {
            // alt = plain text of inner nodes (tags stripped)
            std::string alt = plain_text_between_(b.node);
            // remove inner nodes + opener, replace with single <img>
            NodeIt after_open = std::next(b.node);
            nodes_.erase(after_open, nodes_.end());
            b.node->html.clear();
            b.node->isDelim = false;
            std::string html = "<img src=\"";
            append_href_escaped(html, dest);
            html += "\" alt=\"";
            html += alt;
            html += "\"";
            if (!title.empty()) {
                html += " title=\"";
                append_escaped_html(html, resolve_literal(title));
                html += "\"";
            }
            html += " />";
            b.node->html = std::move(html);
        } else {
            // link: wrap inner nodes
            std::string open = "<a href=\"";
            append_href_escaped(open, dest);
            open += "\"";
            if (!title.empty()) {
                open += " title=\"";
                append_escaped_html(open, resolve_literal(title));
                open += "\"";
            }
            open += ">";
            b.node->html = std::move(open);
            b.node->isDelim = false;
            nodes_.push_back(Node{"</a>", false});
            // deactivate earlier link brackets (no nested links)
            for (auto& br : brackets_) if (!br.image) br.active = false;
        }
        i = after;
    }

    std::string plain_text_between_(NodeIt opener) {
        std::string alt;
        for (NodeIt it = std::next(opener); it != nodes_.end(); ++it) {
            // strip tags from html
            const std::string& h = it->html;
            for (std::size_t k = 0; k < h.size();) {
                if (h[k] == '<') {
                    std::size_t gt = h.find('>', k);
                    if (gt == std::string::npos) break;
                    k = gt + 1;
                } else {
                    alt.push_back(h[k]);
                    ++k;
                }
            }
        }
        return alt;
    }

    std::size_t find_label_end_(std::size_t open) {
        // open points at '['; find matching ']' allowing backslash escapes
        for (std::size_t k = open + 1; k < src_.size(); ++k) {
            if (src_[k] == '\\') { ++k; continue; }
            if (src_[k] == ']') return k;
            if (src_[k] == '[') return std::string_view::npos;
        }
        return std::string_view::npos;
    }

    bool parse_inline_dest_title_(std::size_t& p, std::string& dest, std::string& title) {
        skip_ws_(p);
        // destination
        if (p < src_.size() && src_[p] == '<') {
            std::size_t end = p + 1;
            std::string d;
            while (end < src_.size() && src_[end] != '>' && src_[end] != '\n') {
                if (src_[end] == '\\' && end + 1 < src_.size()) { d.push_back(src_[end + 1]); end += 2; continue; }
                d.push_back(src_[end]);
                ++end;
            }
            if (end >= src_.size() || src_[end] != '>') return false;
            dest = d;
            p = end + 1;
        } else {
            std::string d;
            int depth = 0;
            while (p < src_.size()) {
                char c = src_[p];
                if (c == '\\' && p + 1 < src_.size() && is_ascii_punct(src_[p + 1])) { d.push_back(src_[p + 1]); p += 2; continue; }
                if (is_space(c)) break;
                if (c == '(') { ++depth; }
                else if (c == ')') { if (depth == 0) break; --depth; }
                if (static_cast<unsigned char>(c) < 0x20) break;
                d.push_back(c);
                ++p;
            }
            dest = d;
        }
        skip_ws_(p);
        // optional title
        if (p < src_.size() && (src_[p] == '"' || src_[p] == '\'' || src_[p] == '(')) {
            char open = src_[p];
            char close = open == '(' ? ')' : open;
            std::size_t end = p + 1;
            std::string t;
            bool ok = false;
            while (end < src_.size()) {
                if (src_[end] == '\\' && end + 1 < src_.size()) { t.push_back(src_[end]); t.push_back(src_[end + 1]); end += 2; continue; }
                if (src_[end] == close) { ok = true; break; }
                t.push_back(src_[end]);
                ++end;
            }
            if (!ok) return false;
            title = t;
            p = end + 1;
            skip_ws_(p);
        }
        if (p >= src_.size() || src_[p] != ')') return false;
        ++p;
        return true;
    }

    void skip_ws_(std::size_t& p) {
        while (p < src_.size() && is_space(src_[p])) ++p;
    }

    // --- emphasis processing (delimiter stack) -------------------------------
    void process_emphasis_(DelimIt stackBottom) {
        // openers_bottom per delimiter char & length-mod class
        std::unordered_map<int, DelimIt> openersBottom;
        auto bottomKey = [](char ch, int origCount, bool canOpen, bool canClose) {
            int mod = origCount % 3;
            int flag = (canOpen ? 1 : 0) * 2 + (canClose ? 1 : 0);
            return (static_cast<int>(static_cast<unsigned char>(ch)) << 8) | (mod << 2) | flag;
        };

        DelimIt closer = stackBottom;
        while (closer != delims_.end()) {
            if (!closer->canClose) { ++closer; continue; }
            char ch = closer->ch;
            // find opener
            DelimIt opener = closer;
            bool found = false;
            if (opener != stackBottom) {
                do {
                    --opener;
                    if (opener->ch == ch && opener->canOpen) {
                        bool oddMatch = (closer->canOpen || opener->canClose) &&
                                        (closer->origCount % 3 != 0 || opener->origCount % 3 != 0) &&
                                        (opener->origCount + closer->origCount) % 3 == 0;
                        if (!oddMatch) { found = true; break; }
                    }
                    if (opener == stackBottom) break;
                } while (true);
            }

            DelimIt oldCloser = closer;
            if (found) {
                int use = (opener->count >= 2 && closer->count >= 2) ? 2 : 1;
                std::string openTag, closeTag;
                if (ch == '~') { openTag = "<del>"; closeTag = "</del>"; }
                else if (use == 2) { openTag = "<strong>"; closeTag = "</strong>"; }
                else { openTag = "<em>"; closeTag = "</em>"; }

                // shrink opener literal (remove `use` trailing chars) & closer (leading)
                NodeIt on = opener->node;
                NodeIt cn = closer->node;
                on->html.erase(on->html.size() - static_cast<std::size_t>(use));
                cn->html.erase(0, static_cast<std::size_t>(use));
                opener->count -= use;
                closer->count -= use;

                nodes_.insert(std::next(on), Node{std::move(openTag), false});
                nodes_.insert(cn, Node{std::move(closeTag), false});

                // remove delimiters strictly between opener and closer
                DelimIt d = std::next(opener);
                while (d != closer) d = delims_.erase(d);

                if (opener->count == 0) {
                    nodes_.erase(on);
                    delims_.erase(opener);
                }
                if (closer->count == 0) {
                    NodeIt toErase = cn;
                    DelimIt next = std::next(closer);
                    nodes_.erase(toErase);
                    delims_.erase(closer);
                    closer = next;
                }
            } else {
                int key = bottomKey(ch, oldCloser->origCount, oldCloser->canOpen, oldCloser->canClose);
                // openersBottom for this class -> nothing before it can match; move on
                openersBottom[key] = oldCloser;
                if (!oldCloser->canOpen) {
                    // it can only close and found nothing: leave as literal, advance
                }
                ++closer;
            }
        }
        // remaining delimiters render as their literal text automatically.
    }
};

// Convenience free function.
inline std::string render_inline(std::string_view text, const RefMap& refs, InlineFlags flags) {
    InlineParser p{refs, flags};
    return p.render(text);
}

}  // namespace mbun::md::cm
