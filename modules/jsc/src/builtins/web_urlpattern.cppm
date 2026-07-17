// URLPattern partition: full URLPattern spec implementation (tokenizer,
// pattern-string parser, constructor-string parser, component compiler,
// pattern-string generator, init processing with baseURL inheritance, and
// exec/test) replacing the small subset registered by bootstrap.
//
// Canonicalization leans on the engine's URL parser (WHATWG URL) via setter /
// re-parse tricks, exactly like the reference urlpattern-polyfill does.
//
// NOTE: appended AFTER the master builtins IIFE; self-contained IIFE that
// re-binds G = globalThis. Top level must never throw.
//
// Blueprint: https://urlpattern.spec.whatwg.org/ (as implemented by bun/WebKit
// and the urlpattern-polyfill), acceptance pinned by the WPT suite vendored at
// compat/bun/test/js/web/urlpattern/urlpatterntestdata.json.
export module mbun.jsc.js_builtins:web_urlpattern;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kWebURLPatternJS = R"JS(
(function () {
  const G = globalThis;
  try {
    if (typeof G.URL !== "function") return;

    const SPECIAL_SCHEMES = { ftp: "21", file: "", http: "80", https: "443", ws: "80", wss: "443" };

    // --------------------------------------------------------- canonicalize
    const canonProtocol = (v) => {
      if (v === "") return v;
      return new URL(v + "://dummy.test").protocol.slice(0, -1);
    };
    const canonUsername = (v) => {
      if (v === "") return v;
      const u = new URL("https://dummy.test");
      u.username = v;
      return u.username;
    };
    const canonPassword = (v) => {
      if (v === "") return v;
      const u = new URL("https://dummy.test");
      u.password = v;
      return u.password;
    };
    // Genuinely-invalid host code points per WHATWG host parsing. NOT included:
    //  - tab/newline/CR: stripped below (host canonicalizes with them removed,
    //    e.g. "bad\thostname" -> "badhostname")
    //  - '#' and '/': the URL parser terminates the host there
    //    (e.g. "bad#hostname"/"bad/hostname" -> "bad")
    // '%' is handled separately: allowed only as a valid percent-escape.
    const HOST_FORBIDDEN = /[\0 :<>?@[\]^|]/;
    const canonHostname = (v) => {
      if (v === "") return v;
      // WHATWG URL parsing removes ASCII tab/newline/CR before host parsing;
      // mbun's URL parser does not strip them, so do it here to match.
      v = v.replace(/[\t\n\r]/g, "");
      if (v === "") return v;
      if (v[0] === "[") return canonIPv6Hostname(v);
      // A bare '%' (not part of a %XX escape) is invalid.
      if (v.replace(/%[0-9a-fA-F]{2}/g, "").indexOf("%") !== -1)
        throw new TypeError("Invalid hostname '" + v + "'");
      if (HOST_FORBIDDEN.test(v)) throw new TypeError("Invalid hostname '" + v + "'");
      const u = new URL("https://" + v + "/");
      return u.hostname;
    };
    const canonIPv6Hostname = (v) => {
      let out = "";
      for (const c of v) {
        if (!/[0-9a-fA-F[\]:]/.test(c)) throw new TypeError("Invalid IPv6 hostname '" + v + "'");
        out += c.toLowerCase();
      }
      return out;
    };
    const canonPort = (v, protocol) => {
      if (v === "") return v;
      if (!/^[0-9]+$/.test(v)) throw new TypeError("Invalid port '" + v + "'");
      const n = Number(v);
      if (n > 65535) throw new TypeError("Invalid port '" + v + "'");
      if (protocol && SPECIAL_SCHEMES[protocol] === String(n)) return "";
      return String(n);
    };
    const canonPathname = (v) => {
      if (v === "") return v;
      const leading = v[0] === "/";
      const u = new URL((leading ? "" : "/-") + v, "https://dummy.test");
      let res = u.pathname;
      if (!leading) res = res.slice(2);
      return res;
    };
    const canonOpaquePathname = (v) => {
      if (v === "") return v;
      const u = new URL("fake:" + v);
      return u.pathname;
    };
    const canonSearch = (v) => {
      if (v === "") return v;
      const u = new URL("https://dummy.test");
      u.search = v;
      return u.search[0] === "?" ? u.search.slice(1) : u.search;
    };
    const canonHash = (v) => {
      if (v === "") return v;
      const u = new URL("https://dummy.test");
      u.hash = v;
      return u.hash[0] === "#" ? u.hash.slice(1) : u.hash;
    };

    // ------------------------------------------------------------- tokenizer
    // A pattern name uses JS-identifier code points, which per spec are the
    // Unicode ID_Start / ID_Continue sets. Approximate by allowing the ASCII
    // identifier set plus any non-ASCII (>= 0x80) code point — covering the
    // Unicode letters/marks the WPT suite exercises (é, ℘, 㐀, astral 𐑐, …).
    const NAME_CP = /[$_0-9A-Za-z\u0080-\uffff]/;
    const DIGIT_START = /^[0-9]/;
    const NAME_START_CP = /[$_A-Za-z\u0080-\uffff]/;
    function tokenize(input, policy) {
      const tokens = [];
      let i = 0;
      const n = input.length;
      const fail = (from, to, msg) => {
        if (policy === "strict") throw new TypeError(msg + " in pattern '" + input + "'");
        tokens.push({ type: "invalid-char", index: from, value: input.slice(from, to) });
        i = to;
      };
      while (i < n) {
        const c = input[i];
        if (c === "*") { tokens.push({ type: "asterisk", index: i, value: c }); i++; continue; }
        if (c === "+" || c === "?") { tokens.push({ type: "other-modifier", index: i, value: c }); i++; continue; }
        if (c === "\\") {
          if (i === n - 1) { fail(i, n, "Trailing escape"); continue; }
          tokens.push({ type: "escaped-char", index: i, value: input[i + 1] });
          i += 2;
          continue;
        }
        if (c === "{") { tokens.push({ type: "open", index: i, value: c }); i++; continue; }
        if (c === "}") { tokens.push({ type: "close", index: i, value: c }); i++; continue; }
        if (c === ":") {
          let j = i + 1, name = "";
          while (j < n && (name === "" ? NAME_START_CP : NAME_CP).test(input[j])) { name += input[j]; j++; }
          if (name === "") { fail(i, i + 1, "Missing pattern name"); continue; }
          tokens.push({ type: "name", index: i, value: name });
          i = j;
          continue;
        }
        if (c === "(") {
          let depth = 1, j = i + 1, value = "", error = false;
          if (j < n && input[j] === "?") error = true;
          while (j < n && !error) {
            const rc = input[j];
            if (rc.charCodeAt(0) > 0x7e || rc.charCodeAt(0) < 0x20) { error = true; break; }
            if (rc === "\\") {
              if (j === n - 1) { error = true; break; }
              value += rc + input[j + 1];
              j += 2;
              continue;
            }
            if (rc === ")") {
              depth--;
              if (depth === 0) { j++; break; }
            } else if (rc === "(") {
              depth++;
              if (j === n - 1) { error = true; break; }
              if (input[j + 1] !== "?") { error = true; break; }
            }
            value += rc;
            j++;
          }
          if (depth !== 0 || value === "" || error) { fail(i, i + 1, "Invalid regexp group"); continue; }
          tokens.push({ type: "regexp", index: i, value });
          i = j;
          continue;
        }
        tokens.push({ type: "char", index: i, value: c });
        i++;
      }
      tokens.push({ type: "end", index: i, value: "" });
      return tokens;
    }

    // --------------------------------------------------------- parse pattern
    // NOTE: char-class regexes that contain a literal '/' are built via
    // RegExp() from a string — an inline /.../ literal with an unescaped '/'
    // inside the class trips mbun's regex lexer (it ends the literal early).
    const REGEXP_ESCAPE = new RegExp("[.+*?^${}()[\\]|/\\\\]", "g");
    const escapeRegexp = (s) => s.replace(REGEXP_ESCAPE, "\\$&");
    const PATTERN_ESCAPE = /[+*?:{}()\\]/g;
    const escapePattern = (s) => s.replace(PATTERN_ESCAPE, "\\$&");
    const segmentWildcard = (options) => "[^" + escapeRegexp(options.delimiter || "") + "]+?";
    const MOD_STR = { "none": "", "optional": "?", "zero-or-more": "*", "one-or-more": "+" };

    function parsePatternString(input, options, encode) {
      const tokens = tokenize(input, "strict");
      const parts = [];
      const segWildcardRe = segmentWildcard(options);
      let pendingFixed = "", index = 0, nextNumericName = 0;
      const tryConsume = (type) => (tokens[index].type === type ? tokens[index++] : null);
      const tryConsumeModifier = () => tryConsume("other-modifier") || tryConsume("asterisk");
      const mustConsume = (type) => {
        const t = tryConsume(type);
        if (!t) throw new TypeError("Expected " + type + " at index " + tokens[index].index + " in pattern '" + input + "'");
        return t;
      };
      const consumeText = () => {
        let r = "";
        for (;;) {
          const t = tryConsume("char") || tryConsume("escaped-char");
          if (!t) break;
          r += t.value;
        }
        return r;
      };
      const maybeAddFixed = () => {
        if (pendingFixed === "") return;
        parts.push({ type: "fixed-text", name: "", prefix: "", value: encode(pendingFixed), suffix: "", modifier: "none" });
        pendingFixed = "";
      };
      const addPart = (prefix, nameToken, regexpOrWildcardToken, suffix, modifierToken) => {
        let modifier = "none";
        if (modifierToken) {
          modifier = modifierToken.value === "?" ? "optional" : modifierToken.value === "*" ? "zero-or-more" : "one-or-more";
        }
        if (!nameToken && !regexpOrWildcardToken && modifier === "none") {
          pendingFixed += prefix;
          return;
        }
        maybeAddFixed();
        if (!nameToken && !regexpOrWildcardToken) {
          // A `{ text }<modifier>` group with only fixed text: emit a
          // fixed-text part that carries the modifier (do not merge into
          // pendingFixed, which would drop the modifier).
          if (prefix === "") return;
          parts.push({ type: "fixed-text", name: "", prefix: "", value: encode(prefix), suffix: "", modifier });
          return;
        }
        let regexpValue;
        if (!regexpOrWildcardToken) regexpValue = segWildcardRe;
        else if (regexpOrWildcardToken.type === "asterisk") regexpValue = ".*";
        else regexpValue = regexpOrWildcardToken.value;
        let type = "regexp";
        if (regexpValue === segWildcardRe) { type = "segment-wildcard"; regexpValue = ""; }
        else if (regexpValue === ".*") { type = "full-wildcard"; regexpValue = ""; }
        let name = "";
        if (nameToken) name = nameToken.value;
        else if (regexpOrWildcardToken) name = String(nextNumericName++);
        if (parts.some((p) => p.name === name && name !== "")) throw new TypeError("Duplicate group name '" + name + "'");
        parts.push({ type, name, prefix: encode(prefix), value: regexpValue, suffix: encode(suffix), modifier });
      };
      while (index < tokens.length) {
        const charToken = tryConsume("char");
        const nameToken = tryConsume("name");
        let regexpOrWildcardToken = tryConsume("regexp");
        if (!nameToken && !regexpOrWildcardToken) regexpOrWildcardToken = tryConsume("asterisk");
        if (nameToken || regexpOrWildcardToken) {
          let prefix = charToken ? charToken.value : "";
          if (options.prefix !== undefined && options.prefix !== "" && prefix !== options.prefix) {
            pendingFixed += prefix;
            prefix = "";
          } else if (options.prefix === undefined || options.prefix === "") {
            if (prefix !== "") { pendingFixed += prefix; prefix = ""; }
          }
          maybeAddFixed();
          const modifierToken = tryConsumeModifier();
          addPart(prefix, nameToken, regexpOrWildcardToken, "", modifierToken);
          continue;
        }
        const fixedToken = charToken || tryConsume("escaped-char");
        if (fixedToken) { pendingFixed += fixedToken.value; continue; }
        const openToken = tryConsume("open");
        if (openToken) {
          const prefix = consumeText();
          const nameToken2 = tryConsume("name");
          let regexpOrWildcardToken2 = tryConsume("regexp");
          if (!nameToken2 && !regexpOrWildcardToken2) regexpOrWildcardToken2 = tryConsume("asterisk");
          const suffix = consumeText();
          mustConsume("close");
          const modifierToken2 = tryConsumeModifier();
          addPart(prefix, nameToken2, regexpOrWildcardToken2, suffix, modifierToken2);
          continue;
        }
        maybeAddFixed();
        mustConsume("end");
        break;
      }
      return parts;
    }

    // ------------------------------------------------------ regexp generation
    function generateRegexpAndNames(parts, options) {
      let result = "^";
      const names = [];
      const segWildcardRe = segmentWildcard(options);
      for (const part of parts) {
        if (part.type === "fixed-text") {
          if (part.modifier === "none") result += escapeRegexp(part.value);
          else result += "(?:" + escapeRegexp(part.value) + ")" + MOD_STR[part.modifier];
          continue;
        }
        names.push(part.name);
        const regexpValue = part.type === "segment-wildcard" ? segWildcardRe : part.type === "full-wildcard" ? ".*" : part.value;
        if (part.prefix === "" && part.suffix === "") {
          if (part.modifier === "none" || part.modifier === "optional")
            result += "(" + regexpValue + ")" + MOD_STR[part.modifier];
          else
            result += "((?:" + regexpValue + ")" + MOD_STR[part.modifier] + ")";
          continue;
        }
        if (part.modifier === "none" || part.modifier === "optional") {
          result += "(?:" + escapeRegexp(part.prefix) + "(" + regexpValue + ")" + escapeRegexp(part.suffix) + ")" + MOD_STR[part.modifier];
          continue;
        }
        result += "(?:" + escapeRegexp(part.prefix) +
          "((?:" + regexpValue + ")(?:" + escapeRegexp(part.suffix) + escapeRegexp(part.prefix) + "(?:" + regexpValue + "))*)" +
          escapeRegexp(part.suffix) + ")";
        if (part.modifier === "zero-or-more") result += "?";
      }
      result += "$";
      return [result, names];
    }

    // ------------------------------------------------ pattern string generation
    function generatePatternString(parts, options) {
      let result = "";
      for (let i = 0; i < parts.length; i++) {
        const part = parts[i];
        const prev = i > 0 ? parts[i - 1] : null;
        const next = i + 1 < parts.length ? parts[i + 1] : null;
        if (part.type === "fixed-text") {
          if (part.modifier === "none") { result += escapePattern(part.value); continue; }
          result += "{" + escapePattern(part.value) + "}" + MOD_STR[part.modifier];
          continue;
        }
        const customName = part.name !== "" && !DIGIT_START.test(part.name);
        let needsGrouping = part.suffix !== "" || (part.prefix !== "" && part.prefix !== (options.prefix || ""));
        if (!needsGrouping && customName && part.type === "segment-wildcard" && part.modifier === "none" &&
            next && next.prefix === "" && next.suffix === "") {
          if (next.type === "fixed-text") {
            if (next.value !== "" && NAME_CP.test(next.value[0])) needsGrouping = true;
          } else {
            if (DIGIT_START.test(next.name)) needsGrouping = true;
          }
        }
        if (!needsGrouping && part.prefix === "" && prev && prev.type === "fixed-text" &&
            prev.value !== "" && prev.value[prev.value.length - 1] === (options.prefix || " ")) {
          needsGrouping = true;
        }
        if (needsGrouping) result += "{";
        result += escapePattern(part.prefix);
        if (customName) result += ":" + part.name;
        if (part.type === "regexp") result += "(" + part.value + ")";
        else if (part.type === "segment-wildcard" && !customName) result += "(" + segmentWildcard(options) + ")";
        else if (part.type === "full-wildcard") {
          if (!customName && (!prev || prev.type === "fixed-text" || prev.modifier !== "none" || needsGrouping || part.prefix !== "")) {
            result += "*";
          } else {
            result += "(.*)";
          }
        }
        if (part.type === "segment-wildcard" && customName && part.suffix !== "" && NAME_CP.test(part.suffix[0])) {
          result += "\\";
        }
        result += escapePattern(part.suffix);
        if (needsGrouping) result += "}";
        result += MOD_STR[part.modifier];
      }
      return result;
    }

    // ------------------------------------------------------ component compile
    const DEFAULT_OPTIONS = { delimiter: "", prefix: "" };
    const HOSTNAME_OPTIONS = { delimiter: ".", prefix: "" };
    const PATHNAME_OPTIONS = { delimiter: "/", prefix: "/" };
    function compileComponent(input, encode, options, flags) {
      if (input === undefined) input = "*";
      const parts = parsePatternString("" + input, options, encode);
      const [regexpString, names] = generateRegexpAndNames(parts, options);
      let regexp;
      try { regexp = new RegExp(regexpString, flags); }
      catch (e) { throw new TypeError("Invalid regexp generated from pattern: " + e.message); }
      return {
        patternString: generatePatternString(parts, options),
        regexp,
        names,
        hasRegexpGroups: parts.some((p) => p.type === "regexp"),
      };
    }
    const protocolMatchesSpecialScheme = (protocolComponent) => {
      for (const scheme of Object.keys(SPECIAL_SCHEMES)) {
        if (protocolComponent.regexp.test(scheme)) return true;
      }
      return false;
    };

    // ------------------------------------------------ constructor string parse
    function parseConstructorString(input) {
      const p = {
        input,
        tokens: tokenize(input, "lenient"),
        result: {},
        componentStart: 0,
        tokenIndex: 0,
        tokenIncrement: 1,
        groupDepth: 0,
        ipv6Depth: 0,
        state: "init",
        protocolMatchesSpecial: false,
      };
      const safeToken = (i) => (i < p.tokens.length ? p.tokens[i] : p.tokens[p.tokens.length - 1]);
      const isNonSpecial = (i, v) => {
        const t = safeToken(i);
        return t.value === v && (t.type === "char" || t.type === "escaped-char" || t.type === "invalid-char");
      };
      const isProtocolSuffix = () => isNonSpecial(p.tokenIndex, ":");
      const nextIsAuthoritySlashes = () => isNonSpecial(p.tokenIndex + 1, "/") && isNonSpecial(p.tokenIndex + 2, "/");
      const isIdentityTerminator = () => isNonSpecial(p.tokenIndex, "@");
      const isPasswordPrefix = () => isNonSpecial(p.tokenIndex, ":");
      const isPortPrefix = () => isNonSpecial(p.tokenIndex, ":");
      const isPathnameStart = () => isNonSpecial(p.tokenIndex, "/");
      const isSearchPrefix = () => {
        if (isNonSpecial(p.tokenIndex, "?")) return true;
        if (p.tokens[p.tokenIndex].value !== "?") return false;
        const prev = p.tokenIndex > 0 ? safeToken(p.tokenIndex - 1) : null;
        return !prev || (prev.type !== "name" && prev.type !== "regexp" && prev.type !== "close" && prev.type !== "asterisk");
      };
      const isHashPrefix = () => isNonSpecial(p.tokenIndex, "#");
      const isGroupOpen = () => p.tokens[p.tokenIndex].type === "open";
      const isGroupClose = () => p.tokens[p.tokenIndex].type === "close";
      const isIPv6Open = () => isNonSpecial(p.tokenIndex, "[");
      const isIPv6Close = () => isNonSpecial(p.tokenIndex, "]");
      const makeComponentString = () => {
        const token = p.tokens[p.tokenIndex];
        const start = safeToken(p.componentStart).index;
        return p.input.substring(start, token.index);
      };
      const ORDER = ["init", "protocol", "authority", "username", "password", "hostname", "port", "pathname", "search", "hash", "done"];
      const stateLE = (a, b) => ORDER.indexOf(a) <= ORDER.indexOf(b);
      const changeState = (newState, skip) => {
        switch (p.state) {
          case "init": break;
          case "protocol": p.result.protocol = makeComponentString(); break;
          case "authority": break;
          case "username": p.result.username = makeComponentString(); break;
          case "password": p.result.password = makeComponentString(); break;
          case "hostname": p.result.hostname = makeComponentString(); break;
          case "port": p.result.port = makeComponentString(); break;
          case "pathname": p.result.pathname = makeComponentString(); break;
          case "search": p.result.search = makeComponentString(); break;
          case "hash": p.result.hash = makeComponentString(); break;
        }
        if (p.state !== "init" && newState !== "done") {
          if (stateLE(p.state, "password") && stateLE("port", newState) && stateLE(newState, "hash") && p.result.hostname === undefined)
            p.result.hostname = "";
          if (stateLE(p.state, "hostname") && stateLE("pathname", newState) && stateLE(newState, "hash") && p.result.port === undefined)
            p.result.port = "";
          if (stateLE(p.state, "port") && stateLE("search", newState) && stateLE(newState, "hash") && p.result.pathname === undefined)
            p.result.pathname = p.protocolMatchesSpecial ? "/" : "";
          if (stateLE(p.state, "pathname") && newState === "hash" && p.result.search === undefined)
            p.result.search = "";
        }
        p.state = newState;
        p.tokenIndex += skip;
        p.componentStart = p.tokenIndex;
        p.tokenIncrement = 0;
      };
      const rewind = () => { p.tokenIndex = p.componentStart; p.tokenIncrement = 0; };
      const rewindAndSetState = (s) => { rewind(); p.state = s; };
      const computeSpecialSchemeFlag = () => {
        const comp = compileComponent(makeComponentString(), canonProtocol, DEFAULT_OPTIONS, "");
        p.protocolMatchesSpecial = protocolMatchesSpecialScheme(comp);
      };

      while (p.tokenIndex < p.tokens.length) {
        p.tokenIncrement = 1;
        if (p.tokens[p.tokenIndex].type === "end") {
          if (p.state === "init") {
            rewind();
            if (isHashPrefix()) changeState("hash", 1);
            else if (isSearchPrefix()) { changeState("search", 1); p.result.hash = ""; }
            else { changeState("pathname", 0); p.result.search = ""; p.result.hash = ""; }
            p.tokenIndex += p.tokenIncrement;
            continue;
          }
          if (p.state === "authority") {
            rewindAndSetState("hostname");
            p.tokenIndex += p.tokenIncrement;
            continue;
          }
          changeState("done", 0);
          break;
        }
        if (isGroupOpen()) { p.groupDepth++; p.tokenIndex += p.tokenIncrement; continue; }
        if (p.groupDepth > 0) {
          if (isGroupClose()) p.groupDepth--;
          else { p.tokenIndex += p.tokenIncrement; continue; }
        }
        switch (p.state) {
          case "init":
            if (isProtocolSuffix()) rewindAndSetState("protocol");
            break;
          case "protocol":
            if (isProtocolSuffix()) {
              computeSpecialSchemeFlag();
              let nextState = "pathname", skip = 1;
              if (nextIsAuthoritySlashes()) { nextState = "authority"; skip = 3; }
              else if (p.protocolMatchesSpecial) nextState = "authority";
              changeState(nextState, skip);
            }
            break;
          case "authority":
            if (isIdentityTerminator()) rewindAndSetState("username");
            else if (isPathnameStart() || isSearchPrefix() || isHashPrefix()) rewindAndSetState("hostname");
            break;
          case "username":
            if (isPasswordPrefix()) changeState("password", 1);
            else if (isIdentityTerminator()) changeState("hostname", 1);
            break;
          case "password":
            if (isIdentityTerminator()) changeState("hostname", 1);
            break;
          case "hostname":
            if (isIPv6Open()) p.ipv6Depth++;
            else if (isIPv6Close()) p.ipv6Depth--;
            else if (isPortPrefix() && p.ipv6Depth === 0) changeState("port", 1);
            else if (isPathnameStart()) changeState("pathname", 0);
            else if (isSearchPrefix()) changeState("search", 1);
            else if (isHashPrefix()) changeState("hash", 1);
            break;
          case "port":
            if (isPathnameStart()) changeState("pathname", 0);
            else if (isSearchPrefix()) changeState("search", 1);
            else if (isHashPrefix()) changeState("hash", 1);
            break;
          case "pathname":
            if (isSearchPrefix()) changeState("search", 1);
            else if (isHashPrefix()) changeState("hash", 1);
            break;
          case "search":
            if (isHashPrefix()) changeState("hash", 1);
            break;
          case "hash":
            break;
          case "done":
            break;
        }
        p.tokenIndex += p.tokenIncrement;
      }
      if (p.result.hostname !== undefined && p.result.port === undefined) p.result.port = "";
      return p.result;
    }

    // --------------------------------------------------------- init processing
    const isAbsolutePathname = (value, type) => {
      if (value === "") return false;
      if (value[0] === "/") return true;
      if (type === "url") return false;
      if (value.length < 2) return false;
      if (value[0] === "\\" && value[1] === "/") return true;
      if (value[0] === "{" && value[1] === "/") return true;
      return false;
    };
    function processInit(init, type, defaults) {
      if (init === null || typeof init !== "object") throw new TypeError("URLPattern init must be an object");
      const result = Object.assign({}, defaults);
      let baseURL = null;
      if (init.baseURL !== undefined) {
        baseURL = new URL(init.baseURL); // throws TypeError on invalid
        const esc = (s) => (type === "pattern" ? escapePattern(s) : s);
        if (init.protocol === undefined) result.protocol = esc(baseURL.protocol.slice(0, -1));
        // NOTE: username/password are NOT inherited from baseURL into the
        // pattern (bun/WPT contract) — they stay at the "*" default unless the
        // init supplies them explicitly.
        if (init.protocol === undefined && init.hostname === undefined)
          result.hostname = esc(baseURL.hostname);
        if (init.protocol === undefined && init.hostname === undefined && init.port === undefined)
          result.port = esc(baseURL.port);
        if (init.protocol === undefined && init.hostname === undefined && init.port === undefined && init.pathname === undefined)
          result.pathname = esc(baseURL.pathname);
        if (init.protocol === undefined && init.hostname === undefined && init.port === undefined && init.pathname === undefined && init.search === undefined)
          result.search = esc(baseURL.search[0] === "?" ? baseURL.search.slice(1) : baseURL.search);
        if (init.protocol === undefined && init.hostname === undefined && init.port === undefined && init.pathname === undefined && init.search === undefined && init.hash === undefined)
          result.hash = esc(baseURL.hash[0] === "#" ? baseURL.hash.slice(1) : baseURL.hash);
      }
      const has = (k) => init[k] !== undefined;
      if (has("protocol")) {
        let v = "" + init.protocol;
        if (v.length > 0 && v[v.length - 1] === ":") v = v.slice(0, -1);
        result.protocol = type === "pattern" ? v : canonProtocol(v);
      }
      if (has("username")) result.username = type === "pattern" ? "" + init.username : canonUsername("" + init.username);
      if (has("password")) result.password = type === "pattern" ? "" + init.password : canonPassword("" + init.password);
      if (has("hostname")) result.hostname = type === "pattern" ? "" + init.hostname : canonHostname("" + init.hostname);
      if (has("port")) result.port = type === "pattern" ? "" + init.port : canonPort("" + init.port, result.protocol);
      if (has("pathname")) {
        let v = "" + init.pathname;
        if (baseURL && !isAbsolutePathname(v, type)) {
          const basePath = type === "pattern" ? escapePattern(baseURL.pathname) : baseURL.pathname;
          const slashIndex = basePath.lastIndexOf("/");
          if (slashIndex >= 0) v = basePath.slice(0, slashIndex + 1) + v;
        }
        if (type === "pattern") result.pathname = v;
        else {
          const isSpecial = !result.protocol || Object.prototype.hasOwnProperty.call(SPECIAL_SCHEMES, result.protocol);
          result.pathname = isSpecial ? canonPathname(v) : canonOpaquePathname(v);
        }
      }
      if (has("search")) {
        let v = "" + init.search;
        if (v.length > 0 && v[0] === "?") v = v.slice(1);
        result.search = type === "pattern" ? v : canonSearch(v);
      }
      if (has("hash")) {
        let v = "" + init.hash;
        if (v.length > 0 && v[0] === "#") v = v.slice(1);
        result.hash = type === "pattern" ? v : canonHash(v);
      }
      return result;
    }

    // ---------------------------------------------------------------- class
    const COMPONENTS = ["protocol", "username", "password", "hostname", "port", "pathname", "search", "hash"];
    const COMP = Symbol("mbun.urlPatternComponents");

    class URLPattern {
      constructor(input = {}, baseURLOrOptions = undefined, maybeOptions = undefined) {
        let init, options = {};
        if (typeof input === "string") {
          let baseURL;
          if (typeof baseURLOrOptions === "string") { baseURL = baseURLOrOptions; options = maybeOptions || {}; }
          else if (baseURLOrOptions !== undefined && maybeOptions === undefined && typeof baseURLOrOptions === "object") {
            // string pattern + options object (no baseURL)
            options = baseURLOrOptions || {};
          } else if (baseURLOrOptions === undefined) {
            options = maybeOptions || {};
          } else {
            throw new TypeError("Invalid baseURL argument");
          }
          init = parseConstructorString(input);
          if (init.protocol === undefined && baseURL === undefined)
            throw new TypeError("A relative pattern must have a baseURL");
          if (baseURL !== undefined) init.baseURL = baseURL;
        } else if (input && typeof input === "object") {
          if (input instanceof URL) {
            // A URL object is stringified and parsed as a constructor string.
            init = parseConstructorString("" + input);
          } else {
            if (typeof baseURLOrOptions === "string") throw new TypeError("baseURL is not allowed with a URLPatternInit");
            options = baseURLOrOptions || {};
            init = input;
          }
        } else {
          throw new TypeError("URLPattern input must be a string or object");
        }

        const defaults = { protocol: "*", username: "*", password: "*", hostname: "*", port: "*", pathname: "*", search: "*", hash: "*" };
        const processed = processInit(init, "pattern", defaults);
        if (Object.prototype.hasOwnProperty.call(SPECIAL_SCHEMES, processed.protocol) &&
            SPECIAL_SCHEMES[processed.protocol] === processed.port) {
          processed.port = "";
        }
        const flags = options && options.ignoreCase ? "i" : "";
        const components = {};
        components.protocol = compileComponent(processed.protocol, canonProtocol, DEFAULT_OPTIONS, flags);
        components.username = compileComponent(processed.username, canonUsername, DEFAULT_OPTIONS, flags);
        components.password = compileComponent(processed.password, canonPassword, DEFAULT_OPTIONS, flags);
        const hn = processed.hostname;
        const isIPv6Pattern = typeof hn === "string" && (hn[0] === "[" || hn.startsWith("{[") || hn.startsWith("\\["));
        components.hostname = compileComponent(hn, isIPv6Pattern ? canonIPv6Hostname : canonHostname, HOSTNAME_OPTIONS, flags);
        components.port = compileComponent(processed.port, (v) => canonPort(v), DEFAULT_OPTIONS, flags);
        const special = protocolMatchesSpecialScheme(components.protocol);
        if (special) components.pathname = compileComponent(processed.pathname, canonPathname, PATHNAME_OPTIONS, flags);
        else components.pathname = compileComponent(processed.pathname, canonOpaquePathname, DEFAULT_OPTIONS, flags);
        components.search = compileComponent(processed.search, canonSearch, DEFAULT_OPTIONS, flags);
        components.hash = compileComponent(processed.hash, canonHash, DEFAULT_OPTIONS, flags);
        Object.defineProperty(this, COMP, { value: components, enumerable: false, writable: false, configurable: true });
      }

      test(input = {}, baseURL = undefined) { return matchPattern(this, input, baseURL) !== null; }
      exec(input = {}, baseURL = undefined) { return matchPattern(this, input, baseURL); }

      get protocol() { return this[COMP].protocol.patternString; }
      get username() { return this[COMP].username.patternString; }
      get password() { return this[COMP].password.patternString; }
      get hostname() { return this[COMP].hostname.patternString; }
      get port() { return this[COMP].port.patternString; }
      get pathname() { return this[COMP].pathname.patternString; }
      get search() { return this[COMP].search.patternString; }
      get hash() { return this[COMP].hash.patternString; }
      get hasRegExpGroups() { return COMPONENTS.some((n) => this[COMP][n].hasRegexpGroups); }
    }

    function matchPattern(self, input, baseURLString) {
        const components = self[COMP];
        let values = { protocol: "", username: "", password: "", hostname: "", port: "", pathname: "", search: "", hash: "" };
        const inputs = [input];
        if (typeof input === "string" || input instanceof URL) {
          if (baseURLString !== undefined) inputs.push(baseURLString);
          let url;
          try { url = new URL(input, baseURLString); } catch (_) { return null; }
          values = {
            protocol: url.protocol.slice(0, -1),
            username: url.username,
            password: url.password,
            hostname: url.hostname,
            port: url.port,
            pathname: url.pathname,
            search: url.search[0] === "?" ? url.search.slice(1) : url.search,
            hash: url.hash[0] === "#" ? url.hash.slice(1) : url.hash,
          };
        } else if (input && typeof input === "object") {
          if (baseURLString !== undefined) throw new TypeError("baseURL is not allowed with a URLPatternInit input");
          values = processInit(input, "url", values);
        } else {
          throw new TypeError("URLPattern input must be a string or object");
        }
        const result = { inputs };
        for (const name of COMPONENTS) {
          const component = components[name];
          const m = component.regexp.exec(values[name]);
          if (!m) return null;
          const groups = {};
          for (let i = 0; i < component.names.length; i++) groups[component.names[i]] = m[i + 1];
          result[name] = { input: values[name], groups };
        }
        return result;
    }

    G.URLPattern = URLPattern;
  } catch (e) {}
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
