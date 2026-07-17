// src/js_parser/jsx_pragma.cppm — module mbun.js_parser.jsx_pragma
//
// The per-file JSX comment pragmas: `@jsxRuntime`, `@jsx`, `@jsxFrag`,
// `@jsxImportSource`. They override the caller's JsxOptions for one file.
//
// Blueprint (.mbun/bun-ref/src):
//   - js_parser/lexer.rs:2589-2656  the four `has_prefix_with_word_boundary`
//     arms that populate `jsx_pragma._jsx / _jsx_frag / _jsx_runtime /
//     _jsx_import_source` — scanned per COMMENT, which is why this module has
//     to walk the source rather than grep it (see below).
//   - options_types/jsx.rs:44-53   RUNTIME_MAP ("classic"/"automatic"/"react"/
//     "react-jsx"/"react-jsxdev") — the accepted `@jsxRuntime` values.
//   - options_types/jsx.rs:177     the `/** @jsxImportSource @emotion/core */`
//     doc comment this implements.
//
// Why a source WALK and not `src.find("@jsxRuntime")` — three behaviors of bun
// 1.4.0 (`.mbun/bin/bun-rust`), each verified on the oracle, that a plain search
// would get wrong:
//
//   1. Pragmas are honored ONLY inside comments. `const s = "@jsxRuntime
//      classic";` and the same text in a template literal both leave the file on
//      the automatic runtime — a `find()` would flip it to classic.
//   2. A word boundary is required: `@jsxRuntimeXclassic` is NOT a pragma.
//   3. Position is irrelevant. A pragma in a `//` line comment, mid-file, or
//      even AFTER the last JSX element still applies to the WHOLE file — bun
//      lexes every comment before it visits. That is why this runs as a
//      pre-pass over the entire source instead of during lowering: mbun lowers
//      JSX in source order inside the pre-lexer, so a trailing pragma would
//      otherwise arrive too late to affect the elements above it.
//
// The walker only needs to know where comments AREN'T, so it tracks strings,
// template literals and regex-vs-divide well enough to skip them, and does not
// tokenize anything else.
export module mbun.js_parser.jsx_pragma;

import std;
import mbun.js_parser.jsx_lower;

export namespace mbun::js_parser::detail {

// The outcome of scanning one file's comments. `error` is set when a pragma
// carries a value bun rejects (`@jsxRuntime bogus` -> "Unsupported JSX runtime")
// so the caller can fail the parse the way bun does rather than silently
// ignoring the directive.
struct JsxPragmaResult {
    bool ok{true};
    std::string error;
};

namespace jsx_pragma_impl {

inline bool is_id_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '$';
}

inline bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// ref lexer.rs `strings::has_prefix_with_word_boundary`: the keyword must match
// AND not run on into another identifier character, so `@jsxRuntimeXclassic`
// does not match `jsxRuntime`. Verified on the oracle.
inline bool has_prefix_with_word_boundary(std::string_view chunk, std::string_view kw) {
    if (!chunk.starts_with(kw)) {
        return false;
    }
    return chunk.size() == kw.size() || !is_id_char(chunk[kw.size()]);
}

// A pragma's argument: the run of non-space bytes after the keyword. bun's
// `PragmaArg::scan(SkipSpaceFirst, ...)` — the value ends at whitespace, so
// `@jsxRuntime classic @jsx h` yields "classic" and leaves `@jsx h` to be
// matched by the next iteration (both pragmas in ONE comment is a real form:
// `/* @jsxRuntime classic @jsx h @jsxFrag F */` -> `h(F, null, "x")`).
inline std::string_view scan_arg(std::string_view body, std::size_t& p) {
    while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) {
        ++p;
    }
    const std::size_t start = p;
    while (p < body.size() && !is_ws(body[p]) && body[p] != '*') {
        ++p;
    }
    return body.substr(start, p - start);
}

// Apply every pragma found in one comment body to `opts`.
inline bool apply_comment(std::string_view body, JsxOptions& opts, std::string& error) {
    for (std::size_t i = 0; i < body.size(); ++i) {
        if (body[i] != '@') {
            continue;
        }
        std::string_view rest = body.substr(i + 1);
        // Order matters: `jsx` is a prefix of the other three, but
        // has_prefix_with_word_boundary rejects `jsxFrag` for keyword `jsx`
        // (the 'F' is an id char), so the longer names are tried first only to
        // keep the scan cheap, not for correctness.
        if (has_prefix_with_word_boundary(rest, "jsxImportSource")) {
            std::size_t p = i + 1 + std::string_view{"jsxImportSource"}.size();
            const std::string_view v = scan_arg(body, p);
            if (!v.empty()) {
                opts.import_source = std::string{v};
            }
            i = p - 1;
        } else if (has_prefix_with_word_boundary(rest, "jsxRuntime")) {
            std::size_t p = i + 1 + std::string_view{"jsxRuntime"}.size();
            const std::string_view v = scan_arg(body, p);
            if (!v.empty()) {
                // ref options_types/jsx.rs:44-53 RUNTIME_MAP. bun's map also
                // carries a `development` override for the react-jsx* spellings;
                // every entry that sets it sets it to `true`, which is already
                // the default, so only the runtime is read here.
                if (v == "classic" || v == "react") {
                    opts.runtime = JsxRuntime::Classic;
                } else if (v == "automatic" || v == "react-jsx" || v == "react-jsxdev") {
                    opts.runtime = JsxRuntime::Automatic;
                } else {
                    // ref oracle: `/** @jsxRuntime bogus */` ->
                    // `error: Unsupported JSX runtime: "bogus"`.
                    error = "Unsupported JSX runtime: \"" + std::string{v} + "\"";
                    return false;
                }
            }
            i = p - 1;
        } else if (has_prefix_with_word_boundary(rest, "jsxFrag")) {
            std::size_t p = i + 1 + std::string_view{"jsxFrag"}.size();
            const std::string_view v = scan_arg(body, p);
            if (!v.empty()) {
                opts.fragment = std::string{v};
            }
            i = p - 1;
        } else if (has_prefix_with_word_boundary(rest, "jsx")) {
            std::size_t p = i + 1 + std::string_view{"jsx"}.size();
            const std::string_view v = scan_arg(body, p);
            if (!v.empty()) {
                opts.factory = std::string{v};
            }
            i = p - 1;
        }
    }
    return true;
}

}  // namespace jsx_pragma_impl

// Walk `src`, applying the pragmas in every comment to `opts`.
//
// The walk skips string/template/regex bodies so their contents can never be
// mistaken for a comment (oracle: a pragma inside a string is inert). Regex
// detection reuses the same "was the previous significant token a value?"
// heuristic the JSX lowerer uses for `/`.
inline JsxPragmaResult scan_jsx_pragmas(std::string_view src, JsxOptions& opts) {
    const std::size_t n = src.size();
    bool value_before = false;  // previous significant token produced a value
    std::size_t i = 0;
    while (i < n) {
        const char c = src[i];
        if (c == '"' || c == '\'') {
            const char q = c;
            ++i;
            while (i < n && src[i] != q) {
                i += (src[i] == '\\') ? 2 : 1;
            }
            ++i;
            value_before = true;
            continue;
        }
        if (c == '`') {
            // Template literals nest `${…}`, which can contain anything —
            // including comments. Tracking that properly needs the real lexer;
            // skipping to the closing backtick is enough here because a pragma
            // is only ever authored at statement level, and the cost of the
            // approximation is a missed pragma inside an interpolation, never a
            // false one.
            ++i;
            int depth = 0;
            while (i < n) {
                if (src[i] == '\\') {
                    i += 2;
                    continue;
                }
                if (src[i] == '$' && i + 1 < n && src[i + 1] == '{') {
                    ++depth;
                    i += 2;
                    continue;
                }
                if (depth > 0 && src[i] == '}') {
                    --depth;
                    ++i;
                    continue;
                }
                if (depth == 0 && src[i] == '`') {
                    ++i;
                    break;
                }
                ++i;
            }
            value_before = true;
            continue;
        }
        if (c == '/' && i + 1 < n && src[i + 1] == '/') {
            const std::size_t start = i + 2;
            std::size_t end = start;
            while (end < n && src[end] != '\n') {
                ++end;
            }
            std::string error;
            if (!jsx_pragma_impl::apply_comment(src.substr(start, end - start), opts, error)) {
                return {false, std::move(error)};
            }
            i = end;
            continue;
        }
        if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            const std::size_t start = i + 2;
            std::size_t end = start;
            while (end + 1 < n && !(src[end] == '*' && src[end + 1] == '/')) {
                ++end;
            }
            std::string error;
            if (!jsx_pragma_impl::apply_comment(src.substr(start, end - start), opts, error)) {
                return {false, std::move(error)};
            }
            i = (end + 1 < n) ? end + 2 : n;
            value_before = false;
            continue;
        }
        if (c == '/' && !value_before) {
            // Regex literal — skip its body so `/[/*]/` cannot open a comment.
            ++i;
            bool in_class = false;
            while (i < n) {
                if (src[i] == '\\') {
                    i += 2;
                    continue;
                }
                if (src[i] == '\n') {
                    break;  // unterminated; not a regex after all
                }
                if (src[i] == '[') {
                    in_class = true;
                } else if (src[i] == ']') {
                    in_class = false;
                } else if (src[i] == '/' && !in_class) {
                    ++i;
                    break;
                }
                ++i;
            }
            while (i < n && jsx_pragma_impl::is_id_char(src[i])) {
                ++i;  // flags
            }
            value_before = true;
            continue;
        }
        if (jsx_pragma_impl::is_id_char(c)) {
            while (i < n && jsx_pragma_impl::is_id_char(src[i])) {
                ++i;
            }
            value_before = true;
            continue;
        }
        if (c == ')' || c == ']' || c == '}') {
            value_before = true;
        } else if (!jsx_pragma_impl::is_ws(c)) {
            value_before = false;
        }
        ++i;
    }
    return {};
}

}  // namespace mbun::js_parser::detail
