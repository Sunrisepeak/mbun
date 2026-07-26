// Function.prototype.toString printer normalization.
//
// bun always runs sources through parse->print, so fn.toString() returns
// *printed* text: 2-space indentation, double-quoted strings, semicolons
// after simple statements (blueprint: bun js_printer.zig — quote preference
// is double quotes, indent is two spaces per nesting depth). mbun's runtime
// loader uses the erasure transpiler which preserves source bytes, so
// toString() leaked raw tabs / single quotes and broke printed-source
// consumers (e.g. elysia's sucrose static analyzer compares fn.toString()
// against printer-formatted expectations).
//
// This partition normalizes toString() output toward printer form with a
// small string-/template-/comment-aware lexer (NOT naive regex):
//   - leading tabs -> 2 spaces each (tab-indented sources encode nesting
//     depth as tab count; printer emits 2 spaces per depth)
//   - single-quoted string literals -> double-quoted, re-escaped
//   - `;` appended to one-line `return`/`throw` statements
//   - read-once `const X = IDENT` binding aliases collapsed (single use
//     substituted with IDENT, declarator dropped; zero-use declarators
//     kept) — bun's minify_syntax read-once propagation, observable in
//     printed toString() text (verified against bun 1.4.0: elysia aot
//     "isContextPassToUnknown" handlers print `handle(context)` for
//     source `const c = context; ... handle(c)`)
//   - blank code lines dropped (printer never emits them)
// Content inside templates, comments and double-quoted strings is copied
// verbatim; lines touched by multi-line constructs are marked tainted and
// excluded from the alias pass. Functions whose source contains neither
// tabs nor single quotes pass through untouched (fast path), as does any
// `[native code]` source.
//
// NOTE: appended AFTER the master builtins IIFE, so this is a self-contained
// IIFE that re-binds G = globalThis. Top level must never throw. Loading
// last also means every earlier partition captured the *native* toString,
// which this override delegates to for raw text.
export module mbun.jsc.js_builtins:fn_tostring_printer;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kFnToStringPrinterJS = R"JS(
(function () {
  "use strict";
  try {
    const FP = Function.prototype;
    const origToString = FP.toString;

    // Append ';' to a completed one-line `return <expr>` / `throw <expr>`
    // statement the way the printer would. `line` is already normalized and
    // never starts inside a multi-line template (caller guarantees the line
    // boundary is real code). Skips lines that end in a comment.
    function needsSemi(line) {
      if (!/^\s*(?:return|throw)\b/.test(line)) return false;
      const last = line[line.length - 1];
      if (!last || !/[\w$"')\]}]/.test(last)) return false;
      if (/\*\/\s*$/.test(line)) return false;
      // scan for a trailing // comment, skipping string contents
      for (let i = 0; i < line.length; i++) {
        const c = line[i];
        if (c === '"') {
          for (i++; i < line.length; i++) {
            if (line[i] === "\\") { i++; continue; }
            if (line[i] === '"') break;
          }
          continue;
        }
        if (c === "/" && line[i + 1] === "/") return false;
      }
      return true;
    }

    // Blank out single-line string literal contents (keeps indices) so the
    // alias pass never counts/substitutes identifiers inside strings.
    function maskLine(line) {
      let m = "";
      for (let i = 0; i < line.length; i++) {
        const c = line[i];
        if (c === '"' || c === "`") {
          m += c;
          for (i++; i < line.length; i++) {
            if (line[i] === "\\") { m += "\\_"; i++; continue; }
            if (line[i] === c) { m += c; break; }
            m += "_";
          }
          continue;
        }
        if (c === "/" && line[i + 1] === "/") {
          m += "_".repeat(line.length - i);
          break;
        }
        m += c;
      }
      return m;
    }

    const kKeywordInit = /^(?:this|arguments|true|false|null|undefined|new|typeof|void|await|yield|super)$/;

    // bun minify_syntax read-once propagation: collapse `const X = IDENT`
    // when X is read exactly once and never written. Zero-use declarators
    // are kept (bun keeps them in printed output). `tainted` marks output
    // lines containing template/block-comment content — any candidate that
    // touches a tainted line is skipped.
    function inlineReadOnceAliases(lines, taintArr) {
      for (let i = 0; i < lines.length; i++) {
        if (taintArr[i]) continue;
        const head = /^(\s*)(const|let|var)\s+(.*)$/.exec(lines[i]);
        if (!head) continue;
        // gather a multi-line declarator list (lines ending with ",")
        let last = i;
        let decl = head[3];
        while (/,\s*$/.test(decl) && last + 1 < lines.length && !taintArr[last + 1]) {
          last++;
          decl += " " + lines[last].trim();
        }
        let hadSemi = false;
        if (/;\s*$/.test(decl)) { hadSemi = true; decl = decl.replace(/;\s*$/, ""); }
        const pieces = decl.split(",");
        const parsed = [];
        let ok = pieces.length > 0;
        for (const piece of pieces) {
          const m = /^\s*([A-Za-z_$][\w$]*)\s*=\s*([A-Za-z_$][\w$]*)\s*$/.exec(piece);
          if (!m || kKeywordInit.test(m[2])) { ok = false; break; }
          parsed.push([m[1], m[2]]);
        }
        if (!ok) { i = last; continue; }
        const kept = [];
        for (const [name, init] of parsed) {
          const word = new RegExp("\\b" + name + "\\b", "g"); // used with match/search only (stateless)
          const writes = new RegExp("\\b" + name + "\\b\\s*(?:=[^=]|\\+\\+|--|[-+*/%&|^]=|<<=|>>=)");
          const preWrites = new RegExp("(?:\\+\\+|--)\\s*\\b" + name + "\\b");
          let uses = 0;
          let useLine = -1;
          let bad = false;
          for (let j = 0; j < lines.length; j++) {
            if (j >= i && j <= last) continue; // the declaration itself
            if (lines[j].indexOf(name) < 0) continue;
            if (taintArr[j]) {
              // possible hidden use inside template/comment content
              if (lines[j].match(word)) { bad = true; break; }
              continue;
            }
            const masked = maskLine(lines[j]);
            const hits = masked.match(word);
            if (!hits) continue;
            if (writes.test(masked) || preWrites.test(masked)) { bad = true; break; }
            uses += hits.length;
            useLine = j;
            if (uses > 1) break;
          }
          if (!bad && uses === 1) {
            // substitute the single (string-masked) occurrence with init
            const at = maskLine(lines[useLine]).search(word);
            lines[useLine] = lines[useLine].slice(0, at) + init +
                lines[useLine].slice(at + name.length);
            continue; // declarator dropped
          }
          kept.push(name + " = " + init);
        }
        if (kept.length === parsed.length) { i = last; continue; }
        if (kept.length === 0) {
          lines.splice(i, last - i + 1);
          taintArr.splice(i, last - i + 1);
          i--;
          continue;
        }
        lines.splice(i, last - i + 1,
            head[1] + head[2] + " " + kept.join(", ") + (hadSemi ? ";" : ""));
        taintArr.splice(i, last - i + 1, false);
      }
      return lines;
    }

    function normalize(src) {
      // fast path: nothing the printer would have changed that we model
      if (src.indexOf("\t") < 0 && src.indexOf("'") < 0) return src;
      const n = src.length;
      let out = "";
      let i = 0;
      let atLineStart = false; // toString text begins mid-line (at `function`/`(`)
      let lineNo = 0;
      const tainted = new Set();
      const copyChunk = (chunk) => {
        // verbatim template/comment content: taint every line it touches
        tainted.add(lineNo);
        for (let k = 0; k < chunk.length; k++) {
          if (chunk[k] === "\n") { lineNo++; tainted.add(lineNo); }
        }
        out += chunk;
      };
      const closeLine = () => {
        const ls = out.lastIndexOf("\n") + 1;
        if (needsSemi(out.slice(ls))) out += ";";
      };
      while (i < n) {
        const c = src[i];
        if (atLineStart) {
          atLineStart = false;
          let j = i;
          while (j < n && src[j] === "\t") j++;
          if (j > i) {
            out += "  ".repeat(j - i); // 1 tab of depth -> printer's 2 spaces
            i = j;
            continue;
          }
        }
        if (c === "\n") {
          closeLine();
          out += c;
          lineNo++;
          atLineStart = true;
          i++;
          continue;
        }
        if (c === "'") {
          // re-print as a double-quoted literal
          let j = i + 1;
          let body = "";
          let ok = false;
          while (j < n) {
            const d = src[j];
            if (d === "\\") {
              const e = src[j + 1];
              if (e === "'") { body += "'"; j += 2; continue; } // \' -> '
              body += d + (e === undefined ? "" : e);
              j += 2;
              continue;
            }
            if (d === "'") { ok = true; j++; break; }
            if (d === "\n") break; // not a normal string — bail out below
            if (d === '"') { body += '\\"'; j++; continue; }
            body += d;
            j++;
          }
          if (ok) { out += '"' + body + '"'; i = j; continue; }
          out += c;
          i++;
          continue;
        }
        if (c === '"') {
          // copy double-quoted literal verbatim
          let j = i + 1;
          while (j < n) {
            if (src[j] === "\\") { j += 2; continue; }
            if (src[j] === '"') { j++; break; }
            if (src[j] === "\n") break;
            j++;
          }
          out += src.slice(i, j);
          i = j;
          continue;
        }
        if (c === "`") {
          // copy template literal verbatim, including ${...} and newlines
          let j = i + 1;
          let depth = 0;
          while (j < n) {
            const d = src[j];
            if (d === "\\") { j += 2; continue; }
            if (d === "`" && depth === 0) { j++; break; }
            if (d === "$" && src[j + 1] === "{") { depth++; j += 2; continue; }
            if (d === "}" && depth > 0) { depth--; j++; continue; }
            j++;
          }
          copyChunk(src.slice(i, j));
          i = j;
          continue;
        }
        if (c === "/" && src[i + 1] === "/") {
          let j = src.indexOf("\n", i);
          if (j < 0) j = n;
          out += src.slice(i, j);
          i = j;
          continue;
        }
        if (c === "/" && src[i + 1] === "*") {
          let j = src.indexOf("*/", i + 2);
          j = j < 0 ? n : j + 2;
          copyChunk(src.slice(i, j));
          i = j;
          continue;
        }
        out += c;
        i++;
      }
      closeLine();
      // post-passes: alias collapse, then drop blank code lines (the
      // printer emits neither; first line is the signature, never blank)
      const lines = out.split("\n");
      const taintArr = lines.map((_, idx) => tainted.has(idx));
      inlineReadOnceAliases(lines, taintArr);
      const compact = [lines[0]];
      for (let k = 1; k < lines.length; k++) {
        if (/^\s*$/.test(lines[k]) && !taintArr[k]) continue;
        compact.push(lines[k]);
      }
      return compact.join("\n");
    }

    const fpToString = function toString() {
      if (this === fpToString) {
        return "function toString() {\n    [native code]\n}";
      }
      const raw = origToString.call(this); // TypeError on non-callables, per spec
      if (typeof raw !== "string" || raw.indexOf("[native code]") >= 0) return raw;
      try {
        return normalize(raw);
      } catch (_) {
        return raw;
      }
    };

    Object.defineProperty(FP, "toString", {
      value: fpToString,
      writable: true,
      enumerable: false,
      configurable: true,
    });
  } catch (_) {}

  // Last statement of the builtin image: every partition has now loaded (several
  // of them reach for node:zlib while booting), so node:zlib may start
  // snapshotting buffer.kMaxLength the way node's require() does.
  try { if (typeof globalThis.__mbunZlibArmKMax === "function") globalThis.__mbunZlibArmKMax(); } catch (_) {}
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
