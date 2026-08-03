// CommonMark/GFM block parser -> HTML. Builds a block tree (so forward link
// reference definitions resolve), then renders it, delegating inline content
// to mbun.md.cm_inline. Handles paragraphs, ATX & setext headings, fenced &
// indented code, block quotes, bullet/ordered lists (with task-list items),
// thematic breaks, HTML blocks, and GFM tables. Behavioural CommonMark port
// aligned with bun md (MD4C) blocks/line_analysis blueprints.
export module mbun.md.cm_block;

import std;
import mbun.md.cm_common;
import mbun.md.cm_inline;

export namespace mbun::md::cm {

struct BlockFlags {
    bool tables{false};
    bool strikethrough{false};
    bool tasklists{false};
    bool noIndentedCode{false};
    bool hardSoftBreaks{false};
};

namespace detail {

struct Block {
    enum class Kind { Doc, Para, Heading, Code, Html, Quote, List, Item, Hr, Table };
    Kind kind{Kind::Para};
    int level{0};
    std::string text;
    std::string info;
    bool ordered{false};
    int start{1};
    char delim{'.'};
    char bullet{'-'};
    bool tight{true};
    bool task{false};
    bool taskChecked{false};
    std::vector<Block> children;
    // table
    std::vector<std::string> headers;
    std::vector<int> aligns;  // 0 none,1 left,2 center,3 right
    std::vector<std::vector<std::string>> rows;
};

inline int indent_width(std::string_view line, std::size_t upto) {
    int col = 0;
    for (std::size_t i = 0; i < upto && i < line.size(); ++i) {
        if (line[i] == '\t') col += 4 - (col % 4);
        else col += 1;
    }
    return col;
}

inline std::size_t count_leading_indent(std::string_view line, int& outCols) {
    int col = 0;
    std::size_t i = 0;
    while (i < line.size()) {
        if (line[i] == ' ') { col += 1; ++i; }
        else if (line[i] == '\t') { col += 4 - (col % 4); ++i; }
        else break;
    }
    outCols = col;
    return i;
}

inline bool is_blank(std::string_view l) {
    for (char c : l) if (c != ' ' && c != '\t') return false;
    return true;
}

inline std::string rtrim(std::string_view s) {
    std::size_t e = s.size();
    while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' || s[e - 1] == '\r')) --e;
    return std::string{s.substr(0, e)};
}

inline std::string ltrim(std::string_view s) {
    std::size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) ++b;
    return std::string{s.substr(b)};
}

inline std::string trim(std::string_view s) { return rtrim(ltrim(s)); }

class BlockParser {
public:
    explicit BlockParser(BlockFlags flags) : flags_{flags} {}

    std::string parse(std::string_view text) {
        std::vector<std::string> lines = split_lines_(text);
        Block doc;
        doc.kind = Block::Kind::Doc;
        doc.children = parse_blocks_(lines);
        std::string out;
        render_blocks_(doc.children, out, false);
        return out;
    }

private:
    BlockFlags flags_;
    RefMap refs_;

    static std::vector<std::string> split_lines_(std::string_view text) {
        std::vector<std::string> lines;
        std::size_t start = 0;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\n') {
                std::string_view l = text.substr(start, i - start);
                if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
                lines.emplace_back(l);
                start = i + 1;
            }
        }
        if (start < text.size()) {
            std::string_view l = text.substr(start);
            if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
            lines.emplace_back(l);
        }
        return lines;
    }

    InlineFlags inline_flags_() const {
        InlineFlags f;
        f.strikethrough = flags_.strikethrough;
        f.hardSoftBreaks = flags_.hardSoftBreaks;
        return f;
    }

    // --- line classifiers ----------------------------------------------------

    static bool is_thematic_break_(std::string_view line) {
        int cols;
        std::size_t i = count_leading_indent(line, cols);
        if (cols > 3) return false;
        char c = 0;
        int count = 0;
        for (; i < line.size(); ++i) {
            char ch = line[i];
            if (ch == ' ' || ch == '\t') continue;
            if (ch == '*' || ch == '-' || ch == '_') {
                if (c == 0) c = ch;
                else if (ch != c) return false;
                ++count;
            } else {
                return false;
            }
        }
        return count >= 3;
    }

    // ATX heading -> level (0 if none). Sets outText to inner text.
    static int atx_heading_(std::string_view line, std::string& outText) {
        int cols;
        std::size_t i = count_leading_indent(line, cols);
        if (cols > 3) return 0;
        int level = 0;
        while (i < line.size() && line[i] == '#') { ++level; ++i; }
        if (level == 0 || level > 6) return 0;
        if (i < line.size() && line[i] != ' ' && line[i] != '\t') return 0;
        std::string rest = trim(line.substr(i));
        // strip trailing sequence of # preceded by space (or entire)
        std::size_t e = rest.size();
        while (e > 0 && (rest[e - 1] == ' ' || rest[e - 1] == '\t')) --e;
        std::size_t h = e;
        while (h > 0 && rest[h - 1] == '#') --h;
        if (h < e && (h == 0 || rest[h - 1] == ' ' || rest[h - 1] == '\t')) {
            e = h;
            while (e > 0 && (rest[e - 1] == ' ' || rest[e - 1] == '\t')) --e;
            rest = rest.substr(0, e);
        }
        outText = rest;
        return level;
    }

    // Fenced code opener -> fence length (0 none). Sets char, indent, info.
    static int fenced_open_(std::string_view line, char& fenceChar, int& indent, std::string& info) {
        int cols;
        std::size_t i = count_leading_indent(line, cols);
        if (cols > 3) return 0;
        if (i >= line.size()) return 0;
        char c = line[i];
        if (c != '`' && c != '~') return 0;
        int len = 0;
        while (i < line.size() && line[i] == c) { ++len; ++i; }
        if (len < 3) return 0;
        std::string rest = trim(line.substr(i));
        if (c == '`' && rest.find('`') != std::string::npos) return 0;  // info with backtick invalid
        fenceChar = c;
        indent = cols;
        info = rest;
        return len;
    }

    static bool fenced_close_(std::string_view line, char fenceChar, int openLen) {
        int cols;
        std::size_t i = count_leading_indent(line, cols);
        if (cols > 3) return false;
        int len = 0;
        while (i < line.size() && line[i] == fenceChar) { ++len; ++i; }
        if (len < openLen) return false;
        for (; i < line.size(); ++i) if (line[i] != ' ' && line[i] != '\t') return false;
        return true;
    }

    // Bullet/ordered list marker. Returns marker byte length (chars consumed
    // through the space), sets fields. 0 if not a list item.
    static std::size_t list_marker_(std::string_view line, bool& ordered, int& start, char& delim, char& bullet, int& markerCols, int& afterMarkerCols) {
        int cols;
        std::size_t i = count_leading_indent(line, cols);
        if (cols > 3) return 0;
        markerCols = cols;
        std::size_t markStart = i;
        if (i < line.size() && (line[i] == '-' || line[i] == '+' || line[i] == '*')) {
            bullet = line[i];
            ordered = false;
            ++i;
        } else {
            int num = 0, digits = 0;
            std::size_t j = i;
            while (j < line.size() && is_ascii_digit(line[j]) && digits < 9) { num = num * 10 + (line[j] - '0'); ++digits; ++j; }
            if (digits == 0) return 0;
            if (j >= line.size() || (line[j] != '.' && line[j] != ')')) return 0;
            ordered = true;
            start = num;
            delim = line[j];
            i = j + 1;
        }
        // must be followed by space/tab or end of line
        std::size_t markEnd = i;
        if (i < line.size() && line[i] != ' ' && line[i] != '\t') return 0;
        // compute content indent
        int afterCols = indent_width(line, markEnd);
        // count spaces after marker
        std::size_t k = markEnd;
        int spCols = afterCols;
        while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) {
            if (line[k] == '\t') spCols += 4 - (spCols % 4);
            else spCols += 1;
            ++k;
        }
        int spaces = spCols - afterCols;
        // blank or >4 spaces -> content indent is markerWidth+1
        if (k >= line.size() || spaces == 0) afterMarkerCols = afterCols + 1;
        else if (spaces > 4) afterMarkerCols = afterCols + 1;
        else afterMarkerCols = afterCols + spaces;
        (void)markStart;
        return markEnd;  // return index just after marker char(s)
    }

    static bool setext_underline_(std::string_view line, int& level) {
        int cols;
        std::size_t i = count_leading_indent(line, cols);
        if (cols > 3) return false;
        if (i >= line.size()) return false;
        char c = line[i];
        if (c != '=' && c != '-') return false;
        std::size_t cnt = 0;
        while (i < line.size() && line[i] == c) { ++i; ++cnt; }
        for (; i < line.size(); ++i) if (line[i] != ' ' && line[i] != '\t') return false;
        if (cnt == 0) return false;
        level = (c == '=') ? 1 : 2;
        return true;
    }

    // HTML block start condition -> type 1..7 (0 none)
    static int html_block_start_(std::string_view line, int& endType) {
        int cols;
        std::size_t i = count_leading_indent(line, cols);
        if (cols > 3) return 0;
        if (i >= line.size() || line[i] != '<') return 0;
        std::string_view rest = line.substr(i);
        auto lower = [](std::string_view s) { std::string r; for (char c : s) r.push_back((c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c); return r; };
        // type 1: <script <pre <style <textarea
        std::string lr = lower(rest.substr(0, std::min<std::size_t>(rest.size(), 10)));
        for (std::string_view tag : {"<script", "<pre", "<style", "<textarea"}) {
            if (lr.compare(0, tag.size(), tag) == 0) {
                char after = rest.size() > tag.size() ? rest[tag.size()] : ' ';
                if (after == ' ' || after == '\t' || after == '>' || rest.size() == tag.size()) { endType = 1; return 1; }
            }
        }
        if (rest.compare(0, 4, "<!--") == 0) { endType = 2; return 2; }
        if (rest.compare(0, 2, "<?") == 0) { endType = 3; return 3; }
        if (rest.size() >= 2 && rest[1] == '!' && rest.size() >= 3 && is_ascii_alpha(rest[2])) { endType = 4; return 4; }
        if (rest.compare(0, 9, "<![CDATA[") == 0) { endType = 5; return 5; }
        // type 6: block-level tag names
        std::size_t j = 1;
        bool closing = false;
        if (j < rest.size() && rest[j] == '/') { closing = true; ++j; }
        std::string name;
        while (j < rest.size() && (is_ascii_alpha(rest[j]) || is_ascii_digit(rest[j]))) { name.push_back(char((rest[j] >= 'A' && rest[j] <= 'Z') ? rest[j] - 'A' + 'a' : rest[j])); ++j; }
        static const std::unordered_set<std::string_view> blockTags = {
            "address","article","aside","base","basefont","blockquote","body","caption","center","col","colgroup","dd","details","dialog","dir","div","dl","dt","fieldset","figcaption","figure","footer","form","frame","frameset","h1","h2","h3","h4","h5","h6","head","header","hr","html","iframe","legend","li","link","main","menu","menuitem","nav","noframes","ol","optgroup","option","p","param","section","summary","table","tbody","td","tfoot","th","thead","title","tr","track","ul"};
        if (!name.empty() && blockTags.count(name)) {
            char after = j < rest.size() ? rest[j] : ' ';
            if (after == ' ' || after == '\t' || after == '>' || j >= rest.size() || (after == '/' && j + 1 < rest.size() && rest[j + 1] == '>')) { endType = 6; return 6; (void)closing; }
        }
        return 0;
    }

    static bool html_block_end_(std::string_view line, int type) {
        auto has = [&](std::string_view needle) { return line.find(needle) != std::string_view::npos; };
        switch (type) {
            case 1: {
                std::string l; for (char c : line) l.push_back((c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c);
                return l.find("</script>") != std::string::npos || l.find("</pre>") != std::string::npos || l.find("</style>") != std::string::npos || l.find("</textarea>") != std::string::npos;
            }
            case 2: return has("-->");
            case 3: return has("?>");
            case 4: return has(">");
            case 5: return has("]]>");
            default: return false;  // 6,7 end on blank line
        }
    }

    // --- reference definitions ----------------------------------------------
    // Try to parse a leading link reference definition from `text` (may span
    // multiple lines). Returns bytes consumed (0 if none) and records the ref.
    std::size_t try_ref_def_(std::string_view text) {
        std::size_t i = 0;
        int cols = 0;
        i = count_leading_indent(text, cols);
        if (cols > 3) return 0;
        if (i >= text.size() || text[i] != '[') return 0;
        // label
        std::size_t j = i + 1;
        std::string label;
        bool closed = false;
        while (j < text.size()) {
            char c = text[j];
            if (c == '\\' && j + 1 < text.size()) { label.push_back(c); label.push_back(text[j + 1]); j += 2; continue; }
            if (c == ']') { closed = true; break; }
            if (c == '[') return 0;
            label.push_back(c);
            ++j;
        }
        if (!closed || label.empty() || label.size() > 999) return 0;
        ++j;  // past ']'
        if (j >= text.size() || text[j] != ':') return 0;
        ++j;
        // optional whitespace incl up to one newline
        int nl = 0;
        while (j < text.size() && (text[j] == ' ' || text[j] == '\t' || text[j] == '\n')) { if (text[j] == '\n') { if (++nl > 1) return 0; } ++j; }
        if (j >= text.size()) return 0;
        // destination
        std::string dest;
        if (text[j] == '<') {
            ++j;
            while (j < text.size() && text[j] != '>' && text[j] != '\n') {
                if (text[j] == '\\' && j + 1 < text.size()) { dest.push_back(text[j + 1]); j += 2; continue; }
                dest.push_back(text[j]); ++j;
            }
            if (j >= text.size() || text[j] != '>') return 0;
            ++j;
        } else {
            int depth = 0;
            while (j < text.size()) {
                char c = text[j];
                if (c == ' ' || c == '\t' || c == '\n') break;
                if (static_cast<unsigned char>(c) < 0x20) break;
                if (c == '\\' && j + 1 < text.size() && is_ascii_punct(text[j + 1])) { dest.push_back(text[j + 1]); j += 2; continue; }
                if (c == '(') ++depth;
                else if (c == ')') { if (depth == 0) break; --depth; }
                dest.push_back(c); ++j;
            }
            if (dest.empty()) return 0;
        }
        // optional title, preceded by whitespace (incl newline)
        std::size_t afterDest = j;
        int nl2 = 0;
        std::size_t k = j;
        bool sawSpace = false;
        while (k < text.size() && (text[k] == ' ' || text[k] == '\t' || text[k] == '\n')) { if (text[k] == '\n') ++nl2; sawSpace = true; ++k; }
        std::string title;
        bool haveTitle = false;
        std::size_t endPos = afterDest;
        if (sawSpace && nl2 <= 1 && k < text.size() && (text[k] == '"' || text[k] == '\'' || text[k] == '(')) {
            char open = text[k];
            char close = open == '(' ? ')' : open;
            std::size_t m = k + 1;
            bool ok = false;
            while (m < text.size()) {
                if (text[m] == '\\' && m + 1 < text.size()) { title.push_back(text[m]); title.push_back(text[m + 1]); m += 2; continue; }
                if (text[m] == close) { ok = true; break; }
                if (text[m] == open && open != close) { title.clear(); ok = false; break; }
                title.push_back(text[m]); ++m;
            }
            if (ok) { haveTitle = true; endPos = m + 1; }
        }
        // The rest of the (title's or dest's) line must be blank up to newline.
        std::size_t p = endPos;
        while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) ++p;
        if (p < text.size() && text[p] != '\n') {
            // trailing junk: if we had a title, the def is invalid; if no title,
            // maybe the "title" chars belonged to next paragraph -> reject title.
            if (haveTitle) { haveTitle = false; title.clear(); endPos = afterDest; p = afterDest; while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) ++p; if (p < text.size() && text[p] != '\n') return 0; }
            else return 0;
        }
        if (p < text.size() && text[p] == '\n') ++p;
        std::string norm = normalize_label(label);
        if (!norm.empty() && refs_.find(norm) == refs_.end()) {
            refs_[norm] = LinkRef{resolve_literal(dest), haveTitle ? title : std::string{}};
        }
        return p;
    }

    void finalize_paragraph_text_(std::string& raw) {
        // strip leading reference definitions
        while (!raw.empty()) {
            std::size_t consumed = try_ref_def_(raw);
            if (consumed == 0) break;
            raw.erase(0, consumed);
        }
    }

    // --- table detection (GFM) -----------------------------------------------
    static bool parse_delimiter_row_(std::string_view line, std::vector<int>& aligns) {
        std::string s = trim(line);
        if (s.empty()) return false;
        std::size_t i = 0;
        if (s[i] == '|') ++i;
        bool any = false;
        while (i < s.size()) {
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
            bool left = false, right = false;
            int dashes = 0;
            if (i < s.size() && s[i] == ':') { left = true; ++i; }
            while (i < s.size() && s[i] == '-') { ++dashes; ++i; }
            if (i < s.size() && s[i] == ':') { right = true; ++i; }
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
            if (dashes == 0) return false;
            int a = 0;
            if (left && right) a = 2;
            else if (right) a = 3;
            else if (left) a = 1;
            aligns.push_back(a);
            any = true;
            if (i < s.size()) {
                if (s[i] != '|') return false;
                ++i;
            }
        }
        return any;
    }

    static std::vector<std::string> split_table_row_(std::string_view line) {
        std::vector<std::string> cells;
        std::string s = trim(line);
        std::size_t i = 0;
        if (!s.empty() && s[i] == '|') ++i;
        std::string cur;
        for (; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size()) { cur.push_back(s[i]); cur.push_back(s[i + 1]); ++i; continue; }
            if (s[i] == '|') { cells.push_back(trim(cur)); cur.clear(); }
            else cur.push_back(s[i]);
        }
        if (!s.empty() && s.back() != '|') cells.push_back(trim(cur));
        else if (!cur.empty() || (!s.empty() && s.back() == '|')) { std::string t = trim(cur); if (!t.empty()) cells.push_back(t); }
        return cells;
    }

    // --- core block parsing --------------------------------------------------
    std::vector<Block> parse_blocks_(const std::vector<std::string>& lines) {
        std::vector<Block> out;
        std::size_t idx = 0;
        const std::size_t n = lines.size();
        while (idx < n) {
            const std::string& line = lines[idx];
            if (is_blank(line)) { ++idx; continue; }

            int hlevel; std::string htext;
            char fenceChar; int fenceIndent; std::string finfo;
            bool ordered; int lstart; char delim; char bullet; int markerCols; int afterMarkerCols;
            int htmlEndType;

            if (is_thematic_break_(line) && atx_heading_(line, htext) == 0) {
                Block b; b.kind = Block::Kind::Hr; out.push_back(std::move(b)); ++idx; continue;
            }
            if (int lvl = atx_heading_(line, htext); lvl > 0) {
                Block b; b.kind = Block::Kind::Heading; b.level = lvl; b.text = htext; out.push_back(std::move(b)); ++idx; continue;
            }
            if (int flen = fenced_open_(line, fenceChar, fenceIndent, finfo); flen > 0) {
                idx = parse_fenced_(lines, idx, flen, fenceChar, fenceIndent, finfo, out); continue;
            }
            if (int et = html_block_start_(line, htmlEndType); et > 0) {
                idx = parse_html_block_(lines, idx, et, out); continue;
            }
            {
                int cols; count_leading_indent(line, cols);
                if (!flags_.noIndentedCode && cols >= 4) {
                    idx = parse_indented_code_(lines, idx, out); continue;
                }
            }
            if (line_is_blockquote_(line)) {
                idx = parse_blockquote_(lines, idx, out); continue;
            }
            if (std::size_t mk = list_marker_(line, ordered, lstart, delim, bullet, markerCols, afterMarkerCols); mk > 0) {
                idx = parse_list_(lines, idx, out); continue;
            }
            (void)hlevel;
            // paragraph (with possible setext heading / table)
            idx = parse_paragraph_(lines, idx, out);
        }
        return out;
    }

    static bool line_is_blockquote_(std::string_view line) {
        int cols; std::size_t i = count_leading_indent(line, cols);
        return cols <= 3 && i < line.size() && line[i] == '>';
    }

    std::size_t parse_fenced_(const std::vector<std::string>& lines, std::size_t idx, int flen, char fenceChar, int fenceIndent, std::string info, std::vector<Block>& out) {
        Block b; b.kind = Block::Kind::Code; b.info = info;
        ++idx;
        std::string content;
        while (idx < lines.size()) {
            if (fenced_close_(lines[idx], fenceChar, flen)) { ++idx; break; }
            // strip up to fenceIndent leading spaces
            std::string_view l = lines[idx];
            int removed = 0; std::size_t k = 0;
            while (k < l.size() && removed < fenceIndent && (l[k] == ' ' || l[k] == '\t')) {
                removed += (l[k] == '\t') ? (4 - (removed % 4)) : 1;
                ++k;
            }
            content += std::string{l.substr(k)};
            content.push_back('\n');
            ++idx;
        }
        b.text = content;
        out.push_back(std::move(b));
        return idx;
    }

    std::size_t parse_indented_code_(const std::vector<std::string>& lines, std::size_t idx, std::vector<Block>& out) {
        Block b; b.kind = Block::Kind::Code;
        std::string content;
        std::vector<std::string> buffered;
        while (idx < lines.size()) {
            const std::string& l = lines[idx];
            if (is_blank(l)) { buffered.emplace_back(""); ++idx; continue; }
            int cols; count_leading_indent(l, cols);
            if (cols < 4) break;
            // flush buffered blanks
            for (auto& bl : buffered) { (void)bl; content.push_back('\n'); }
            buffered.clear();
            // strip 4 columns
            int removed = 0; std::size_t k = 0;
            while (k < l.size() && removed < 4) { removed += (l[k] == '\t') ? (4 - (removed % 4)) : 1; ++k; }
            content += std::string{l.substr(k)};
            content.push_back('\n');
            ++idx;
        }
        b.text = content;
        out.push_back(std::move(b));
        return idx;
    }

    std::size_t parse_html_block_(const std::vector<std::string>& lines, std::size_t idx, int type, std::vector<Block>& out) {
        Block b; b.kind = Block::Kind::Html;
        std::string content;
        // type 6/7 end on blank line; 1-5 on match
        while (idx < lines.size()) {
            const std::string& l = lines[idx];
            if ((type == 6 || type == 7)) {
                if (is_blank(l)) break;
                content += l; content.push_back('\n'); ++idx; continue;
            }
            content += l; content.push_back('\n');
            bool end = html_block_end_(l, type);
            ++idx;
            if (end) break;
        }
        b.text = content;
        out.push_back(std::move(b));
        return idx;
    }

    std::size_t parse_blockquote_(const std::vector<std::string>& lines, std::size_t idx, std::vector<Block>& out) {
        std::vector<std::string> inner;
        while (idx < lines.size()) {
            const std::string& l = lines[idx];
            if (line_is_blockquote_(l)) {
                int cols; std::size_t i = count_leading_indent(l, cols);
                ++i;  // skip '>'
                if (i < l.size() && l[i] == ' ') ++i;
                else if (i < l.size() && l[i] == '\t') ++i;
                inner.emplace_back(l.substr(i));
                ++idx;
            } else if (is_blank(l)) {
                break;
            } else {
                // lazy continuation: only if it could continue a paragraph
                if (could_be_lazy_continuation_(l)) { inner.emplace_back(l); ++idx; }
                else break;
            }
        }
        Block b; b.kind = Block::Kind::Quote;
        b.children = parse_blocks_(inner);
        out.push_back(std::move(b));
        return idx;
    }

    bool could_be_lazy_continuation_(std::string_view l) {
        if (is_blank(l)) return false;
        std::string t;
        if (is_thematic_break_(l)) return false;
        if (atx_heading_(l, t) > 0) return false;
        char fc; int fi; std::string finfo;
        if (fenced_open_(l, fc, fi, finfo) > 0) return false;
        if (line_is_blockquote_(l)) return false;
        bool ord; int st; char dl; char bu; int mc; int amc;
        if (list_marker_(l, ord, st, dl, bu, mc, amc) > 0) return false;
        int et;
        if (html_block_start_(l, et) > 0) return false;
        return true;
    }

    std::size_t parse_list_(const std::vector<std::string>& lines, std::size_t idx, std::vector<Block>& out) {
        bool ordered; int start; char delim; char bullet; int markerCols; int afterCols;
        list_marker_(lines[idx], ordered, start, delim, bullet, markerCols, afterCols);
        Block list; list.kind = Block::Kind::List; list.ordered = ordered; list.start = start; list.delim = delim; list.bullet = bullet;
        bool loose = false;
        bool sawBlankBetween = false;

        while (idx < lines.size()) {
            bool o2; int s2; char d2; char b2; int mc2; int ac2;
            std::size_t mk = list_marker_(lines[idx], o2, s2, d2, b2, mc2, ac2);
            if (mk == 0) break;
            if (o2 != ordered) break;
            if (ordered && d2 != delim) break;
            if (!ordered && b2 != bullet) break;

            // gather this item's lines
            std::vector<std::string> itemLines;
            // first line: strip through content indent
            {
                const std::string& l = lines[idx];
                std::string stripped = strip_cols_(l, ac2);
                itemLines.emplace_back(stripped);
                ++idx;
            }
            bool itemHadBlank = false;
            // continuation lines
            while (idx < lines.size()) {
                const std::string& l = lines[idx];
                if (is_blank(l)) {
                    itemLines.emplace_back("");
                    itemHadBlank = true;
                    ++idx;
                    continue;
                }
                int cols; count_leading_indent(l, cols);
                if (cols >= ac2) {
                    itemLines.emplace_back(strip_cols_(l, ac2));
                    ++idx;
                    continue;
                }
                // new marker at this level ends the item
                bool o3; int s3; char d3; char b3; int mc3; int ac3;
                if (list_marker_(l, o3, s3, d3, b3, mc3, ac3) > 0) break;
                // lazy paragraph continuation
                if (could_be_lazy_continuation_(l) && !itemHadBlank) { itemLines.emplace_back(l); ++idx; continue; }
                break;
            }
            // trim trailing blank lines belonging to separation
            bool trimmedTrailingBlank = false;
            while (!itemLines.empty() && is_blank(itemLines.back())) { itemLines.pop_back(); sawBlankBetween = true; trimmedTrailingBlank = true; }
            if (contains_blank_between_content_(itemLines)) loose = true;
            // A blank line trimmed from the end of this item separates it from a
            // following item -> loose list (only when another item follows).
            if (trimmedTrailingBlank && idx < lines.size()) {
                bool oN; int sN; char dN; char bN; int mcN; int acN;
                if (list_marker_(lines[idx], oN, sN, dN, bN, mcN, acN) > 0 && oN == ordered && (ordered ? dN == delim : bN == bullet)) {
                    loose = true;
                }
            }

            Block item; item.kind = Block::Kind::Item;
            // task list marker
            if (flags_.tasklists && !itemLines.empty()) {
                std::string& first = itemLines[0];
                std::string ft = ltrim(first);
                if (ft.size() >= 3 && ft[0] == '[' && (ft[1] == ' ' || ft[1] == 'x' || ft[1] == 'X') && ft[2] == ']' && (ft.size() == 3 || ft[3] == ' ')) {
                    item.task = true;
                    item.taskChecked = (ft[1] == 'x' || ft[1] == 'X');
                    std::size_t lead = first.size() - ft.size();
                    first = first.substr(0, lead) + (ft.size() > 3 ? ft.substr(4) : std::string{});
                }
            }
            item.children = parse_blocks_(itemLines);
            list.children.push_back(std::move(item));

            // check blank line separating from next marker
            if (idx < lines.size() && is_blank(lines[idx])) {
                std::size_t peek = idx;
                while (peek < lines.size() && is_blank(lines[peek])) ++peek;
                bool o4; int s4; char d4; char b4; int mc4; int ac4;
                if (peek < lines.size() && list_marker_(lines[peek], o4, s4, d4, b4, mc4, ac4) > 0 && o4 == ordered && (ordered ? d4 == delim : b4 == bullet)) {
                    loose = true;
                    idx = peek;
                } else {
                    break;
                }
            }
        }
        (void)sawBlankBetween;
        list.tight = !loose;
        out.push_back(std::move(list));
        return idx;
    }

    static bool contains_blank_between_content_(const std::vector<std::string>& itemLines) {
        bool seenContent = false, seenBlankAfter = false;
        for (auto& l : itemLines) {
            if (is_blank(l)) { if (seenContent) seenBlankAfter = true; }
            else { if (seenBlankAfter) return true; seenContent = true; }
        }
        return false;
    }

    // Remove up to `cols` display columns of leading characters. On an item's
    // first line these columns cover the list marker + following spaces; on a
    // continuation line they cover indentation. Any character counts (the
    // marker is not whitespace), which prevents re-parsing the marker.
    static std::string strip_cols_(std::string_view l, int cols) {
        int removed = 0; std::size_t k = 0;
        while (k < l.size() && removed < cols) {
            if (l[k] == '\t') removed += 4 - (removed % 4);
            else removed += 1;
            ++k;
        }
        return std::string{l.substr(k)};
    }

    std::size_t parse_paragraph_(const std::vector<std::string>& lines, std::size_t idx, std::vector<Block>& out) {
        std::vector<std::string> para;
        std::size_t startIdx = idx;
        while (idx < lines.size()) {
            const std::string& l = lines[idx];
            if (is_blank(l)) break;
            // setext underline (only if we already have paragraph content)
            int slevel;
            if (!para.empty() && setext_underline_(l, slevel)) {
                // But '-' underline could be a thematic break / list; setext wins if para exists
                std::string raw = join_(para);
                finalize_paragraph_text_(raw);
                if (!trim(raw).empty()) {
                    Block b; b.kind = Block::Kind::Heading; b.level = slevel; b.text = trim(raw);
                    out.push_back(std::move(b));
                    return idx + 1;
                }
                // paragraph was only ref defs; underline starts fresh
                break;
            }
            // interrupting constructs
            if (!para.empty()) {
                std::string t;
                if (is_thematic_break_(l)) break;
                if (atx_heading_(l, t) > 0) break;
                char fc; int fi; std::string finfo;
                if (fenced_open_(l, fc, fi, finfo) > 0) break;
                if (line_is_blockquote_(l)) break;
                int et;
                if (html_block_start_(l, et) > 0 && et != 7) break;
                bool o; int s; char d; char b2; int mc; int ac;
                if (std::size_t mk = list_marker_(l, o, s, d, b2, mc, ac); mk > 0) {
                    // ordered list can only interrupt if starting at 1; bullets always (if not blank content)
                    std::string rest = strip_cols_(l, ac);
                    bool emptyItem = trim(rest).empty();
                    if (!emptyItem && (!o || s == 1)) break;
                }
            }
            para.emplace_back(l);
            ++idx;
        }

        // GFM table detection
        if (flags_.tables && para.size() >= 2) {
            std::vector<int> aligns;
            if (para[0].find('|') != std::string::npos && parse_delimiter_row_(para[1], aligns)) {
                std::vector<std::string> headers = split_table_row_(para[0]);
                if (headers.size() == aligns.size()) {
                    Block t; t.kind = Block::Kind::Table; t.headers = headers; t.aligns = aligns;
                    for (std::size_t r = 2; r < para.size(); ++r) {
                        auto cells = split_table_row_(para[r]);
                        cells.resize(headers.size());
                        t.rows.push_back(std::move(cells));
                    }
                    out.push_back(std::move(t));
                    return idx;
                }
            }
        }

        std::string raw = join_(para);
        finalize_paragraph_text_(raw);
        std::string trimmed = trim(raw);
        if (!trimmed.empty()) {
            Block b; b.kind = Block::Kind::Para; b.text = trimmed;
            out.push_back(std::move(b));
        }
        (void)startIdx;
        return idx;
    }

    static std::string join_(const std::vector<std::string>& para) {
        std::string raw;
        for (std::size_t i = 0; i < para.size(); ++i) { if (i) raw.push_back('\n'); raw += para[i]; }
        return raw;
    }

    // --- rendering -----------------------------------------------------------
    void render_blocks_(const std::vector<Block>& blocks, std::string& out, bool tight) {
        for (const auto& b : blocks) render_block_(b, out, tight);
    }

    void render_block_(const Block& b, std::string& out, bool tight) {
        using K = Block::Kind;
        switch (b.kind) {
            case K::Hr: out += "<hr />\n"; break;
            case K::Heading: {
                out += "<h" + std::to_string(b.level) + ">";
                out += render_inline(b.text, refs_, inline_flags_());
                out += "</h" + std::to_string(b.level) + ">\n";
                break;
            }
            case K::Para: {
                if (tight) {
                    out += render_inline(b.text, refs_, inline_flags_());
                } else {
                    out += "<p>";
                    out += render_inline(b.text, refs_, inline_flags_());
                    out += "</p>\n";
                }
                break;
            }
            case K::Code: {
                out += "<pre><code";
                std::string lang = first_word_(b.info);
                if (!lang.empty()) {
                    out += " class=\"language-";
                    append_escaped_html(out, resolve_literal(lang));
                    out += "\"";
                }
                out += ">";
                append_escaped_html(out, b.text);
                out += "</code></pre>\n";
                break;
            }
            case K::Html: {
                out += b.text;  // verbatim (already newline-terminated per line)
                break;
            }
            case K::Quote: {
                out += "<blockquote>\n";
                render_blocks_(b.children, out, false);
                out += "</blockquote>\n";
                break;
            }
            case K::List: {
                if (b.ordered) {
                    out += "<ol";
                    if (b.start != 1) out += " start=\"" + std::to_string(b.start) + "\"";
                    out += ">\n";
                } else {
                    out += "<ul>\n";
                }
                for (const auto& item : b.children) render_item_(item, out, b.tight);
                out += b.ordered ? "</ol>\n" : "</ul>\n";
                break;
            }
            case K::Item: render_item_(b, out, tight); break;
            case K::Table: render_table_(b, out); break;
            case K::Doc: render_blocks_(b.children, out, tight); break;
        }
    }

    void render_item_(const Block& item, std::string& out, bool tight) {
        out += "<li>";
        if (item.task) {
            out += "<input ";
            if (item.taskChecked) out += "checked=\"\" ";
            out += "disabled=\"\" type=\"checkbox\"> ";
        }
        if (tight) {
            // Tight item: paragraphs unwrap to inline text on the <li> line; any
            // contained block (sub-list, code, quote, ...) keeps its own newlines.
            const auto& ch = item.children;
            for (std::size_t i = 0; i < ch.size(); ++i) {
                if (ch[i].kind == Block::Kind::Para) {
                    out += render_inline(ch[i].text, refs_, inline_flags_());
                    if (i + 1 < ch.size()) out += "\n";  // separate from following block
                } else {
                    render_block_(ch[i], out, true);
                }
            }
        } else {
            out += "\n";
            render_blocks_(item.children, out, false);
        }
        out += "</li>\n";
    }

    void render_table_(const Block& t, std::string& out) {
        static constexpr const char* alignAttr[] = {"", " align=\"left\"", " align=\"center\"", " align=\"right\""};
        out += "<table>\n<thead>\n<tr>\n";
        for (std::size_t c = 0; c < t.headers.size(); ++c) {
            out += "<th";
            out += alignAttr[t.aligns[c]];
            out += ">";
            out += render_inline(unescape_pipes_(t.headers[c]), refs_, inline_flags_());
            out += "</th>\n";
        }
        out += "</tr>\n</thead>\n";
        if (!t.rows.empty()) {
            out += "<tbody>\n";
            for (const auto& row : t.rows) {
                out += "<tr>\n";
                for (std::size_t c = 0; c < t.headers.size(); ++c) {
                    out += "<td";
                    out += alignAttr[t.aligns[c]];
                    out += ">";
                    out += render_inline(unescape_pipes_(c < row.size() ? row[c] : std::string{}), refs_, inline_flags_());
                    out += "</td>\n";
                }
                out += "</tr>\n";
            }
            out += "</tbody>\n";
        }
        out += "</table>\n";
    }

    static std::string unescape_pipes_(std::string_view s) {
        std::string out;
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '|') { out.push_back('|'); ++i; }
            else out.push_back(s[i]);
        }
        return out;
    }

    static std::string first_word_(std::string_view info) {
        std::string t = trim(info);
        std::size_t sp = 0;
        while (sp < t.size() && t[sp] != ' ' && t[sp] != '\t') ++sp;
        return t.substr(0, sp);
    }
};

}  // namespace detail

inline std::string render_to_html(std::string_view text, BlockFlags flags) {
    detail::BlockParser p{flags};
    return p.parse(text);
}

}  // namespace mbun::md::cm
