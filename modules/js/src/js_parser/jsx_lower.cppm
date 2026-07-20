// src/js_parser/jsx_lower.cppm — module mbun.js_parser.jsx_lower
//
// JSX/TSX lowering — hand-scanned straight off the source bytes — to EITHER the
// classic factory call (`React.createElement(...)`) or the automatic runtime
// (`jsxDEV(...)` / `jsx(...)` / `jsxs(...)` imported from `<source>/jsx-runtime`).
//
// ⚠️ The default is AUTOMATIC, not classic. This module emitted classic
// unconditionally until the automatic runtime was ported; that was wrong for
// bun's DEFAULT configuration, not just for an opt-in one. Verified on bun 1.4.0
// (`.mbun/bin/bun-rust`), no flags, no tsconfig:
//
//   $ echo 'const a = <div x="1"/>;' | Bun.Transpiler({loader:"tsx"}).transformSync
//   const a = jsxDEV_7x81h0kn("div", { x: "1" }, undefined, false, undefined, this);
//
// ref options_types/jsx.rs:186-197 `impl Default for Pragma` — `runtime:
// Runtime::Automatic`, `development: true`, `package_name: "react"`. So every
// `React.createElement` mbun used to print was a silent behavior divergence that
// no test caught: the classic output still *runs* whenever a `React` global
// happens to exist, which is exactly how the hono failures presented ("[object
// Object]" out of a shim, never an error).
//
// Why this is a module of its own (AGENTS.md 规则 10 — split by 职责): the JS
// tokenizer cannot lex JSX — a `</` close tag and the text runs between elements
// derail it — so `Parser::pre_lex_` hands any `<` that opens an element to this
// scanner, which consumes the whole element from raw source and hands back ONE
// lowered `React.createElement(...)` string. That makes the seam unusually
// narrow: the entire subsystem reads `src_` and nothing else. It touches no
// token, no AST node, no arena, no lexer — hence this module imports only `std`,
// where the parser imports mbun.js_lexer + mbun.ast.
//
// The contract with the parser is exactly two entry points, both called from
// pre_lex_ (see its JSX branch):
//
//   jsx_element_starts_at_(pos)   — does an element open just past the `<`?
//   jsx_parse_element_(p, out)    — scan one element; `p` in/out cursor, `out`
//                                   receives the lowered call expression.
//
// `Parser::parse_jsx_token_` — the bridge that turns the synthetic token into an
// arena edit + placeholder node — deliberately stays in the parser: it is the
// one piece of this path that speaks AST.
export module mbun.js_parser.jsx_lower;

import std;

export namespace mbun::js_parser::detail {

// ref options_types/jsx.rs:20-26 `enum Runtime`. bun carries `_None` (to
// round-trip an api::Jsx zero value) and `Solid`; neither reaches a lowering
// decision here, so this is the two-state the emitter actually branches on.
enum class JsxRuntime : std::uint8_t { Classic, Automatic };

// ref options_types/jsx.rs:160-183 `struct Pragma` + :186-197 its Default.
// bun stores factory/fragment as a MemberList (`["React","createElement"]`) to
// keep default+clone allocation-free; mbun keeps the dotted string because the
// lowerer's only consumer is string concatenation — splitting on '.' just to
// re-join it would be pure overhead.
struct JsxOptions {
    JsxRuntime runtime{JsxRuntime::Automatic};
    bool development{true};
    // `Pragma::package_name` — the bare specifier. The imported module is this
    // plus `/jsx-dev-runtime` or `/jsx-runtime` (ref jsx.rs:246-256
    // `set_import_source`), which `import_module()` below derives.
    std::string import_source{"react"};
    std::string factory{"React.createElement"};   // jsx.rs defaults::FACTORY
    std::string fragment{"React.Fragment"};       // jsx.rs defaults::FRAGMENT

    // Emit the automatic runtime's `import {jsxDEV as …} from …`. OFF by default
    // to match Bun.Transpiler.transformSync, which prints the call but not the
    // import; the module loader turns it on. See TranspileOptions in
    // js_parser.cppm for the full note on why bun splits these.
    bool inject_import{false};

    // ref jsx.rs:230-235 `import_source()`: development picks the dev entry.
    [[nodiscard]] std::string import_module() const {
        return import_source + (development ? "/jsx-dev-runtime" : "/jsx-runtime");
    }
};

// Which automatic-runtime symbols the lowered output actually referenced, so
// the caller imports exactly those and no more.
// ref js_parser/parser.rs:668-674 `JSXImportSymbols` + :719-746
// `runtime_import_names`/`source_import_names`, which serve the same purpose in
// bun (there as Refs into the symbol table, here as bools — mbun's lowerer emits
// text, so there is nothing to point at).
struct JsxUsed {
    bool jsx{false};
    bool jsxs{false};
    bool jsx_dev{false};
    bool fragment{false};

    [[nodiscard]] bool any() const { return jsx || jsxs || jsx_dev || fragment; }
};

// The generated symbol names for the automatic runtime.
//
// `kJsxDev` and `kFragment` are bun 1.4.0's own, byte-for-byte — the oracle
// prints `jsxDEV_7x81h0kn` / `Fragment_8vg9x3sq` for every input, and the
// suffixes are CONSTANT (verified across differing file contents, names, and
// paths), so matching them costs nothing and keeps mbun's transformSync output
// identical to bun's on the corpus.
//
// `kJsx` / `kJsxs` are mbun's own: bun's spellings are not observable through
// any 1.4.0 surface found. Bun.Transpiler ignores its `jsx` constructor option
// (`{runtime:"automatic",development:false}` still emits `jsxDEV_7x81h0kn`), and
// the bundler — the only other way to reach `development:false` — renames
// generated symbols to plain `jsx`/`jsxs` before printing. They are only ever
// emitted when `development == false`, which no mbun caller currently selects.
// The suffix's job is collision-avoidance with user identifiers, and these do
// that; if bun's real names surface later this is a one-line change.
inline constexpr std::string_view kJsxDev{"jsxDEV_7x81h0kn"};
inline constexpr std::string_view kFragment{"Fragment_8vg9x3sq"};
inline constexpr std::string_view kJsx{"jsx_7x81h0kn"};
inline constexpr std::string_view kJsxs{"jsxs_7x81h0kn"};

// Scans and lowers JSX elements out of raw source. Holds the source view, the
// resolved per-file options, and a record of which runtime symbols it emitted:
// every method below is otherwise a pure function of `src_` plus its cursor,
// which is why the parser can build one on demand rather than keeping JSX state.
class JsxLowerer {
private:
    std::string_view src_;
    JsxOptions opts_;
    JsxUsed used_;
    // Every NAME this lowering emits into its replacement text as a VALUE
    // reference. The lowering is a source rewriter: `<Foo a={v} {...p}/>`
    // becomes the TEXT `jsxDEV(Foo, {a: v, ...p})`, so `Foo`/`v`/`p` are never
    // lexed as expressions and the parser's identifier arm never sees them.
    // Without this list, TS unused-import trimming drops `import {Foo}` while
    // still emitting `Foo` — a ReferenceError. Collected here (this class owns
    // the rewrite) and drained by the Parser, which owns the trimmer.
    // Deliberately OVER-inclusive: every identifier-like run inside a copied
    // expression is noted, `o.prop` contributing both `o` and `prop`. The error
    // is one-directional — a stray name can only KEEP an import, never drop a
    // live one.
    std::vector<std::string> jsxRefs_;

    // ── recursion budget ─────────────────────────────────────────────────────
    // `jsx_parse_element_` and `jsx_parse_children_` recurse into each other once
    // per nesting level, straight off the source bytes. Unlike the parser's own
    // descent (token_cursor.cppm kMaxParseDepth) this scanner had no cap, so a
    // source that nests thousands of elements — `("() => <div>").repeat(50_000)`
    // parses the `() => ` runs as JSX text, nesting one `<div>` per repetition —
    // ran the NATIVE stack out and died on the guard page with a bare SIGSEGV.
    // bun bounds the same recursion and reports a catchable
    // "Maximum call stack size exceeded" instead.
    // ref: compat/bun/test/bundler/transpiler/jsx-deep-nesting-stack-overflow.test.ts
    // The cap matches the parser's: a nesting count, not a byte budget, set far
    // past anything real JSX nests.
    static constexpr int kMaxJsxDepth{1000};
    int depth_{0};
    bool overflowed_{false};

public:
    explicit JsxLowerer(std::string_view src, JsxOptions opts = {})
        : src_{src}, opts_{std::move(opts)} {}

    // True when a scan bailed out on kMaxJsxDepth rather than on bad syntax, so
    // the parser can report the overflow instead of "Unexpected token in JSX".
    [[nodiscard]] bool overflowed() const { return overflowed_; }

    [[nodiscard]] const JsxUsed& used() const { return used_; }
    // The names above. Non-empty only for a JSX file that lowered an element.
    [[nodiscard]] const std::vector<std::string>& jsx_value_refs() const { return jsxRefs_; }
    [[nodiscard]] const JsxOptions& options() const { return opts_; }

    // Note every identifier-like run in `text` as a value reference.
    void jsx_note_refs_(std::string_view text) {
        std::size_t i = 0;
        while (i < text.size()) {
            if (!jsx_is_id_char_(text[i]) || (text[i] >= '0' && text[i] <= '9')) {
                ++i;
                continue;
            }
            const std::size_t s = i;
            while (i < text.size() && jsx_is_id_char_(text[i])) {
                ++i;
            }
            jsxRefs_.emplace_back(text.substr(s, i - s));
        }
    }

    static bool jsx_is_ws_(char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    }
    static bool jsx_is_id_char_(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '$';
    }
    static bool is_jsx_name_start_(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
    }
    void jsx_skip_ws_(std::size_t& p) const {
        while (p < src_.size() && jsx_is_ws_(src_[p])) {
            ++p;
        }
    }

    // Does a JSX element begin just after a `<` at byte `pos` (a tag-name start, or
    // `>` for a fragment)? Cheap gate the pre-lexer uses before hand-scanning.
    bool jsx_element_starts_at_(std::size_t pos) const {
        if (pos >= src_.size()) {
            return false;
        }
        char c = src_[pos];
        return c == '>' || is_jsx_name_start_(c);
    }

    // A JSX tag / member name: `Foo`, `foo`, `foo.Bar`, `custom-element`, `a:b`.
    std::string jsx_scan_name_(std::size_t& p) const {
        const std::size_t n = src_.size();
        std::size_t start = p;
        if (p >= n || !is_jsx_name_start_(src_[p])) {
            return {};
        }
        ++p;
        while (p < n && (jsx_is_id_char_(src_[p]) || src_[p] == '.' || src_[p] == '-' ||
                         src_[p] == ':')) {
            ++p;
        }
        return std::string{src_.substr(start, p - start)};
    }

    // The tag expression: lowercase simple name → intrinsic string ("div"); a
    // dotted name (member) or a Capitalized name → identifier reference as-is.
    static std::string jsx_tag_expr_(const std::string& name) {
        if (name.find('.') != std::string::npos) {
            return name;  // member expression, e.g. Foo.Bar
        }
        if (!name.empty() && name[0] >= 'a' && name[0] <= 'z') {
            return jsx_quote_string_(name);  // intrinsic → string literal
        }
        return name;  // component identifier
    }

    // An attribute object key: a plain JS identifier stays bare, anything else
    // (hyphen / colon / leading digit) becomes a quoted string key.
    static std::string jsx_attr_key_(std::string_view name) {
        bool simple = !name.empty() && !(name[0] >= '0' && name[0] <= '9');
        for (char c : name) {
            if (!jsx_is_id_char_(c)) {
                simple = false;
            }
        }
        return simple ? std::string{name} : jsx_quote_string_(name);
    }

    static std::string jsx_quote_string_(std::string_view s) {
        std::string out{"\""};
        for (char c : s) {
            switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
            }
        }
        out += '"';
        return out;
    }

    static std::string jsx_trim_(std::string_view s) {
        std::size_t a = 0;
        std::size_t b = s.size();
        while (a < b && jsx_is_ws_(s[a])) {
            ++a;
        }
        while (b > a && jsx_is_ws_(s[b - 1])) {
            --b;
        }
        return std::string{s.substr(a, b - a)};
    }

    // The standard JSX text-whitespace fold (matches babel/esbuild): split on line
    // breaks, drop leading/trailing whitespace adjacent to a newline, collapse the
    // rest with single spaces. Whitespace-only multi-line text folds to empty.
    static std::string jsx_clean_text_(std::string_view text) {
        std::vector<std::string> lines;
        std::string cur;
        for (std::size_t i = 0; i < text.size(); ++i) {
            char c = text[i];
            if (c == '\n') {
                lines.push_back(cur);
                cur.clear();
            } else if (c == '\r') {
                lines.push_back(cur);
                cur.clear();
                if (i + 1 < text.size() && text[i + 1] == '\n') {
                    ++i;
                }
            } else {
                cur += (c == '\t') ? ' ' : c;
            }
        }
        lines.push_back(cur);
        std::size_t lastNonEmpty = 0;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].find_first_not_of(' ') != std::string::npos) {
                lastNonEmpty = i;
            }
        }
        std::string result;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            std::string line = lines[i];
            if (i != 0) {
                std::size_t s = line.find_first_not_of(' ');
                line = (s == std::string::npos) ? std::string{} : line.substr(s);
            }
            if (i + 1 != lines.size()) {
                std::size_t e = line.find_last_not_of(' ');
                line = (e == std::string::npos) ? std::string{} : line.substr(0, e + 1);
            }
            if (!line.empty()) {
                if (i != lastNonEmpty) {
                    line += ' ';
                }
                result += line;
            }
        }
        return result;
    }

    // Copy a raw `'…'` / `"…"` string literal (quotes included), honoring escapes.
    std::string jsx_scan_quoted_(std::size_t& p) const {
        const std::size_t n = src_.size();
        char q = src_[p];
        std::string out;
        out += src_[p++];
        while (p < n && src_[p] != q) {
            if (src_[p] == '\\' && p + 1 < n) {
                out += src_[p++];
                out += src_[p++];
                continue;
            }
            out += src_[p++];
        }
        if (p < n) {
            out += src_[p++];  // closing quote
        }
        return out;
    }

    // Should a `<` inside a JSX `{expr}` begin a nested element? Yes at any
    // non-value position, or right after an expression-introducing keyword.
    static bool jsx_expr_wants_element_(bool valueBefore, const std::string& lastWord) {
        if (!valueBefore) {
            return true;
        }
        static constexpr std::string_view kws[]{"return",  "typeof",     "void",  "delete",
                                                "in",      "instanceof", "new",   "do",
                                                "else",    "yield",      "await", "case",
                                                "default", "throw"};
        for (std::string_view k : kws) {
            if (lastWord == k) {
                return true;
            }
        }
        return false;
    }

    bool jsx_copy_regex_(std::size_t& p, std::string& out) const {
        const std::size_t n = src_.size();
        out += src_[p++];  // '/'
        bool inClass = false;
        while (p < n) {
            char c = src_[p];
            if (c == '\\') {
                out += c;
                if (p + 1 < n) {
                    out += src_[p + 1];
                }
                p += 2;
                continue;
            }
            if (c == '\n') {
                return false;
            }
            if (c == '[') {
                inClass = true;
            } else if (c == ']') {
                inClass = false;
            } else if (c == '/' && !inClass) {
                out += c;
                ++p;
                break;
            }
            out += c;
            ++p;
        }
        while (p < n && jsx_is_id_char_(src_[p])) {  // flags
            out += src_[p++];
        }
        return true;
    }

    bool jsx_copy_template_(std::size_t& p, std::string& out) {
        const std::size_t n = src_.size();
        out += src_[p++];  // '`'
        while (p < n) {
            char c = src_[p];
            if (c == '\\') {
                out += c;
                if (p + 1 < n) {
                    out += src_[p + 1];
                }
                p += 2;
                continue;
            }
            if (c == '`') {
                out += c;
                ++p;
                return true;
            }
            if (c == '$' && p + 1 < n && src_[p + 1] == '{') {
                out += src_[p++];  // $
                out += src_[p++];  // {
                std::string inner;
                bool hc = false;
                if (!jsx_copy_expr_(p, inner, hc)) {  // consumes the matching '}'
                    return false;
                }
                out += inner;
                out += '}';
                continue;
            }
            out += c;
            ++p;
        }
        return false;
    }

    // Copy a JS expression starting just after `{` up to its matching `}` (which is
    // consumed). Strings/templates/comments/regex are passed through untouched;
    // nested JSX elements are transformed in place. `hasContent` becomes true when
    // a significant (non-whitespace, non-comment) token is seen — a `{}` or
    // `{/*…*/}` child folds away.
    // The single choke point for every `{…}` the lowering copies verbatim into
    // its output — attribute values, {...spreads} and children all route here,
    // so noting refs once here covers all three.
    bool jsx_copy_expr_(std::size_t& p, std::string& out, bool& hasContent) {
        const std::size_t outStart = out.size();
        struct NoteOnExit {
            JsxLowerer* self;
            const std::string* out;
            std::size_t from;
            ~NoteOnExit() { self->jsx_note_refs_(std::string_view{*out}.substr(from)); }
        } note{this, &out, outStart};
        const std::size_t n = src_.size();
        int depth = 0;              // nested () [] {} depth inside the container
        bool valueBefore = false;   // previous significant token produced a value
        std::string lastWord;       // last identifier word (keyword lookahead)
        while (p < n) {
            char c = src_[p];
            if (c == '}' && depth == 0) {
                ++p;  // consume the closing brace
                return true;
            }
            if (c == '"' || c == '\'') {
                out += jsx_scan_quoted_(p);
                valueBefore = true;
                lastWord.clear();
                hasContent = true;
                continue;
            }
            if (c == '`') {
                if (!jsx_copy_template_(p, out)) {
                    return false;
                }
                valueBefore = true;
                lastWord.clear();
                hasContent = true;
                continue;
            }
            if (c == '/' && p + 1 < n && src_[p + 1] == '/') {
                while (p < n && src_[p] != '\n') {
                    out += src_[p++];
                }
                continue;
            }
            if (c == '/' && p + 1 < n && src_[p + 1] == '*') {
                out += src_[p++];
                out += src_[p++];
                while (p < n && !(src_[p] == '*' && p + 1 < n && src_[p + 1] == '/')) {
                    out += src_[p++];
                }
                if (p + 1 < n) {
                    out += src_[p++];
                    out += src_[p++];
                }
                continue;
            }
            if (c == '/' && !valueBefore) {
                if (!jsx_copy_regex_(p, out)) {
                    return false;
                }
                valueBefore = true;
                lastWord.clear();
                hasContent = true;
                continue;
            }
            if (c == '<' && jsx_expr_wants_element_(valueBefore, lastWord) && p + 1 < n &&
                (is_jsx_name_start_(src_[p + 1]) || src_[p + 1] == '>')) {
                if (!jsx_parse_element_(p, out)) {
                    return false;
                }
                valueBefore = true;
                lastWord.clear();
                hasContent = true;
                continue;
            }
            if (c == '{' || c == '(' || c == '[') {
                ++depth;
                valueBefore = false;
                lastWord.clear();
                out += c;
                ++p;
                continue;
            }
            if (c == '}' || c == ')' || c == ']') {
                --depth;
                valueBefore = true;
                lastWord.clear();
                out += c;
                ++p;
                continue;
            }
            if (jsx_is_id_char_(c)) {
                lastWord.clear();
                while (p < n && jsx_is_id_char_(src_[p])) {
                    out += src_[p];
                    lastWord += src_[p];
                    ++p;
                }
                valueBefore = true;
                hasContent = true;
                continue;
            }
            if (!jsx_is_ws_(c)) {
                valueBefore = false;
                lastWord.clear();
                hasContent = true;
            }
            out += c;
            ++p;
        }
        return false;  // unterminated
    }

    // The attributes of one element, as the emitter needs them.
    struct JsxAttrs {
        std::vector<std::string> parts;  // `a: 1`, `...rest`, in source order
        std::string key;                 // automatic runtime: the `key` prop's VALUE
        bool key_after_spread{false};    // `<div {...p} key="k"/>` — see below
    };

    // Parse the attribute list between a tag name and the closing `>` / `/>`.
    //
    // On the automatic runtime `key` is NOT a prop: it is lifted out of the
    // object and passed as the call's third argument (ref ast/e.rs:614
    // `React.jsxDEV(type, arguments, key, ...)`; oracle: `<div key="k" x="1"/>`
    // -> `jsxDEV_7x81h0kn("div", { x: "1" }, "k", false, undefined, this)`).
    // Classic keeps it as an ordinary prop, so the lift is gated on the runtime.
    //
    // `key_after_spread` records `<div {...p} key="k"/>`, where lifting `key`
    // would CHANGE MEANING — the spread could itself carry a `key`, and object
    // order decides which wins. bun refuses to guess and drops the element to
    // the classic runtime ("key" prop after a {...spread} is deprecated in JSX.
    // Falling back to classic runtime.); the caller does the same.
    bool jsx_parse_attributes_(std::size_t& p, JsxAttrs& attrs) {
        const std::size_t n = src_.size();
        const bool lift_key = opts_.runtime == JsxRuntime::Automatic;
        bool saw_spread = false;
        while (true) {
            jsx_skip_ws_(p);
            if (p >= n) {
                return false;
            }
            char c = src_[p];
            if (c == '>' || (c == '/' && p + 1 < n && src_[p + 1] == '>')) {
                break;
            }
            if (c == '{') {
                ++p;  // '{'
                std::string expr;
                bool hc = false;
                if (!jsx_copy_expr_(p, expr, hc)) {
                    return false;  // {...spread}
                }
                attrs.parts.push_back(jsx_trim_(expr));  // "...rest" → object spread
                saw_spread = true;
                continue;
            }
            std::string name = jsx_scan_name_(p);
            if (name.empty()) {
                return false;
            }
            const bool is_key = lift_key && name == "key";
            std::string keyed = jsx_attr_key_(name);
            jsx_skip_ws_(p);
            std::string value;
            if (p < n && src_[p] == '=') {
                ++p;
                jsx_skip_ws_(p);
                if (p >= n) {
                    return false;
                }
                if (src_[p] == '"' || src_[p] == '\'') {
                    value = jsx_scan_quoted_(p);
                } else if (src_[p] == '{') {
                    ++p;
                    std::string expr;
                    bool hc = false;
                    if (!jsx_copy_expr_(p, expr, hc)) {
                        return false;
                    }
                    value = jsx_trim_(expr);
                } else {
                    return false;
                }
            } else {
                value = "true";  // boolean shorthand
            }
            if (is_key) {
                attrs.key = std::move(value);
                attrs.key_after_spread = saw_spread;
            } else {
                attrs.parts.push_back(keyed + ": " + value);
            }
        }
        return true;
    }

    // `{ a: 1, ...rest }` from the collected parts, or `{}` when there are none.
    // The automatic runtime always passes an object (oracle: `<div/>` ->
    // `jsxDEV_7x81h0kn("div", {}, ...)`), where classic passes `null`.
    static std::string jsx_object_(const std::vector<std::string>& parts) {
        if (parts.empty()) {
            return "{}";
        }
        std::string out{"{ "};
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (i != 0) {
                out += ", ";
            }
            out += parts[i];
        }
        out += " }";
        return out;
    }

    // Parse element children up to (and consuming) the matching close tag.
    bool jsx_parse_children_(std::size_t& p, std::vector<std::string>& out) {
        const std::size_t n = src_.size();
        while (p < n) {
            char c = src_[p];
            if (c == '<' && p + 1 < n && src_[p + 1] == '/') {
                p += 2;  // '</'
                jsx_skip_ws_(p);
                jsx_scan_name_(p);  // (empty for a fragment close)
                jsx_skip_ws_(p);
                if (p < n && src_[p] == '>') {
                    ++p;
                }
                return true;
            }
            if (c == '<') {
                std::string child;
                if (!jsx_parse_element_(p, child)) {
                    return false;
                }
                out.push_back(std::move(child));
                continue;
            }
            if (c == '{') {
                ++p;  // '{'
                std::string expr;
                bool hasContent = false;
                if (!jsx_copy_expr_(p, expr, hasContent)) {
                    return false;
                }
                if (hasContent) {
                    out.push_back(jsx_trim_(expr));
                }
                continue;
            }
            std::size_t textStart = p;
            while (p < n && src_[p] != '<' && src_[p] != '{') {
                ++p;
            }
            std::string cleaned = jsx_clean_text_(src_.substr(textStart, p - textStart));
            if (!cleaned.empty()) {
                out.push_back(jsx_quote_string_(cleaned));
            }
        }
        return false;  // unterminated
    }

    // Emit ONE lowered call. The whole classic/automatic split lives here; the
    // scanning above is runtime-agnostic.
    //
    // Classic  (ref jsx.rs defaults::FACTORY, oracle `/** @jsxRuntime classic */`):
    //     factory(tag, props|null, ...children)
    //
    // Automatic dev (ref ast/e.rs:614-628; oracle, the DEFAULT):
    //     jsxDEV(tag, {...props, children}, key|undefined, isStaticChildren,
    //            undefined, this)
    //   — 6 args, always `jsxDEV` (never a `jsxsDEV`); the static-children flag
    //     carries what `jsxs` carries in production. ref bun_core/
    //     feature_flags.rs:38 "…and \"jsxs\" is not used".
    //
    // Automatic prod (oracle, `NODE_ENV=production bun build`):
    //     jsx(tag, {...props, children})            — 0/1 child
    //     jsxs(tag, {...props, children: [...]})    — 2+ children
    //     …with `key` appended as a 3rd arg only when present.
    //
    // `children` folds INTO the props object on the automatic runtime (0 -> no
    // key, 1 -> `children: x`, 2+ -> `children: [x, y]`), where classic spreads
    // them as trailing call arguments.
    void jsx_emit_call_(std::string& out, const std::string& tag, const JsxAttrs& attrs,
                        const std::vector<std::string>& children, bool classic) {
        if (classic) {
            out += opts_.factory + "(" + tag + ", " +
                   (attrs.parts.empty() ? std::string{"null"} : jsx_object_(attrs.parts));
            for (const std::string& child : children) {
                out += ", ";
                out += child;
            }
            out += ")";
            return;
        }
        std::vector<std::string> parts{attrs.parts};
        if (children.size() == 1) {
            parts.push_back("children: " + children[0]);
        } else if (children.size() > 1) {
            // `[ a, b ]` — the inner padding matches the object literal's `{ … }`
            // above, and collapses to bun's own spelling once its multi-line
            // printer output is whitespace-normalised.
            std::string arr{"children: [ "};
            for (std::size_t i = 0; i < children.size(); ++i) {
                if (i != 0) {
                    arr += ", ";
                }
                arr += children[i];
            }
            arr += " ]";
            parts.push_back(std::move(arr));
        }
        const bool is_static = children.size() > 1;
        if (opts_.development) {
            used_.jsx_dev = true;
            out += std::string{kJsxDev} + "(" + tag + ", " + jsx_object_(parts) + ", " +
                   (attrs.key.empty() ? std::string{"undefined"} : attrs.key) + ", " +
                   (is_static ? "true" : "false") + ", undefined, this)";
            return;
        }
        if (is_static) {
            used_.jsxs = true;
        } else {
            used_.jsx = true;
        }
        out += std::string{is_static ? kJsxs : kJsx} + "(" + tag + ", " + jsx_object_(parts);
        if (!attrs.key.empty()) {
            out += ", " + attrs.key;
        }
        out += ")";
    }

    // Parse a whole JSX element (or `<>…</>` fragment) at `p`, appending its
    // lowered call to `out`. `p` ends just past the element.
    bool jsx_parse_element_(std::size_t& p, std::string& out) {
        const std::size_t n = src_.size();
        if (p >= n || src_[p] != '<') {
            return false;
        }
        // RAII depth budget: unwinds on every exit path below, so the counter
        // always tracks the live recursion (see kMaxJsxDepth).
        struct DepthGuard {
            JsxLowerer* self;
            bool ok;
            explicit DepthGuard(JsxLowerer* s) : self{s} {
                ok = ++self->depth_ <= kMaxJsxDepth;
                if (!ok) {
                    self->overflowed_ = true;
                }
            }
            DepthGuard(const DepthGuard&) = delete;
            DepthGuard& operator=(const DepthGuard&) = delete;
            ~DepthGuard() { --self->depth_; }
        } depth{this};
        if (!depth.ok) {
            return false;
        }
        ++p;  // '<'
        jsx_skip_ws_(p);
        // Per-ELEMENT, not per-file: `key` after a spread demotes just this call
        // (see jsx_parse_attributes_), so an element's runtime is decided here.
        bool classic = opts_.runtime == JsxRuntime::Classic;
        std::string tag;
        JsxAttrs attrs;
        bool self_closing = false;
        if (p < n && src_[p] == '>') {
            // `<>` — the fragment tag. Classic names it (`React.Fragment`, or
            // whatever `@jsxFrag` set); automatic imports it.
            if (classic) {
                tag = opts_.fragment;
            } else {
                tag = std::string{kFragment};
                used_.fragment = true;
            }
            ++p;  // '>' of `<>`
        } else {
            std::string name = jsx_scan_name_(p);
            if (name.empty()) {
                return false;
            }
            tag = jsx_tag_expr_(name);
            // A tag that did NOT become a quoted string is an identifier or a
            // member chain (jsx_tag_expr_:175 draws that line): `<Foo/>` and
            // `<N.X/>` reference `Foo`/`N`, `<div/>` references nothing.
            if (!tag.empty() && tag.front() != '"') {
                jsx_note_refs_(tag);
            }
            if (!jsx_parse_attributes_(p, attrs)) {
                return false;
            }
            if (attrs.key_after_spread && !classic) {
                // ref oracle: bun reports `"key" prop after a {...spread} is
                // deprecated in JSX. Falling back to classic runtime.` and does
                // exactly that. Put `key` back where it was written — classic
                // has no third argument to lift it into.
                classic = true;
                attrs.parts.push_back("key: " + attrs.key);
                attrs.key.clear();
            }
            jsx_skip_ws_(p);
            if (p + 1 < n && src_[p] == '/' && src_[p + 1] == '>') {
                p += 2;  // self-closing → no children
                self_closing = true;
            } else {
                if (p >= n || src_[p] != '>') {
                    return false;
                }
                ++p;  // end of open tag
            }
        }
        std::vector<std::string> children;
        if (!self_closing && !jsx_parse_children_(p, children)) {
            return false;
        }
        jsx_emit_call_(out, tag, attrs, children, classic);
        return true;
    }
};

}  // namespace mbun::js_parser::detail
