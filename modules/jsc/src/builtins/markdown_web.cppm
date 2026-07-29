// Markdown and Web payload partition; keep raw bytes aligned with js_builtins.cppm lines 6365-7695.
export module mbun.jsc.js_builtins:markdown_web;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kMarkdownWebJS = R"JS(  // ---------------- Emit tree -> events ----------------
  function emitNode(r, node, inTight) {
    switch (node.type) {
      case "document": for (const ch of node.children) emitNode(r, ch, false); break;
      case "blockquote":
        r.enterBlock("quote");
        for (const ch of node.children) emitNode(r, ch, false);
        r.leaveBlock("quote");
        break;
      case "list": {
        r.enterBlock(node.ordered ? "ol" : "ul", node.ordered ? node.start : 0);
        for (const item of node.items) {
          r.enterBlock("li", { task: item.task });
          for (const ch of item.children) emitNode(r, ch, node.tight);
          r.leaveBlock("li");
        }
        r.leaveBlock(node.ordered ? "ol" : "ul");
        break;
      }
      case "heading":
        r.enterBlock("h", node.level);
        emitInlineNodes(r, finalizeInline(parseInline(node.text)));
        r.leaveBlock("h");
        break;
      case "paragraph": {
        const inl = finalizeInline(parseInline(node.text));
        if (inTight) { emitInlineNodes(r, inl); }
        else { r.enterBlock("p"); emitInlineNodes(r, inl); r.leaveBlock("p"); }
        break;
      }
      case "code":
        r.enterBlock("code", 0, { fenced: true, lang: node.lang });
        r.text("normal", node.literal + "\n");
        r.leaveBlock("code");
        break;
      case "hr": r.enterBlock("hr"); r.leaveBlock("hr"); break;
      case "table": emitTable(r, node); break;
    }
  }

  function emitTable(r, node) {
    r.enterBlock("table");
    r.enterBlock("thead");
    r.enterBlock("tr");
    node.header.forEach((cell, i) => {
      r.enterBlock("th", { align: node.align[i] || "default" });
      emitInlineNodes(r, finalizeInline(parseInline(cell)));
      r.leaveBlock("th");
    });
    r.leaveBlock("tr");
    r.leaveBlock("thead");
    if (node.rows.length) {
      r.enterBlock("tbody");
      for (const row of node.rows) {
        r.enterBlock("tr");
        for (let i = 0; i < node.header.length; i++) {
          r.enterBlock("td", { align: node.align[i] || "default" });
          emitInlineNodes(r, finalizeInline(parseInline(row[i] || "")));
          r.leaveBlock("td");
        }
        r.leaveBlock("tr");
      }
      r.leaveBlock("tbody");
    }
    r.leaveBlock("table");
  }

  function emitInlineNodes(r, nodes) {
    for (const nd of nodes) {
      switch (nd.type) {
        case "text": r.text("normal", nd.value); break;
        case "code": r.enterSpan("code"); r.text("code", nd.value); r.leaveSpan("code"); break;
        case "strong": r.enterSpan("strong"); emitInlineNodes(r, cleanDelims(nd.children)); r.leaveSpan("strong"); break;
        case "em": r.enterSpan("em"); emitInlineNodes(r, cleanDelims(nd.children)); r.leaveSpan("em"); break;
        case "del": r.enterSpan("del"); emitInlineNodes(r, cleanDelims(nd.children)); r.leaveSpan("del"); break;
        case "link": r.enterSpan("a", { href: nd.href, title: nd.title }); emitInlineNodes(r, cleanDelims(nd.children)); r.leaveSpan("a"); break;
        case "image": r.enterSpan("img", { href: nd.href, title: nd.title }); emitInlineNodes(r, cleanDelims(nd.children)); r.leaveSpan("img"); break;
        case "autolink": {
          const href = nd.kind === "email" ? "mailto:" + nd.href : nd.href;
          r.enterSpan("a", { href });
          r.text("normal", nd.text);
          r.leaveSpan("a");
          break;
        }
        case "wikilink": r.enterSpan("wikilink"); r.text("normal", nd.target); r.leaveSpan("wikilink"); break;
        case "delim": if (nd.remaining > 0) r.text("normal", nd.ch.repeat(nd.remaining)); break;
      }
    }
  }

  function renderAnsi(src, opts) {
    opts = opts || {};
    const theme = {
      light: !!opts.light,
      columns: opts.columns == null ? 80 : opts.columns | 0,
      colors: opts.colors !== false,
      hyperlinks: !!opts.hyperlinks,
    };
    const doc = parseDocument(String(src));
    const r = new Renderer(theme);
    emitNode(r, doc, false);
    return r.output();
  }
  globalThis.__renderMarkdownAnsi = renderAnsi;

  // ---------------- HTML renderer (Bun.markdown.html / .render) ----------------
  // Walks the same block/inline AST as the ANSI renderer and emits HTML in the
  // style of md4c (bun's markdown engine). GFM extensions (tables, strikethrough,
  // tasklists, permissive autolinks, wiki-links) are OFF unless enabled via opts,
  // matching bun's default CommonMark-only behavior.
  function htmlEscText(s) {
    return String(s).replace(/[&<>]/g, (c) => (c === "&" ? "&amp;" : c === "<" ? "&lt;" : "&gt;"));
  }
  function htmlEscAttr(s) {
    return String(s).replace(/[&<>"]/g, (c) => (c === "&" ? "&amp;" : c === "<" ? "&lt;" : c === ">" ? "&gt;" : "&quot;"));
  }
  function htmlEscHref(s) {
    // md4c escapes the same set in URLs plus leaves other bytes intact.
    return String(s).replace(/[&<>"]/g, (c) => (c === "&" ? "&amp;" : c === "<" ? "&lt;" : c === ">" ? "&gt;" : "&quot;"));
  }
  // GFM's tagfilter is deliberately narrower than an HTML sanitizer: it does
  // not remove a tag or escape its complete text.  It only changes the leading
  // '<' on the nine raw-HTML tags named by the extension, leaving both the
  // remainder of the tag and ordinary (allowed) HTML untouched.
  const tagFilterNames = new Set(["title", "textarea", "style", "xmp", "iframe", "noembed", "noframes", "script", "plaintext"]);
  const tagFilterBlockNames = new Set(["title", "textarea", "style", "iframe", "noframes", "script"]);
  const tagFilterToken = /\\?<\/?[A-Za-z][A-Za-z0-9-]*(?:\s[^<>]*?)?\/?>/g;
  function renderTagFilteredText(text, O, quoteText) {
    const source = String(text);
    let out = "", pos = 0, match;
    const renderText = (part) => {
      let rendered = renderInlineHtml(finalizeInline(parseInline(part)), O);
      // cmark-gfm entity-escapes quotes in inline raw-HTML text.  Block HTML
      // retains its literal payload, so keep that path separate below.
      if (quoteText) rendered = rendered.replace(/"/g, "&quot;");
      return rendered;
    };
    tagFilterToken.lastIndex = 0;
    while ((match = tagFilterToken.exec(source)) !== null) {
      out += renderText(source.slice(pos, match.index));
      const token = match[0];
      const escaped = token[0] === "\\";
      const html = escaped ? token.slice(1) : token;
      const name = /^<\/?([A-Za-z][A-Za-z0-9-]*)/.exec(html);
      if (escaped || (name && tagFilterNames.has(name[1].toLowerCase()))) out += "&lt;" + html.slice(1);
      else out += html;
      pos = match.index + token.length;
    }
    return out + renderText(source.slice(pos));
  }
  function isTagFilterHtmlBlock(text) {
    const match = /^\s*<\/?([A-Za-z][A-Za-z0-9-]*)\b/.exec(String(text));
    return !!(match && tagFilterBlockNames.has(match[1].toLowerCase()));
  }
  function inlinePlainText(nodes) {
    let s = "";
    for (const nd of nodes) {
      switch (nd.type) {
        case "text": s += nd.value; break;
        case "code": s += nd.value; break;
        case "autolink": s += nd.text; break;
        case "wikilink": s += nd.target; break;
        case "em": case "strong": case "del": case "link": case "image":
          s += inlinePlainText(nd.children || []); break;
        case "delim": if (nd.remaining > 0) s += nd.ch.repeat(nd.remaining); break;
      }
    }
    return s;
  }
  function slugify(t) {
    return String(t).toLowerCase().trim().replace(/[^\w\s-]/g, "").replace(/\s+/g, "-");
  }
  function renderInlineHtml(nodes, O) {
    let out = "";
    for (const nd of nodes) {
      switch (nd.type) {
        case "text": out += htmlEscText(nd.value); break;
        case "code": out += "<code>" + htmlEscText(nd.value) + "</code>"; break;
        case "em": out += "<em>" + renderInlineHtml(nd.children, O) + "</em>"; break;
        case "strong": out += "<strong>" + renderInlineHtml(nd.children, O) + "</strong>"; break;
        case "del":
          if (O.strikethrough) out += "<del>" + renderInlineHtml(nd.children, O) + "</del>";
          else out += "~~" + renderInlineHtml(nd.children, O) + "~~";
          break;
        case "link": {
          const t = nd.title ? ' title="' + htmlEscAttr(nd.title) + '"' : "";
          out += '<a href="' + htmlEscHref(nd.href) + '"' + t + ">" + renderInlineHtml(nd.children, O) + "</a>";
          break;
        }
        case "image": {
          const alt = htmlEscAttr(inlinePlainText(nd.children || []));
          const t = nd.title ? ' title="' + htmlEscAttr(nd.title) + '"' : "";
          out += '<img src="' + htmlEscHref(nd.href) + '" alt="' + alt + '"' + t + " />";
          break;
        }
        case "autolink": {
          if (nd.bare && !O.autolinks) { out += htmlEscText(nd.text); break; }
          const href = nd.kind === "email" ? "mailto:" + nd.href : nd.href;
          out += '<a href="' + htmlEscHref(href) + '">' + htmlEscText(nd.text) + "</a>";
          break;
        }
        case "wikilink":
          if (O.wikilinks) out += '<a href="' + htmlEscHref(nd.target) + '">' + htmlEscText(nd.target) + "</a>";
          else out += htmlEscText("[[" + nd.target + "]]");
          break;
        case "delim": if (nd.remaining > 0) out += htmlEscText(nd.ch.repeat(nd.remaining)); break;
      }
    }
    return out;
  }
  function renderTableHtml(node, O) {
    const alignAttr = (i) => {
      const a = node.align[i];
      return a && a !== "default" ? ' align="' + a + '"' : "";
    };
    let out = "<table>\n<thead>\n<tr>\n";
    for (let i = 0; i < node.header.length; i++) {
      out += "<th" + alignAttr(i) + ">" + renderInlineHtml(finalizeInline(parseInline(node.header[i])), O) + "</th>\n";
    }
    out += "</tr>\n</thead>\n";
    if (node.rows.length) {
      out += "<tbody>\n";
      for (const row of node.rows) {
        out += "<tr>\n";
        for (let i = 0; i < node.header.length; i++) {
          out += "<td" + alignAttr(i) + ">" + renderInlineHtml(finalizeInline(parseInline(row[i] || "")), O) + "</td>\n";
        }
        out += "</tr>\n";
      }
      out += "</tbody>\n";
    }
    out += "</table>\n";
    return out;
  }
  function renderListHtml(node, O) {
    const tag = node.ordered ? "ol" : "ul";
    let startAttr = "";
    if (node.ordered && node.start !== 1) startAttr = ' start="' + node.start + '"';
    let out = "<" + tag + startAttr + ">\n";
    for (const item of node.items) {
      let content = renderBlocksHtml(item.children, O, node.tight);
      let liAttr = "";
      let prefix = "";
      if (item.task !== null && O.tasklists) {
        const checked = item.task === "checked" ? ' checked=""' : "";
        prefix = '<input' + checked + ' disabled="" type="checkbox"> ';
        liAttr = ' class="task-list-item"';
      }
      if (node.tight) out += "<li" + liAttr + ">" + prefix + content.replace(/\n$/, "") + "</li>\n";
      else out += "<li" + liAttr + ">\n" + prefix + content + "</li>\n";
    }
    out += "</" + tag + ">\n";
    return out;
  }
  function renderBlocksHtml(children, O, tight) {
    let out = "";
    for (const node of children) {
      switch (node.type) {
        case "heading": {
          const inNodes = finalizeInline(parseInline(node.text));
          const inl = renderInlineHtml(inNodes, O);
          let idAttr = "";
          let inner = inl;
          if (O.headingIds) {
            const id = O._slug(inlinePlainText(inNodes));
            idAttr = ' id="' + htmlEscAttr(id) + '"';
            if (O.headingAuto) inner = '<a href="#' + htmlEscAttr(id) + '">' + inl + "</a>";
          }
          out += "<h" + node.level + idAttr + ">" + inner + "</h" + node.level + ">\n";
          break;
        }
        case "paragraph": {
          if (O.tagFilter && isTagFilterHtmlBlock(node.text)) {
            out += renderTagFilteredText(node.text, O, false) + "\n";
            break;
          }
          const inl = O.tagFilter
            ? renderTagFilteredText(node.text, O, true)
            : renderInlineHtml(finalizeInline(parseInline(node.text)), O);
          if (tight) out += inl + "\n";
          else out += "<p>" + inl + "</p>\n";
          break;
        }
        case "code": {
          const cls = node.lang ? ' class="language-' + htmlEscAttr(node.lang) + '"' : "";
          const body = node.literal ? htmlEscText(node.literal) + "\n" : "";
          out += "<pre><code" + cls + ">" + body + "</code></pre>\n";
          break;
        }
        case "hr": out += "<hr />\n"; break;
        case "blockquote":
          out += "<blockquote>\n" + renderBlocksHtml(node.children, O, false) + "</blockquote>\n";
          break;
        case "list": out += renderListHtml(node, O); break;
        case "table":
          if (O.tables) out += renderTableHtml(node, O);
          else {
            // Without the tables extension a pipe table is plain paragraph text.
            const raw = [node.header.join(" | ")].concat(node.rows.map((r) => r.join(" | "))).join("\n");
            out += "<p>" + renderInlineHtml(finalizeInline(parseInline(raw)), O) + "</p>\n";
          }
          break;
      }
    }
    return out;
  }
  function renderHtml(src, opts) {
    opts = opts || {};
    const O = {
      strikethrough: !!opts.strikethrough,
      tables: !!opts.tables,
      tasklists: !!opts.tasklists,
      autolinks: !!opts.autolinks,
      wikilinks: !!opts.wikilinks,
      tagFilter: !!opts.tagFilter,
    };
    let hIds = false, hAuto = false;
    const h = opts.headings;
    if (h === true) { hIds = true; hAuto = true; }
    else if (h && typeof h === "object") { hIds = !!h.ids; hAuto = !!h.autolink; }
    O.headingIds = hIds;
    O.headingAuto = hAuto && hIds;
    const usedIds = Object.create(null);
    O._slug = function (text) {
      const base = slugify(text);
      let id = base, n = 1;
      while (id in usedIds) { id = base + "-" + n; n++; }
      usedIds[id] = true;
      return id;
    };
    const doc = parseDocument(String(src));
    return renderBlocksHtml(doc.children, O, false);
  }

  const G = globalThis;
  if (G.Bun && typeof G.Bun.markdown === "undefined") {
    G.Bun.markdown = { ansi: renderAnsi, html: renderHtml, render: renderHtml };
  } else if (G.Bun && G.Bun.markdown) {
    if (typeof G.Bun.markdown.html === "undefined") G.Bun.markdown.html = renderHtml;
    if (typeof G.Bun.markdown.render === "undefined") G.Bun.markdown.render = renderHtml;
  }
})();
  }

  // ---- console extras (time/count/group/debug/…) ----
  const con = G.console || (G.console = {});
  if (typeof con.time !== "function") {
    const _t = {}, _cnt = {}, nowMs = () => (G.Bun && Bun.nanoseconds ? Bun.nanoseconds() / 1e6 : 0);
    con.time = (l) => { _t[l == null ? "default" : l] = nowMs(); };
    con.timeEnd = (l) => { const k = l == null ? "default" : l; const t = _t[k]; con.error("[" + (t != null ? (nowMs() - t).toFixed(2) : "0.00") + "ms]" + (k === "" ? "" : " " + k)); delete _t[k]; };
    con.timeLog = (l, ...a) => { const k = l == null ? "default" : l; const t = _t[k]; con.error("[" + (t != null ? (nowMs() - t).toFixed(2) : "0.00") + "ms]" + (k === "" ? "" : " " + k), ...a); };
    con.count = (l) => { l = l == null ? "default" : l; _cnt[l] = (_cnt[l] || 0) + 1; con.log(l + ": " + _cnt[l]); };
    con.countReset = (l) => { delete _cnt[l == null ? "default" : l]; };
    con.group = con.log; con.groupCollapsed = con.log; con.groupEnd = () => {};
    con.debug = con.log; con.info = con.log; con.trace = con.log; con.dir = con.log;
    con.table = con.log;
    if (typeof con.warn !== "function") con.warn = con.error || con.log;
    con.assert = (c2) => { if (!c2) con.error("Assertion failed"); };
  }
  // console.log/error/… apply util.format when the first arg is a format string
  // with %-specifiers (node/bun behavior). Non-format calls pass through unchanged
  // so object rendering is preserved. Guard with a flag to avoid double-wrapping.
  if (!con.__fmtWrapped) {
    con.__fmtWrapped = true;
    for (const meth of ["log", "error", "info", "warn", "debug"]) {
      const native = con[meth];
      if (typeof native !== "function") continue;
      // Non-format object args render through Bun.inspect (bun's console layout:
      // multi-line, double-quoted, trailing comma), NOT node's util.inspect —
      // real bun's console.log output === Bun.inspect(x). util.inspect stays
      // node-style for node:util tests. ref bun ConsoleObject format path.
      const inspect1 = (x) => (typeof x === "string" ? x : (G.Bun && Bun.inspect ? Bun.inspect(x) : util.inspect(x)));
      // Computed-name method shorthand: keeps the correct `.name` (the test
      // test-console-methods asserts console.log.name === 'log') and is
      // non-constructable (`new console.log()` must throw), unlike a plain
      // function expression.
      con[meth] = ({ [meth](...a) {
        const text = typeof a[0] === "string" && /%[sdifjoOc%]/.test(a[0])
          ? util.format(...a) : a.map(inspect1).join(" ");
        // console._stdout/_stderr are intentionally mutable lazy properties.
        // Retain the native fast path for the normal process streams, but route
        // an overridden target through its write method.
        const stdout = meth === "log" || meth === "info" || meth === "debug";
        const target = stdout ? con._stdout : con._stderr;
        const standard = G.process && (stdout ? G.process.stdout : G.process.stderr);
        if (target && target !== standard && typeof target.write === "function") {
          target.write(text + "\n");
          return;
        }
        return native(text);
      } })[meth];
    }
  }

  // ---- performance ----
  if (typeof G.performance === "undefined") {
    G.performance = { now: () => (G.Bun && Bun.nanoseconds ? Bun.nanoseconds() / 1e6 : 0),
      timeOrigin: 0, mark: () => {}, measure: () => {}, getEntriesByName: () => [],
      getEntriesByType: () => [], getEntries: () => [], clearMarks: () => {}, clearMeasures: () => {} };
  }

  // ---- ReadableStream (minimal: enqueue via start(controller), reader/iterator) ----
  if (typeof G.ReadableStream === "undefined") {
    G.ReadableStream = class ReadableStream {
      constructor(src) {
        this._chunks = []; this._src = src || {};
        const controller = { enqueue: (c) => this._chunks.push(c), close: () => {}, error: () => {}, get desiredSize() { return 1; } };
        if (this._src.start) { try { this._src.start(controller); } catch (e) {} }
        this._pull = this._src.pull; this._controller = controller;
      }
      getReader() { let i = 0; const s = this; return { read() { return i < s._chunks.length ? Promise.resolve({ value: s._chunks[i++], done: false }) : Promise.resolve({ value: undefined, done: true }); }, releaseLock() {}, cancel() { return Promise.resolve(); }, closed: Promise.resolve() }; }
      [Symbol.asyncIterator]() { let i = 0; const s = this; return { next() { return i < s._chunks.length ? Promise.resolve({ value: s._chunks[i++], done: false }) : Promise.resolve({ value: undefined, done: true }); } }; }
      cancel() { return Promise.resolve(); }
      pipeTo(dest) { const w = dest && dest.getWriter ? dest.getWriter() : null; for (const c of this._chunks) { if (w) w.write(c); else if (dest && dest.write) dest.write(c); } if (w && w.close) w.close(); else if (dest && dest.close) dest.close(); return Promise.resolve(); }
      // pipeThrough({writable, readable}): feed our chunks into the transform's
      // writable and return its readable (chunks flow synchronously here).
      pipeThrough(transform) { this.pipeTo(transform.writable); return transform.readable; }
      tee() { return [this, this]; }
    };
  }
  if (typeof G.WritableStream === "undefined") {
    G.WritableStream = class WritableStream {
      constructor(sink) { this._sink = sink || {}; this._closed = false; }
      getWriter() { const s = this; return {
        write(chunk) { try { return Promise.resolve(s._sink.write ? s._sink.write(chunk) : undefined); } catch (e) { return Promise.reject(e); } },
        close() { s._closed = true; try { return Promise.resolve(s._sink.close ? s._sink.close() : undefined); } catch (e) { return Promise.reject(e); } },
        abort(r) { try { return Promise.resolve(s._sink.abort ? s._sink.abort(r) : undefined); } catch (e) { return Promise.reject(e); } },
        releaseLock() {}, ready: Promise.resolve(), closed: Promise.resolve(),
      }; }
      close() { this._closed = true; return Promise.resolve(this._sink.close ? this._sink.close() : undefined); }
      abort(r) { return Promise.resolve(this._sink.abort ? this._sink.abort(r) : undefined); }
    };
  }
  if (typeof G.TransformStream === "undefined") {
    G.TransformStream = class TransformStream {
      constructor(transformer) { transformer = transformer || {}; let ctrl; this.readable = new G.ReadableStream({ start(c) { ctrl = c; } });
        this.writable = new G.WritableStream({ write(chunk) { return transformer.transform ? transformer.transform(chunk, ctrl) : ctrl.enqueue(chunk); }, close() { if (transformer.flush) transformer.flush(ctrl); ctrl.close && ctrl.close(); } }); }
    };
  }
  // ---- Request / Blob (minimal) ----
  if (typeof G.Request === "undefined") {
    G.Request = class Request {
      constructor(url, init) {
        init = init || {};
        const userInit = init;   // pre-merge: distinguishes an explicit signal from an inherited one
        let inputSignal;
        let rawUrl;
        if (typeof url === "object" && url && !(url instanceof G.URL)) { inputSignal = url.signal; init = Object.assign({}, url, init); rawUrl = (url.url !== undefined && url.url !== null) ? String(url.url) : String(url); if (init.body === undefined && url._body !== undefined) init.body = url._body; }
        else rawUrl = String(url);
        // Per spec the input is serialized through the URL parser (Request.rs: href_from_string).
        if (!rawUrl) throw new Error("Failed to construct 'Request': url is required.");
        let href = "";
        try { href = new G.URL(rawUrl).href; } catch (e) { href = ""; }
        if (!href) { const e = new TypeError("Failed to construct 'Request': Invalid URL \"" + rawUrl + "\""); e.code = "ERR_INVALID_URL"; throw e; }
        // A subclass may declare getter-only prototype accessors (get method(){...});
        // super()'s plain assignment would throw "assign to readonly property" and
        // abort construction. bun's Request is native (prototype getters over slots),
        // so a subclass getter simply shadows. Compute Headers first (keeps its
        // validation), then swallow the readonly-accessor throw so the override wins.
        const __h = new G.Headers(init.headers);
        try { this.url = href; } catch (e) {}
        try { this.method = (init.method || "GET").toUpperCase(); } catch (e) {}
        try { this.headers = __h; } catch (e) {}
        // WebIDL AbortSignal? (Request.rs:1394-1418): an explicit user signal wins
        // (null = detach → fresh non-aborted signal; a real AbortSignal is stored;
        // anything else throws); absent/undefined inherits the input Request's signal.
        if (userInit.signal !== undefined) {
          if (userInit.signal === null) this.signal = new G.AbortSignal();
          else if (userInit.signal instanceof G.AbortSignal) this.signal = userInit.signal;
          else throw new TypeError("Failed to construct 'Request': signal is not of type AbortSignal.");
        } else {
          this.signal = (inputSignal instanceof G.AbortSignal) ? inputSignal : new G.AbortSignal();
        }
        // Request.redirect mode (spec default "follow"); fetch(Request) reads it
        // when init doesn't override. A Request built from another Request
        // inherits its mode via the init merge above.
        this.redirect = init.redirect || "follow";
        this._used = false;
        const norm = G.__mbunNormalizeBody(init.body);
        const body = norm.body;
        if (norm.contentType && !this.headers.has("content-type")) this.headers.set("content-type", norm.contentType);
        // The `_body` slot is the synchronous raw-body fast path consumed by
        // fetch(request) (js_net.cppm: `body: input._body`) and clone(). It must
        // retain every buffer-like body that u8()/consume can replay from a raw
        // reference, or a Request built from that body fetches EMPTY even though
        // its `_stream` (and thus req.text()/clone().text()) reads it correctly.
        // ArrayBuffer / ArrayBuffer views were omitted here, so
        // `fetch(new Request({ body: <ArrayBuffer> }))` sent no body at all
        // (js/web/fetch/body-stream reader matrix, issue #15). Streams stay out
        // (fetch/clone fall back to the `_stream` tee path for those).
        this._body = (body != null && !isStream(body) && (typeof body === "string" || body instanceof Uint8Array || ArrayBuffer.isView(body) || body instanceof ArrayBuffer || (body && body._u8))) ? body : undefined;
        // PERF: bodyToStream() allocates a ReadableStream and TextEncoder-encodes
        // the whole body on EVERY construction, which dominated `new Request(...)`
        // (js/web/request/request-clone-leak.test.ts builds 18M of them). A string
        // or Uint8Array body is byte-identical whether encoded now or on first
        // `.body`/consume — `bodyToStream` enqueues the SAME Uint8Array reference,
        // it does not snapshot — so defer it. Everything else (streams, which must
        // be validated eagerly by Body.rs:1009, and FormData/URLSearchParams/
        // ArrayBuffer views, which ARE snapshotted) keeps the eager path.
        if (body != null && (typeof body === "string" || body instanceof Uint8Array)) {
          this.__st = undefined;
          this.__pendingBody = body;
        } else {
          this._stream = bodyToStream(body);
        }
      }
      // Lazily materialized companion of `__pendingBody` (see the constructor).
      get _stream() { if (this.__st === undefined) { this.__st = this.__pendingBody === undefined ? null : bodyToStream(this.__pendingBody); this.__pendingBody = undefined; } return this.__st; }
      set _stream(v) { this.__st = v === undefined ? null : v; this.__pendingBody = undefined; }
      get body() { return this._stream || null; }
      // `__st` (not `_stream`) on the disturbed checks: an unmaterialized lazy body
      // can be neither disturbed nor locked, so reading it through the getter would
      // build the stream just to prove it is pristine.
      get bodyUsed() { return bodyDisturbed(this._body, this._used, this.__st); }   // Body.rs:1860
      _consume(kind) {
        // Same body-before-stream ordering as Response._consume: Body.rs:1784
        // rejects with "Body already used" (ERR_BODY_ALREADY_USED) before the
        // stream is touched. Verified against bun-rust 1.4.0: a second
        // `request.text()` rejects with that message, not the stream-level one.
        if (bodyDisturbed(this._body, this._used, this.__st) || (isStream(this._stream) && this._stream.locked))
          return Promise.reject(bodyAlreadyUsed());
        // Same MIME gate as Response._consume (defined in the process_web
        // partition -- the partitions are concatenated into one script by
        // js_builtins.cppm:57,61, so its top-level consts are in scope here).
        // Blueprint: Body.rs:2038-2077 get_form_data; Request.rs:473
        // get_form_data_encoding is byte-identical to Response.rs:404.
        let fdEncoding;
        if (kind === "formData") {
          fdEncoding = formDataEncodingForHeaders(this.headers);
          if (fdEncoding === null) return Promise.reject(formDataMimeError());
        }
        this._used = true;
        const S = G.__mbunStreams;
        if (this._stream && S && S.isReadableStream(this._stream)) {
          if (kind === "text") return S.text(this._stream);
          if (kind === "json") return S.json(this._stream);
          if (kind === "bytes") return S.bytes(this._stream);
          if (kind === "arrayBuffer") return S.arrayBuffer(this._stream);
          if (kind === "blob") return S.array(this._stream).then((cs) => new G.Blob(cs, { type: (this.headers.get && this.headers.get("content-type")) || "" }));
          if (kind === "formData") return S.bytes(this._stream).then((u8) => formDataParseBody(u8, fdEncoding));
        }
        const b = this._body;
        // Same synthetic-allocation-limit cap as Response._consume; a Blob body
        // delegates so its own guard fires. arrayBuffer() stays exempt.
        if (kind === "text") { if (b == null) return Promise.resolve(""); if (b instanceof Uint8Array) { G.__mbunCheckAllocLimit(b.length, "text"); return Promise.resolve(td.decode(b)); } if (b && typeof b.text === "function") return b.text(); return Promise.resolve(String(b)); }
        // Body.rs:1884 get_json shares get_text's cap but reports the JSON
        // message ("Cannot parse a JSON string longer than 2^32-1 characters").
        if (kind === "json") {
          let t;
          if (b == null) t = Promise.resolve("");
          else if (b instanceof Uint8Array) { G.__mbunCheckAllocLimit(b.length, "json"); t = Promise.resolve(td.decode(b)); }
          else if (b && G.Blob && b instanceof G.Blob) { G.__mbunCheckAllocLimit(b.size, "json"); t = Promise.resolve(td.decode(b._u8)); }
          else if (b && b._u8 instanceof Uint8Array) { G.__mbunCheckAllocLimit(b._u8.length, "json"); t = Promise.resolve(td.decode(b._u8)); }
          else if (b && typeof b.text === "function") t = b.text();
          else t = Promise.resolve(String(b));
          return t.then((s) => JSON.parse(s));
        }
        // Cap on `size` (a number) before touching `_u8`, which would otherwise
        // join the whole part list to answer a call that is about to throw.
        if (kind === "bytes") { if (b instanceof Uint8Array) { G.__mbunCheckAllocLimit(b.length, "bytes"); return Promise.resolve(new Uint8Array(b)); } if (b && G.Blob && b instanceof G.Blob) { G.__mbunCheckAllocLimit(b.size, "bytes"); return Promise.resolve(new Uint8Array(b._u8)); } if (b && b._u8 instanceof Uint8Array) { G.__mbunCheckAllocLimit(b._u8.length, "bytes"); return Promise.resolve(new Uint8Array(b._u8)); } return this._consume("text").then((t) => te.encode(t)); }
        if (kind === "arrayBuffer") {
          const u = b instanceof Uint8Array ? b : (b && b._u8 instanceof Uint8Array ? b._u8 : null);
          if (u) return Promise.resolve(u.buffer.slice(u.byteOffset, u.byteOffset + u.byteLength));
          return this._consume("bytes").then((u8) => u8.buffer.slice(u8.byteOffset, u8.byteOffset + u8.byteLength));
        }
        if (kind === "blob") return Promise.resolve(new G.Blob([b == null ? "" : b]));
        if (kind === "formData") return this._consume("bytes").then((u8) => formDataParseBody(u8, fdEncoding));
      }
      text() { return this._consume("text"); }
      json() { return this._consume("json"); }
      arrayBuffer() { return this._consume("arrayBuffer"); }
      bytes() { return this._consume("bytes"); }
      blob() { return this._consume("blob"); }
      formData() { return this._consume("formData"); }
      // Same shared-stream bug Response.clone() had: `this._stream || this._body`
      // gave both requests one stream. Blueprint: Request.rs:1610 clone_into ->
      // the same Body.rs:1591 clone_with_readable_stream (tee a Locked body,
      // refcount a byte body).
      clone() {
        throwIfBodyUnusable(this._body, this._used, this.__st);   // spec step 1
        const init = { method: this.method, headers: this.headers, signal: this.signal };
        if (this._body !== undefined) return new G.Request(this.url, Object.assign({}, init, { body: this._body }));
        if (!isStream(this._stream)) return new G.Request(this.url, init);
        const [mine, theirs] = this._stream.tee();
        this._stream = mine;
        const r = new G.Request(this.url, init);
        r._stream = theirs;
        return r;
      }
    };
  }
  // ---- crypto (getRandomValues/randomUUID/randomBytes; real via __mbunCryptoNative) ----
  if (typeof G.crypto === "undefined" || typeof G.crypto.randomUUID === "undefined") {
    const CN0 = G.__mbunCryptoNative;
    const rand = () => Math.floor(Math.random() * 256);
    const cr = G.crypto || (G.crypto = {});
    if (!cr.getRandomValues) cr.getRandomValues = (arr) => {
      if (CN0 && arr && ArrayBuffer.isView(arr)) { CN0.randomFillSync(arr instanceof Uint8Array ? arr : new Uint8Array(arr.buffer, arr.byteOffset, arr.byteLength)); return arr; }
      for (let i = 0; i < arr.length; i++) arr[i] = rand(); return arr;
    };
    if (!cr.randomUUID) cr.randomUUID = () => {
      if (CN0) return CN0.randomUUID();
      const h = []; for (let i = 0; i < 16; i++) h.push(rand()); h[6] = (h[6] & 0x0f) | 0x40; h[8] = (h[8] & 0x3f) | 0x80;
      const s = h.map((b) => b.toString(16).padStart(2, "0")).join("");
      return s.slice(0, 8) + "-" + s.slice(8, 12) + "-" + s.slice(12, 16) + "-" + s.slice(16, 20) + "-" + s.slice(20);
    };
  }
  {
    const CN = G.__mbunCryptoNative;   // native mbun.crypto backend (hash/hmac/pbkdf2/random)
    // SECURITY: FAIL CLOSED. This used to fall back to Math.random() when the
    // native CSPRNG binding was absent, which silently downgraded
    // crypto.randomBytes / randomUUID / randomInt / generateKey to a
    // non-cryptographic PRNG with no error anywhere — key material an auditor would
    // read as CSPRNG-derived. The fallback is unreachable in a correctly linked
    // build, but "unreachable" is exactly the assumption a silently-unbound
    // partition breaks (see .agents/skills/mbun-runtime-debugging: an IIFE-scope
    // mistake makes a jsc binding vanish without an error). An absent CSPRNG must
    // be an exception, never weaker randomness.
    const rb = (n) => {
      if (CN) return Buffer.from(CN.randomBytes(n));
      const e = new Error("No secure random number generator available");
      e.code = "ERR_CRYPTO_OPERATION_FAILED";
      throw e;
    };
    // JS digest fallbacks (only reached if the native backend is absent).
    // Real digests (SHA-256/SHA-1/MD5) implemented in JS (verified vs known vectors).
    const pad64 = (msg, lenLE) => {
      const l = msg.length, bitLen = l * 8, size = (((l + 8) >> 6) + 1) << 6;
      const buf = new Uint8Array(size); buf.set(msg); buf[l] = 0x80;
      const dv = new DataView(buf.buffer);
      if (lenLE) { dv.setUint32(size - 8, bitLen >>> 0, true); dv.setUint32(size - 4, Math.floor(bitLen / 0x100000000), true); }
      else { dv.setUint32(size - 4, bitLen >>> 0, false); dv.setUint32(size - 8, Math.floor(bitLen / 0x100000000), false); }
      return { dv, size };
    };
    const rotr = (n, x) => (x >>> n) | (x << (32 - n));
    const rol = (x, n) => ((x << n) | (x >>> (32 - n))) >>> 0;
    const kSHA256 = new Uint32Array([0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2]);
    // SHA-256 core parameterized by the initial hash H0 and output byte count, so
    // SHA-224 (different IV, 28-byte truncation) reuses the same compression fn.
    function sha256core(msg, H0, outBytes) {
      const H = Uint32Array.from(H0);
      const { dv, size } = pad64(msg, false), w = new Uint32Array(64);
      for (let off = 0; off < size; off += 64) {
        for (let i = 0; i < 16; i++) w[i] = dv.getUint32(off + i * 4, false);
        for (let i = 16; i < 64; i++) { const s0 = rotr(7, w[i-15]) ^ rotr(18, w[i-15]) ^ (w[i-15] >>> 3); const s1 = rotr(17, w[i-2]) ^ rotr(19, w[i-2]) ^ (w[i-2] >>> 10); w[i] = (w[i-16] + s0 + w[i-7] + s1) >>> 0; }
        let a=H[0],b=H[1],c=H[2],d=H[3],e=H[4],f=H[5],g=H[6],h=H[7];
        for (let i = 0; i < 64; i++) { const S1 = rotr(6,e) ^ rotr(11,e) ^ rotr(25,e); const ch = (e & f) ^ (~e & g); const t1 = (h + S1 + ch + kSHA256[i] + w[i]) >>> 0; const S0 = rotr(2,a) ^ rotr(13,a) ^ rotr(22,a); const maj = (a & b) ^ (a & c) ^ (b & c); const t2 = (S0 + maj) >>> 0; h=g;g=f;f=e;e=(d+t1)>>>0;d=c;c=b;b=a;a=(t1+t2)>>>0; }
        H[0]=(H[0]+a)>>>0;H[1]=(H[1]+b)>>>0;H[2]=(H[2]+c)>>>0;H[3]=(H[3]+d)>>>0;H[4]=(H[4]+e)>>>0;H[5]=(H[5]+f)>>>0;H[6]=(H[6]+g)>>>0;H[7]=(H[7]+h)>>>0;
      }
      const words = outBytes / 4, out = new Uint8Array(outBytes), odv = new DataView(out.buffer);
      for (let i = 0; i < words; i++) odv.setUint32(i * 4, H[i], false);
      return out;
    }
    function sha256bytes(msg) { return sha256core(msg, [0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19], 32); }
    function sha224bytes(msg) { return sha256core(msg, [0xc1059ed8,0x367cd507,0x3070dd17,0xf70e5939,0xffc00b31,0x68581511,0x64f98fa7,0xbefa4fa4], 28); }
    // SHA-512/384 over 64-bit words held as [hi, lo] 32-bit pairs.
    const kSHA512 = [[0x428a2f98,0xd728ae22],[0x71374491,0x23ef65cd],[0xb5c0fbcf,0xec4d3b2f],[0xe9b5dba5,0x8189dbbc],[0x3956c25b,0xf348b538],[0x59f111f1,0xb605d019],[0x923f82a4,0xaf194f9b],[0xab1c5ed5,0xda6d8118],[0xd807aa98,0xa3030242],[0x12835b01,0x45706fbe],[0x243185be,0x4ee4b28c],[0x550c7dc3,0xd5ffb4e2],[0x72be5d74,0xf27b896f],[0x80deb1fe,0x3b1696b1],[0x9bdc06a7,0x25c71235],[0xc19bf174,0xcf692694],[0xe49b69c1,0x9ef14ad2],[0xefbe4786,0x384f25e3],[0x0fc19dc6,0x8b8cd5b5],[0x240ca1cc,0x77ac9c65],[0x2de92c6f,0x592b0275],[0x4a7484aa,0x6ea6e483],[0x5cb0a9dc,0xbd41fbd4],[0x76f988da,0x831153b5],[0x983e5152,0xee66dfab],[0xa831c66d,0x2db43210],[0xb00327c8,0x98fb213f],[0xbf597fc7,0xbeef0ee4],[0xc6e00bf3,0x3da88fc2],[0xd5a79147,0x930aa725],[0x06ca6351,0xe003826f],[0x14292967,0x0a0e6e70],[0x27b70a85,0x46d22ffc],[0x2e1b2138,0x5c26c926],[0x4d2c6dfc,0x5ac42aed],[0x53380d13,0x9d95b3df],[0x650a7354,0x8baf63de],[0x766a0abb,0x3c77b2a8],[0x81c2c92e,0x47edaee6],[0x92722c85,0x1482353b],[0xa2bfe8a1,0x4cf10364],[0xa81a664b,0xbc423001],[0xc24b8b70,0xd0f89791],[0xc76c51a3,0x0654be30],[0xd192e819,0xd6ef5218],[0xd6990624,0x5565a910],[0xf40e3585,0x5771202a],[0x106aa070,0x32bbd1b8],[0x19a4c116,0xb8d2d0c8],[0x1e376c08,0x5141ab53],[0x2748774c,0xdf8eeb99],[0x34b0bcb5,0xe19b48a8],[0x391c0cb3,0xc5c95a63],[0x4ed8aa4a,0xe3418acb],[0x5b9cca4f,0x7763e373],[0x682e6ff3,0xd6b2b8a3],[0x748f82ee,0x5defb2fc],[0x78a5636f,0x43172f60],[0x84c87814,0xa1f0ab72],[0x8cc70208,0x1a6439ec],[0x90befffa,0x23631e28],[0xa4506ceb,0xde82bde9],[0xbef9a3f7,0xb2c67915],[0xc67178f2,0xe372532b],[0xca273ece,0xea26619c],[0xd186b8c7,0x21c0c207],[0xeada7dd6,0xcde0eb1e],[0xf57d4f7f,0xee6ed178],[0x06f067aa,0x72176fba],[0x0a637dc5,0xa2c898a6],[0x113f9804,0xbef90dae],[0x1b710b35,0x131c471b],[0x28db77f5,0x23047d84],[0x32caab7b,0x40c72493],[0x3c9ebe0a,0x15c9bebc],[0x431d67c4,0x9c100d4c],[0x4cc5d4be,0xcb3e42b6],[0x597f299c,0xfc657e2a],[0x5fcb6fab,0x3ad6faec],[0x6c44198c,0x4a475817]];
    function sha512core(msg, H0, outBytes) {
      // helpers on [hi,lo]
      const add = (x, y) => { const l = (x[1] >>> 0) + (y[1] >>> 0); const h = (x[0] + y[0] + (l > 0xffffffff ? 1 : 0)) >>> 0; return [h, l >>> 0]; };
      const xor = (x, y) => [(x[0] ^ y[0]) >>> 0, (x[1] ^ y[1]) >>> 0];
      const and = (x, y) => [(x[0] & y[0]) >>> 0, (x[1] & y[1]) >>> 0];
      const not = (x) => [(~x[0]) >>> 0, (~x[1]) >>> 0];
      const rotr = (x, n) => { if (n === 32) return [x[1], x[0]]; if (n < 32) return [((x[0] >>> n) | (x[1] << (32 - n))) >>> 0, ((x[1] >>> n) | (x[0] << (32 - n))) >>> 0]; n -= 32; return [((x[1] >>> n) | (x[0] << (32 - n))) >>> 0, ((x[0] >>> n) | (x[1] << (32 - n))) >>> 0]; };
      const shr = (x, n) => { if (n < 32) return [x[0] >>> n, ((x[1] >>> n) | (x[0] << (32 - n))) >>> 0]; return [0, x[0] >>> (n - 32)]; };
      // pad to 128-byte blocks, 128-bit length (we only fill low 64 bits)
      const l = msg.length, bitLen = l * 8, size = (((l + 16) >> 7) + 1) << 7;
      const buf = new Uint8Array(size); buf.set(msg); buf[l] = 0x80;
      const bdv = new DataView(buf.buffer);
      bdv.setUint32(size - 4, bitLen >>> 0, false); bdv.setUint32(size - 8, Math.floor(bitLen / 0x100000000), false);
      const H = H0.map((p) => [p[0], p[1]]); const w = new Array(80);
      for (let off = 0; off < size; off += 128) {
        for (let i = 0; i < 16; i++) w[i] = [bdv.getUint32(off + i * 8, false), bdv.getUint32(off + i * 8 + 4, false)];
        for (let i = 16; i < 80; i++) { const s0 = xor(xor(rotr(w[i-15], 1), rotr(w[i-15], 8)), shr(w[i-15], 7)); const s1 = xor(xor(rotr(w[i-2], 19), rotr(w[i-2], 61)), shr(w[i-2], 6)); w[i] = add(add(add(w[i-16], s0), w[i-7]), s1); }
        let a=H[0],b=H[1],c=H[2],d=H[3],e=H[4],f=H[5],g=H[6],h=H[7];
        for (let i = 0; i < 80; i++) { const S1 = xor(xor(rotr(e, 14), rotr(e, 18)), rotr(e, 41)); const ch = xor(and(e, f), and(not(e), g)); const t1 = add(add(add(add(h, S1), ch), kSHA512[i]), w[i]); const S0 = xor(xor(rotr(a, 28), rotr(a, 34)), rotr(a, 39)); const maj = xor(xor(and(a, b), and(a, c)), and(b, c)); const t2 = add(S0, maj); h=g;g=f;f=e;e=add(d,t1);d=c;c=b;b=a;a=add(t1,t2); }
        H[0]=add(H[0],a);H[1]=add(H[1],b);H[2]=add(H[2],c);H[3]=add(H[3],d);H[4]=add(H[4],e);H[5]=add(H[5],f);H[6]=add(H[6],g);H[7]=add(H[7],h);
      }
      const out = new Uint8Array(64), odv = new DataView(out.buffer);
      for (let i = 0; i < 8; i++) { odv.setUint32(i * 8, H[i][0], false); odv.setUint32(i * 8 + 4, H[i][1], false); }
      return out.subarray(0, outBytes);
    }
    function sha512bytes(msg) { return sha512core(msg, [[0x6a09e667,0xf3bcc908],[0xbb67ae85,0x84caa73b],[0x3c6ef372,0xfe94f82b],[0xa54ff53a,0x5f1d36f1],[0x510e527f,0xade682d1],[0x9b05688c,0x2b3e6c1f],[0x1f83d9ab,0xfb41bd6b],[0x5be0cd19,0x137e2179]], 64); }
    function sha384bytes(msg) { return sha512core(msg, [[0xcbbb9d5d,0xc1059ed8],[0x629a292a,0x367cd507],[0x9159015a,0x3070dd17],[0x152fecd8,0xf70e5939],[0x67332667,0xffc00b31],[0x8eb44a87,0x68581511],[0xdb0c2e0d,0x64f98fa7],[0x47b5481d,0xbefa4fa4]], 48); }
    function sha1bytes(msg) {
      const H = new Uint32Array([0x67452301,0xEFCDAB89,0x98BADCFE,0x10325476,0xC3D2E1F0]);
      const { dv, size } = pad64(msg, false), w = new Uint32Array(80);
      for (let off = 0; off < size; off += 64) {
        for (let i = 0; i < 16; i++) w[i] = dv.getUint32(off + i * 4, false);
        for (let i = 16; i < 80; i++) w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        let a=H[0],b=H[1],c=H[2],d=H[3],e=H[4];
        for (let i = 0; i < 80; i++) { let f, k; if (i<20){f=(b&c)|(~b&d);k=0x5A827999;} else if(i<40){f=b^c^d;k=0x6ED9EBA1;} else if(i<60){f=(b&c)|(b&d)|(c&d);k=0x8F1BBCDC;} else {f=b^c^d;k=0xCA62C1D6;} const t=(rol(a,5)+f+e+k+w[i])>>>0; e=d;d=c;c=rol(b,30);b=a;a=t; }
        H[0]=(H[0]+a)>>>0;H[1]=(H[1]+b)>>>0;H[2]=(H[2]+c)>>>0;H[3]=(H[3]+d)>>>0;H[4]=(H[4]+e)>>>0;
      }
      const out = new Uint8Array(20), odv = new DataView(out.buffer);
      for (let i = 0; i < 5; i++) odv.setUint32(i * 4, H[i], false);
      return out;
    }
    const MDK = new Uint32Array(64), MDS = [7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21];
    for (let i = 0; i < 64; i++) MDK[i] = Math.floor(Math.abs(Math.sin(i + 1)) * 0x100000000) >>> 0;
    function md5bytes(msg) {
      let a0=0x67452301,b0=0xefcdab89,c0=0x98badcfe,d0=0x10325476;
      const { dv, size } = pad64(msg, true), M = new Uint32Array(16);
      for (let off = 0; off < size; off += 64) {
        for (let i = 0; i < 16; i++) M[i] = dv.getUint32(off + i * 4, true);
        let A=a0,B=b0,C=c0,D=d0;
        for (let i = 0; i < 64; i++) { let F, g; if (i<16){F=(B&C)|(~B&D);g=i;} else if(i<32){F=(D&B)|(~D&C);g=(5*i+1)%16;} else if(i<48){F=B^C^D;g=(3*i+5)%16;} else {F=C^(B|~D);g=(7*i)%16;} F=(F+A+MDK[i]+M[g])>>>0; A=D;D=C;C=B;B=(B+rol(F,MDS[i]))>>>0; }
        a0=(a0+A)>>>0;b0=(b0+B)>>>0;c0=(c0+C)>>>0;d0=(d0+D)>>>0;
      }
      const out = new Uint8Array(16), odv = new DataView(out.buffer);
      odv.setUint32(0,a0,true);odv.setUint32(4,b0,true);odv.setUint32(8,c0,true);odv.setUint32(12,d0,true);
      return out;
    }
    const hashFns = { sha256: sha256bytes, sha224: sha224bytes, sha384: sha384bytes, sha512: sha512bytes, sha1: sha1bytes, md5: md5bytes };
    // Algorithms served by the native mbun.crypto backend, with digest byte length.
    const NATIVE_ALGOS = { md4:16, md5:16, sha1:20, sha128:20, sha224:28, sha256:32, sha384:48, sha512:64, sha512224:28, sha512256:32, ripemd160:20, rmd160:20, sha3224:28, sha3256:32, sha3384:48, sha3512:64, shake128:16, shake256:32, blake2b512:64, blake2b256:32, blake2s256:32 };
    const NORM = (a) => String(a).toLowerCase().replace(/[-_/.]/g, "");
    // OpenSSL signature-OID spellings that name a plain digest: `RSA-SHA256` is the
    // "rsaEncryption with SHA-256" object, and EVP_get_digestbyname resolves it to
    // SHA-256 itself. The `*WithRSAEncryption` long names are NOT resolvable (node
    // rejects them), so only the short `rsa-*` forms alias here.
    // ref node test/parallel + bun test/js/node/crypto/node-crypto.test.js algo table.
    const DIGEST_ALIASES = { rsasha1: "sha1", rsasha224: "sha224", rsasha256: "sha256", rsasha384: "sha384", rsasha512: "sha512" };
    // MD5-SHA1 (TLS 1.0/1.1 PRF digest) is literally MD5(m) || SHA1(m); OpenSSL
    // ships it as one EVP_MD, mbun composes it from the two native digests.
    const isMd5Sha1 = (algo) => NORM(algo) === "md5sha1";
    const canonAlgo = (algo) => DIGEST_ALIASES[NORM(algo)] || algo;
    const supported = (algo) => { const a = NORM(canonAlgo(algo)); return a === "md5sha1" || (CN && (a in NATIVE_ALGOS)) || (!!hashFns[a]); };
    // One-shot digest over already-joined message bytes, honouring the composite.
    const digestBytes = (algo, m, outLen) => {
      if (isMd5Sha1(algo)) {
        const a = CN ? CN.digest("md5", m, 0) : hashFns.md5(m);
        const b = CN ? CN.digest("sha1", m, 0) : hashFns.sha1(m);
        const out = new Uint8Array(a.length + b.length); out.set(a, 0); out.set(b, a.length); return out;
      }
      const c = canonAlgo(algo);
      return CN ? CN.digest(c, m, outLen) : hashFns[NORM(c)](m);
    };
    const toBytes = (data, enc) => {
      if (typeof data === "string") return (enc && enc !== "utf8" && enc !== "utf-8") ? new Uint8Array(Buffer.from(data, enc)) : te.encode(data);
      if (data instanceof Uint8Array) return data;
      if (ArrayBuffer.isView(data)) return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
      if (data instanceof ArrayBuffer) return new Uint8Array(data);
      if (typeof SharedArrayBuffer !== "undefined" && data instanceof SharedArrayBuffer) return new Uint8Array(data);
      if (data && data.type === "secret" && typeof data.export === "function") return toBytes(data.export());
      return te.encode(String(data));
    };
    const encode = (d, enc) => { if (!enc || enc === "buffer") return Buffer.from(d); if (enc === "hex") { let s = ""; for (const b of d) s += b.toString(16).padStart(2, "0"); return s; } if (enc === "base64") { let bin = ""; for (const b of d) bin += String.fromCharCode(b); return G.btoa(bin); } return Buffer.from(d).toString(enc); };
    // Hash/Hmac are function-style (callable with or without `new`, like node) and
    // real stream.Transform subclasses (node's LazyHash quirk: instanceof Transform
    // === true, and they can be piped). Digests come from the native mbun.crypto
    // backend when present, else the pure-JS fallbacks above.
    // `Transform` is lexically the bootstrap load-order stub (this partition is
    // inside the master builtins IIFE); the real node:stream Transform is only
    // registered later, by the node_stream_* partitions. Resolve it lazily so
    // Hash/Hmac are genuine Transform subclasses at construction time — node's
    // LazyHash quirk (hash instanceof Transform) and _transform/_flush below
    // both depend on it. Falls back to the stub if node:stream is unavailable.
    const streamTransform = () => {
      const s = M["stream"] || M["node:stream"];
      return (s && s.Transform) || Transform;
    };
    function Hash(algo, opts) {
      const self = Reflect.construct(streamTransform(), [], Hash);
      self._algo = algo; self._fn = hashFns[NORM(algo)];
      self._out = opts && typeof opts.outputLength === "number" ? opts.outputLength : -1;  // -1 = native default (XOF); 0 = explicit empty
      if (NORM(algo).startsWith("shake") && self._out < 0 && G.process &&
          typeof G.process.emitWarning === "function") {
        G.process.emitWarning(
          "Creating SHAKE128/256 digests without an explicit options.outputLength is deprecated.",
          "DeprecationWarning", "DEP0198");
      }
      self._chunks = []; self._done = false;
      return self;
    }
    Object.setPrototypeOf(Hash.prototype, Transform.prototype);
    Object.setPrototypeOf(Hash, Transform);
    const joinChunks = function (chunks) { let t = 0; for (const c of chunks) t += c.length; const m = new Uint8Array(t); let o = 0; for (const c of chunks) { m.set(c, o); o += c.length; } return m; };
    Hash.prototype.update = function (data, enc) { if (this._done) throw new Error("Digest already called"); if (typeof data !== "string" && !ArrayBuffer.isView(data) && !(data instanceof ArrayBuffer)) throw mkErr(TypeError, "ERR_INVALID_ARG_TYPE", 'The "data" argument must be of type string or an instance of Buffer, TypedArray, or DataView.' + invalidArgType(data)); this._chunks.push(toBytes(data, enc)); return this; };
    // node's native hash keeps the finalized digest around: the stream path
    // finalizes through the handle (bypassing the JS "already called" guard), and
    // user code may still call digest() afterwards and must get the same bytes
    // back rather than a throw or a recomputation (nodejs/node#28245).
    Hash.prototype._rawDigest = function () {
      if (this._digestBytes === undefined) {
        const m = joinChunks(this._chunks);
        const isXof = NORM(this._algo).startsWith("shake");
        let d;
        if (isXof) {
          d = this._out === 0 ? new Uint8Array(0) : digestBytes(this._algo, m, this._out < 0 ? 0 : this._out);
        } else {
          d = digestBytes(this._algo, m, 0);
          // node: a non-XOF digest rejects an explicit outputLength that isn't its
          // natural length ("Output length N is invalid for <algo>...").
          if (this._out >= 0 && this._out !== d.length) throw new Error("Output length " + this._out + " is invalid for " + this._algo + ", which does not support XOF");
        }
        this._digestBytes = d;
      }
      return this._digestBytes;
    };
    Hash.prototype.digest = function (enc) {
      if (this._done) throw new Error("Digest already called");
      this._done = true;
      return encode(this._rawDigest(), enc);
    };
    Hash.prototype._transform = function (chunk, e, cb) { this.update(chunk); cb(); };
    // Finalize through _rawDigest, not digest(): piping must not arm the
    // "Digest already called" guard against a later digest() call.
    Hash.prototype._flush = function (cb) { this.push(encode(this._rawDigest())); cb(); };
    // node's Hash#copy clones the EVP context, which is gone once digest() ran:
    // copying a finalized hash throws, exactly like update() does.
    Hash.prototype.copy = function () { if (this._done) throw new Error("Digest already called"); const h = new Hash(this._algo, { outputLength: this._out }); h._chunks = this._chunks.slice(); return h; };
    // node exposes the native context under a `kHandle` symbol whose methods must
    // reject a bad `this` with ERR_INVALID_THIS (rather than dereferencing a null
    // native pointer). We mirror that contract with a guarded handle object.
    const kHandle = Symbol("kHandle");
    const invalidThis = (type) => mkErr(TypeError, "ERR_INVALID_THIS", 'Value of "this" must be of type ' + type);
    function NativeHmacHandle(owner) { this._owner = owner; }
    NativeHmacHandle.prototype.update = function (data, enc) {
      if (!(this instanceof NativeHmacHandle)) throw invalidThis("Hmac");
      this._owner.update(data, enc); return this;
    };
    NativeHmacHandle.prototype.digest = function (enc) {
      if (!(this instanceof NativeHmacHandle)) throw invalidThis("Hmac");
      return this._owner.digest(enc);
    };
    function Hmac(algo, key, opts) {
      const self = Reflect.construct(streamTransform(), [], Hmac);
      if (!supported(algo)) throw new Error("Invalid digest: " + algo);
      self._algo = algo; self._key = toBytes(key); self._chunks = []; self._done = false;
      self[kHandle] = new NativeHmacHandle(self);
      return self;
    }
    Object.setPrototypeOf(Hmac.prototype, Transform.prototype);
    Object.setPrototypeOf(Hmac, Transform);
    Hmac.prototype.update = function (data, enc) { if (this._done) throw new Error("Digest already called"); this._chunks.push(toBytes(data, enc)); return this; };
    Hmac.prototype.digest = function (enc) {
      if (this._done) return encode(new Uint8Array(0), enc);   // node resets ctx: re-digest yields empty
      this._done = true; const m = joinChunks(this._chunks);
      let d;
      if (CN) { d = CN.hmac(this._algo, this._key, m); }
      else { const fn = hashFns[NORM(this._algo)]; let k = this._key; if (k.length > 64) k = fn(k); const k0 = new Uint8Array(64); k0.set(k); const ip = new Uint8Array(64), op = new Uint8Array(64); for (let i = 0; i < 64; i++) { ip[i] = k0[i] ^ 0x36; op[i] = k0[i] ^ 0x5c; } const inner = new Uint8Array(64 + m.length); inner.set(ip); inner.set(m, 64); const ih = fn(inner); const outer = new Uint8Array(64 + ih.length); outer.set(op); outer.set(ih, 64); d = fn(outer); }
      return encode(d, enc);
    };
    Hmac.prototype._transform = function (chunk, e, cb) { this.update(chunk); cb(); };
    Hmac.prototype._flush = function (cb) { this.push(this.digest()); cb(); };
    function createHash(algo, opts) { if (typeof algo !== "string") throw new TypeError('The "algorithm" argument must be of type string. Received ' + (algo === null ? "null" : typeof algo)); if (!supported(algo)) throw new Error("Digest method not supported"); return new Hash(algo, opts); }
    // node prepareSecretKey(): the key must be a string, a BufferSource, a
    // *branded* KeyObject, or a CryptoKey. It used to reject only null/undefined,
    // so an arbitrary object — including one wearing KeyObject.prototype with no
    // key in it — was accepted and silently MAC'd as empty bytes.
    const invalidArgTypeRecv = (v) => (v === null ? "null"
      : v === undefined ? "undefined"
      : typeof v === "object" ? "an instance of " + ((v.constructor && v.constructor.name) || "Object")
      : "type " + typeof v + " (" + String(v) + ")");
    const validHmacKey = (key) => {
      if (typeof key === "string") return true;
      if (key === null || typeof key !== "object") return false;
      if (ArrayBuffer.isView(key) || key instanceof ArrayBuffer) return true;
      if (typeof SharedArrayBuffer === "function" && key instanceof SharedArrayBuffer) return true;
      if (typeof G.__mbunIsKeyObject === "function" && G.__mbunIsKeyObject(key)) return true;
      if (typeof G.__mbunIsCryptoKey === "function" && G.__mbunIsCryptoKey(key)) return true;
      return false;
    };
    function createHmac(algo, key, opts) { if (typeof algo !== "string") throw new TypeError('The "hmac" argument must be of type string. Received ' + (algo === null ? "null" : typeof algo)); if (!supported(algo)) throw new Error("Invalid digest: " + algo); if (!validHmacKey(key)) throw mkErr(TypeError, "ERR_INVALID_ARG_TYPE", 'The "key" argument must be of type string or an instance of ArrayBuffer, Buffer, TypedArray, DataView, KeyObject, or CryptoKey. Received ' + invalidArgTypeRecv(key)); return new Hmac(algo, key, opts); }
    // node-style error helpers (message + .code, matching node:crypto).
    const mkErr = (Ctor, code, msg) => { const e = new Ctor(msg); e.code = code; return e; };
    const invalidArgType = (input) => {
      if (input == null) return " Received " + input;
      if (typeof input === "function") return " Received function " + input.name;
      if (typeof input === "object") { const n = input.constructor && input.constructor.name; return n ? " Received an instance of " + n : " Received " + String(input); }
      if (typeof input === "string") { let s = input; if (s.length > 28) s = s.slice(0, 25) + "..."; return s.indexOf("'") === -1 ? " Received type string ('" + s + "')" : " Received type string (" + JSON.stringify(s) + ")"; }
      return " Received type " + typeof input + " (" + String(input) + ")";
    };
    const isDataInput = (d) => typeof d === "string" || d instanceof Uint8Array || ArrayBuffer.isView(d) || d instanceof ArrayBuffer;
    // crypto.hash(algorithm, data[, outputEncoding]) — one-shot digest (default hex).
    const cryptoHash = (algo, data, outputEncoding) => {
      if (typeof algo !== "string") throw mkErr(TypeError, "ERR_INVALID_ARG_TYPE", 'The "algorithm" argument must be of type string.' + invalidArgType(algo));
      if (!isDataInput(data)) throw mkErr(TypeError, "ERR_INVALID_ARG_TYPE", 'The "data" argument must be of type string or an instance of Buffer, TypedArray, or DataView.' + invalidArgType(data));
      // node crypto.hash(algorithm, data[, outputEncoding|options]) — the 3rd arg
      // is either an output-encoding string or an options object carrying
      // { outputEncoding, outputLength } (XOF digest length).
      let enc, outLen;
      if (outputEncoding !== undefined && outputEncoding !== null && typeof outputEncoding === "object") {
        const oe = outputEncoding.outputEncoding;
        // An options object that carries neither knob is not an options object,
        // it is a bad outputEncoding: bun rejects any non-string third argument,
        // while node's XOF form only ever passes outputEncoding/outputLength.
        if (oe === undefined && outputEncoding.outputLength === undefined)
          throw mkErr(TypeError, "ERR_INVALID_ARG_TYPE", 'The "outputEncoding" argument must be of type string.' + invalidArgType(outputEncoding));
        if (oe !== undefined && typeof oe !== "string") throw mkErr(TypeError, "ERR_INVALID_ARG_TYPE", 'The "options.outputEncoding" argument must be of type string.' + invalidArgType(oe));
        enc = oe === undefined ? "hex" : oe;
        if (typeof outputEncoding.outputLength === "number") outLen = outputEncoding.outputLength;
      } else {
        if (outputEncoding !== undefined && typeof outputEncoding !== "string") throw mkErr(TypeError, "ERR_INVALID_ARG_TYPE", 'The "outputEncoding" argument must be of type string.' + invalidArgType(outputEncoding));
        enc = outputEncoding === undefined ? "hex" : outputEncoding;
      }
      const VALID_ENC = { hex:1, base64:1, base64url:1, buffer:1, latin1:1, binary:1, ascii:1, utf8:1, "utf-8":1, ucs2:1, "ucs-2":1, utf16le:1, "utf-16le":1 };
      if (!(enc in VALID_ENC)) throw mkErr(TypeError, "ERR_INVALID_ARG_VALUE", "The argument 'options.outputEncoding' is invalid. Received " + JSON.stringify(enc));
      return createHash(algo, outLen !== undefined ? { outputLength: outLen } : undefined).update(data).digest(enc);
    };
    // pbkdf2 password/salt input validation (string | ArrayBuffer | TypedArray |
    // DataView). node validates these before iterations/keylen/digest.
    const validatePbkdf2Input = (password, salt) => {
      const okBuf = (v) => typeof v === "string" || ArrayBuffer.isView(v) || v instanceof ArrayBuffer || (typeof SharedArrayBuffer !== "undefined" && v instanceof SharedArrayBuffer);
      if (!okBuf(password)) throw mkErr(TypeError, "ERR_INVALID_ARG_TYPE", 'The "password" argument must be of type string or an instance of ArrayBuffer, Buffer, TypedArray, or DataView.' + invalidArgType(password));
      if (!okBuf(salt)) throw mkErr(TypeError, "ERR_INVALID_ARG_TYPE", 'The "salt" argument must be of type string or an instance of ArrayBuffer, Buffer, TypedArray, or DataView.' + invalidArgType(salt));
    };
    // pbkdf2 shared parameter validation (node error codes/messages).
    const validatePbkdf2 = (iterations, keylen, digest) => {
      if (typeof iterations !== "number") throw mkErr(TypeError, "ERR_INVALID_ARG_TYPE", 'The "iterations" argument must be of type number.' + invalidArgType(iterations));
      if (!Number.isInteger(iterations)) throw mkErr(RangeError, "ERR_OUT_OF_RANGE", 'The value of "iterations" is out of range. It must be an integer. Received ' + iterations);
      if (iterations < 1 || iterations > 2147483647) throw mkErr(RangeError, "ERR_OUT_OF_RANGE", 'The value of "iterations" is out of range. It must be >= 1 && <= 2147483647. Received ' + iterations);
      if (typeof keylen !== "number") throw mkErr(TypeError, "ERR_INVALID_ARG_TYPE", 'The "keylen" argument must be of type number.' + invalidArgType(keylen));
      if (!Number.isInteger(keylen)) throw mkErr(RangeError, "ERR_OUT_OF_RANGE", 'The value of "keylen" is out of range. It must be an integer. Received ' + keylen);
      if (keylen < 0 || keylen > 2147483647) throw mkErr(RangeError, "ERR_OUT_OF_RANGE", 'The value of "keylen" is out of range. It must be >= 0 and <= 2147483647. Received ' + keylen);
      // node requires an explicit string digest (no sha1 default): a missing digest
      // is ERR_INVALID_ARG_TYPE, an unknown one ERR_CRYPTO_INVALID_DIGEST.
      if (typeof digest !== "string") throw mkErr(TypeError, "ERR_INVALID_ARG_TYPE", 'The "digest" argument must be of type string.' + invalidArgType(digest));
      if (!supported(digest)) throw mkErr(Error, "ERR_CRYPTO_INVALID_DIGEST", "Invalid digest: " + digest);
    };
    // hkdf/hkdfSync shared parameter validation (node lib/internal/crypto/hkdf.js).
    // Order (matches node): digest type → ikm type → salt type → info type →
    // info length (<=1024) → length type → length range → digest supported.
    const isAnyAB = (v) => v instanceof ArrayBuffer || (typeof SharedArrayBuffer !== "undefined" && v instanceof SharedArrayBuffer);
    const validateHkdf = (digest, ikm, salt, info, keylen) => {
      if (typeof digest !== "string") throw mkErr(TypeError, "ERR_INVALID_ARG_TYPE", 'The "digest" argument must be of type string.' + invalidArgType(digest));
      const isKO = ikm && typeof ikm === "object" && ikm.type !== undefined && typeof ikm.export === "function";
      if (typeof ikm !== "string" && !ArrayBuffer.isView(ikm) && !isAnyAB(ikm) && !isKO) throw mkErr(TypeError, "ERR_INVALID_ARG_TYPE", 'The "ikm" argument must be of type string or an instance of ArrayBuffer, Buffer, TypedArray, DataView, KeyObject, or CryptoKey.' + invalidArgType(ikm));
      if (typeof salt !== "string" && !ArrayBuffer.isView(salt) && !isAnyAB(salt)) throw mkErr(TypeError, "ERR_INVALID_ARG_TYPE", 'The "salt" argument must be of type string or an instance of ArrayBuffer, Buffer, TypedArray, or DataView.' + invalidArgType(salt));
      if (typeof info !== "string" && !ArrayBuffer.isView(info) && !isAnyAB(info)) throw mkErr(TypeError, "ERR_INVALID_ARG_TYPE", 'The "info" argument must be of type string or an instance of ArrayBuffer, Buffer, TypedArray, or DataView.' + invalidArgType(info));
      if (toBytes(info).length > 1024) throw mkErr(RangeError, "ERR_OUT_OF_RANGE", 'The value of "info" is out of range. It must be <= 1024 bytes. Received ' + toBytes(info).length);
      if (typeof keylen !== "number") throw mkErr(TypeError, "ERR_INVALID_ARG_TYPE", 'The "length" argument must be of type number.' + invalidArgType(keylen));
      if (!Number.isInteger(keylen) || keylen < 0 || keylen > 2147483647) throw mkErr(RangeError, "ERR_OUT_OF_RANGE", 'The value of "length" is out of range. It must be >= 0 && <= 2147483647. Received ' + keylen);
      if (!supported(digest)) throw mkErr(Error, "ERR_CRYPTO_INVALID_DIGEST", "Invalid digest: " + digest);
    };
    const deferCb = (fn) => { (typeof queueMicrotask === "function" ? queueMicrotask : (f) => Promise.resolve().then(f))(fn); };
    // KeyObject instances (real class so instanceof + structured clone work).
    class KeyObject {
      constructor(type, data, asymmetricKeyType) { this.type = type; this._data = data; if (asymmetricKeyType) this.asymmetricKeyType = asymmetricKeyType; }
      export(o) { if (this.type === "secret") return Buffer.from(toBytes(this._data)); return (this._data && this._data.key) || this._data; }
      get [Symbol.toStringTag]() { return "KeyObject"; }
      get symmetricKeySize() { return this.type === "secret" ? toBytes(this._data).length : undefined; }
    }
    // Deterministic Miller-Rabin over the first 12 prime bases: exact for all
    // 64-bit inputs and a vanishingly small error rate beyond, matching what
    // node's checkPrime returns for these inputs.
    const millerRabinPrime = (n) => {
      const SMALL = [2n, 3n, 5n, 7n, 11n, 13n, 17n, 19n, 23n, 29n, 31n, 37n];
      if (n < 2n) return false;
      for (const p of SMALL) { if (n === p) return true; if (n % p === 0n) return false; }
      let d = n - 1n, r = 0n;
      while ((d & 1n) === 0n) { d >>= 1n; r++; }
      const modpow = (b, e, m) => { let res = 1n; b %= m; while (e > 0n) { if (e & 1n) res = (res * b) % m; e >>= 1n; b = (b * b) % m; } return res; };
      for (const a of SMALL) {
        if (a >= n) continue;
        let x = modpow(a, d, n);
        if (x === 1n || x === n - 1n) continue;
        let composite = true;
        for (let i = 1n; i < r; i++) { x = (x * x) % n; if (x === n - 1n) { composite = false; break; } }
        if (composite) return false;
      }
      return true;
    };
    // getDiffieHellman(group) returns a DiffieHellmanGroup. Its `verifyError`
    // getter (like node's) validates `this` and throws ERR_INVALID_THIS on a bad
    // receiver instead of reading a null native field.
    function DiffieHellmanGroup() {}
    DiffieHellmanGroup.prototype.generateKeys = () => Buffer.alloc(0);
    DiffieHellmanGroup.prototype.computeSecret = () => Buffer.alloc(0);
    DiffieHellmanGroup.prototype.getPrime = () => Buffer.alloc(0);
    DiffieHellmanGroup.prototype.getGenerator = () => Buffer.alloc(0);
    DiffieHellmanGroup.prototype.getPublicKey = () => Buffer.alloc(0);
    DiffieHellmanGroup.prototype.getPrivateKey = () => Buffer.alloc(0);
    DiffieHellmanGroup.prototype.setPublicKey = function () {};
    DiffieHellmanGroup.prototype.setPrivateKey = function () {};
    Object.defineProperty(DiffieHellmanGroup.prototype, "verifyError", {
      configurable: true,
      enumerable: true,
      get: function () {
        if (!(this instanceof DiffieHellmanGroup)) throw invalidThis("DiffieHellmanGroup");
        return 0;
      },
    });
    const nodeCrypto = {
      randomUUID: (options) => {
        if (options !== undefined) {
          if (typeof options !== "object" || options === null) throw mkErr(TypeError, "ERR_INVALID_ARG_TYPE", 'The "options" argument must be of type object.' + invalidArgType(options));
          if (options.disableEntropyCache !== undefined && typeof options.disableEntropyCache !== "boolean") throw mkErr(TypeError, "ERR_INVALID_ARG_TYPE", 'The "options.disableEntropyCache" property must be of type boolean.' + invalidArgType(options.disableEntropyCache));
        }
        return G.crypto.randomUUID();
      },
      // crypto.randomBytes(size[, cb]) — sync return, or async when a callback is
      // given (node passes null as the error on success).
      randomBytes: (n, cb) => {
        if (typeof cb === "function") { const b = rb(n); deferCb(() => cb(null, b)); return; }
        return rb(n);
      },
      // crypto.pseudoRandomBytes / prng / rng — node's deprecated aliases, all
      // three literally randomBytes (lib/crypto.js `getRandomBytesAlias`).
      // Still exported, and still called by the corpus (test-domain-crypto).
      pseudoRandomBytes: (n, cb) => {
        if (typeof cb === "function") { const b = rb(n); deferCb(() => cb(null, b)); return; }
        return rb(n);
      },
      prng: (n, cb) => {
        if (typeof cb === "function") { const b = rb(n); deferCb(() => cb(null, b)); return; }
        return rb(n);
      },
      rng: (n, cb) => {
        if (typeof cb === "function") { const b = rb(n); deferCb(() => cb(null, b)); return; }
        return rb(n);
      },
      randomFillSync: (buf, offset, size) => {
        // node scales offset/size by BYTES_PER_ELEMENT for TypedArrays (1 for
        // DataView / ArrayBuffer). Bounds are validated against the byte length so
        // an out-of-range offset+size never writes past the allocation.
        const view = buf instanceof Uint8Array ? buf : (ArrayBuffer.isView(buf) ? new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength) : new Uint8Array(buf));
        const elem = (ArrayBuffer.isView(buf) && buf.BYTES_PER_ELEMENT) ? buf.BYTES_PER_ELEMENT : 1;
        const total = view.length;
        const off = (offset || 0) * elem;
        const len = size == null ? total - off : size * elem;
        if (off < 0 || len < 0 || off + len > total) throw mkErr(RangeError, "ERR_OUT_OF_RANGE", 'The value of "size + offset" is out of range. It must be <= ' + total + '. Received ' + (off + len));
        // Write directly into `view` at [off, off+len): fill a fresh zero-offset
        // buffer (native randomFillSync ignores a view's byteOffset) then copy
        // element-wise. A subarray view can't be used here — it copies rather than
        // aliases in this build, so writes to it would never reach the original.
        const tmp = new Uint8Array(len);
        if (CN) { CN.randomFillSync(tmp); } else { for (let i = 0; i < len; i++) tmp[i] = Math.floor(Math.random() * 256); }
        for (let i = 0; i < len; i++) view[off + i] = tmp[i];
        return buf;
      },
      getRandomValues: (a) => G.crypto.getRandomValues(a),
      // crypto.randomUUIDv7([options]) — RFC 9562 UUIDv7: 48-bit big-endian
      // millisecond timestamp, version 7, variant 10xx, remaining bits random.
      randomUUIDv7: (options) => {
        if (options !== undefined) {
          if (typeof options !== "object" || options === null) throw mkErr(TypeError, "ERR_INVALID_ARG_TYPE", 'The "options" argument must be of type object.' + invalidArgType(options));
          if (options.disableEntropyCache !== undefined && typeof options.disableEntropyCache !== "boolean") throw mkErr(TypeError, "ERR_INVALID_ARG_TYPE", 'The "options.disableEntropyCache" property must be of type boolean.' + invalidArgType(options.disableEntropyCache));
        }
        const ts = Date.now();
        const b = new Uint8Array(16);
        b[0] = Math.floor(ts / 1099511627776) & 0xff;   // ts >> 40
        b[1] = Math.floor(ts / 4294967296) & 0xff;       // ts >> 32
        b[2] = Math.floor(ts / 16777216) & 0xff;         // ts >> 24
        b[3] = Math.floor(ts / 65536) & 0xff;            // ts >> 16
        b[4] = Math.floor(ts / 256) & 0xff;              // ts >> 8
        b[5] = ts & 0xff;
        const r = CN ? CN.randomBytes(10) : rb(10);
        for (let i = 0; i < 10; i++) b[6 + i] = r[i];
        b[6] = (b[6] & 0x0f) | 0x70;                     // version 7
        b[8] = (b[8] & 0x3f) | 0x80;                     // variant 10xx
        let s = "";
        for (let i = 0; i < 16; i++) s += b[i].toString(16).padStart(2, "0");
        return s.slice(0, 8) + "-" + s.slice(8, 12) + "-" + s.slice(12, 16) + "-" + s.slice(16, 20) + "-" + s.slice(20);
      },
      createHash, createHmac, Hash, Hmac,
      getHashes: () => ["md5", "sha1", "sha224", "sha256", "sha384", "sha512", "sha512-256", "sha3-224", "sha3-256", "sha3-384", "sha3-512", "shake128", "shake256", "blake2b512", "blake2b256", "blake2s256"],
      // Real PBKDF2 (RFC 2898): native mbun.crypto for supported PRFs, JS otherwise.
      pbkdf2Sync: (password, salt, iterations, keylen, digest) => {
        validatePbkdf2Input(password, salt);
        validatePbkdf2(iterations, keylen, digest);
        if (keylen === 0) throw new Error("PBKDF2 derivation failed");
        const dg = digest || "sha1";
        if (CN && (NORM(dg) in { md5:1, sha1:1, sha224:1, sha256:1, sha384:1, sha512:1, sha512256:1 })) {
          return Buffer.from(CN.pbkdf2(toBytes(password), toBytes(salt), iterations, keylen, dg));
        }
        const hLen = { sha1: 20, sha224: 28, sha256: 32, sha384: 48, sha512: 64, sha512224: 28, sha512256: 32, md5: 16, md4: 16, ripemd160: 20, rmd160: 20, sha3224: 28, sha3256: 32, sha3384: 48, sha3512: 64 }[String(digest || "sha1").toLowerCase().replace(/[-_/.]/g, "")] || 20;
        const pw = toBytes(password), sb = toBytes(salt), blocks = Math.ceil(keylen / hLen), out = new Uint8Array(blocks * hLen);
        for (let b = 1; b <= blocks; b++) {
          const bi = new Uint8Array(sb.length + 4); bi.set(sb); bi[sb.length] = (b >>> 24) & 0xff; bi[sb.length + 1] = (b >>> 16) & 0xff; bi[sb.length + 2] = (b >>> 8) & 0xff; bi[sb.length + 3] = b & 0xff;
          let u = createHmac(digest || "sha1", pw).update(bi).digest(); const t = new Uint8Array(u);
          for (let i = 1; i < iterations; i++) { u = createHmac(digest || "sha1", pw).update(u).digest(); for (let j = 0; j < hLen; j++) t[j] ^= u[j]; }
          out.set(t, (b - 1) * hLen);
        }
        return Buffer.from(out.subarray(0, keylen));
      },
      // HKDF (RFC 5869): extract PRK=HMAC(salt,ikm) then expand. Returns an
      // ArrayBuffer. ref: bun crypto.ts getArrayBufferOrView (secret KeyObject
      // check) + ncrypto.cpp HKDF.
      hkdfSync: (digest, ikm, salt, info, keylen) => {
        validateHkdf(digest, ikm, salt, info, keylen);
        if (ikm && typeof ikm === "object" && ikm.type !== undefined && typeof ikm.export === "function" && ikm.type !== "secret") {
          const e = new TypeError("Invalid key object type " + ikm.type + ", expected secret.");
          e.code = "ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE"; throw e;
        }
        const ikmB = toBytes(ikm), saltB = toBytes(salt), infoB = toBytes(info);
        const prk = new Uint8Array(createHmac(digest, saltB).update(ikmB).digest());
        const hashLen = prk.length;
        const n = Math.ceil(keylen / hashLen);
        if (n > 255) { const e = new RangeError("Invalid key length"); e.code = "ERR_CRYPTO_INVALID_KEYLEN"; throw e; }
        const okm = new Uint8Array(n * hashLen);
        let prev = new Uint8Array(0);
        for (let i = 1; i <= n; i++) {
          const inp = new Uint8Array(prev.length + infoB.length + 1);
          inp.set(prev, 0); inp.set(infoB, prev.length); inp[inp.length - 1] = i;
          prev = new Uint8Array(createHmac(digest, prk).update(inp).digest());
          okm.set(prev, (i - 1) * hashLen);
        }
        return okm.slice(0, keylen).buffer;
      },
      hkdf: (digest, ikm, salt, info, keylen, cb) => {
        // Validation (bad KeyObject / key length) throws synchronously and must
        // NOT reach the callback; success defers cb(null, arrayBuffer).
        const res = nodeCrypto.hkdfSync(digest, ikm, salt, info, keylen);
        queueMicrotask(() => cb(null, res));
      },
      // Async pbkdf2: argument validation throws synchronously (node semantics —
      // a bad keylen/iterations must NOT reach the callback); derivation errors
      // (e.g. keylen 0) surface via the callback.
      pbkdf2: (password, salt, iterations, keylen, digest, cb) => {
        // A function in the `digest` slot is the callback; `digest` is then
        // undefined and validation reports the missing-digest error (node).
        let dg = digest, fn = cb;
        if (typeof digest === "function") { fn = digest; dg = undefined; }
        validatePbkdf2Input(password, salt);
        validatePbkdf2(iterations, keylen, dg);   // synchronous throw on invalid params (incl. missing digest)
        if (typeof fn !== "function") throw mkErr(TypeError, "ERR_INVALID_ARG_TYPE", 'The "callback" argument must be of type function.' + invalidArgType(fn));
        deferCb(() => { try { const r = nodeCrypto.pbkdf2Sync(password, salt, iterations, keylen, dg); fn(null, r); } catch (e) { fn(e); } });
      },
      hash: cryptoHash,
      getCurves: () => ["prime256v1", "secp256k1", "secp384r1", "secp521r1"],
      getCiphers: () => ["aes-128-cbc","aes-128-ctr","aes-128-gcm","aes-128-cfb","aes-128-ofb","aes-128-ecb","aes-192-cbc","aes-192-ctr","aes-192-gcm","aes-256-cbc","aes-256-ctr","aes-256-gcm","aes-256-cfb","aes-256-ofb","aes-256-ecb","chacha20","chacha20-poly1305","des-ede3-cbc","des-ede-cbc","aria-128-gcm","aria-256-gcm","camellia-128-cbc","camellia-256-cbc","sm4-cbc"],
      getDiffieHellman: () => new DiffieHellmanGroup(),
      // crypto.randomInt([min, ]max[, cb]) — uniform integer in [min, max).
      randomInt: (...args) => {
        let cb; if (typeof args[args.length - 1] === "function") cb = args.pop();
        let min = 0, max; if (args.length <= 1) { max = args[0]; } else { min = args[0]; max = args[1]; }
        if (!Number.isSafeInteger(min)) throw mkErr(TypeError, "ERR_INVALID_ARG_TYPE", 'The "min" argument must be a safe integer.' + invalidArgType(min));
        if (!Number.isSafeInteger(max)) throw mkErr(TypeError, "ERR_INVALID_ARG_TYPE", 'The "max" argument must be a safe integer.' + invalidArgType(max));
        if (max <= min) throw mkErr(RangeError, "ERR_OUT_OF_RANGE", 'The value of "max" is out of range. It must be greater than the value of "min".');
        const range = max - min;
        if (range > 281474976710655) throw mkErr(RangeError, "ERR_OUT_OF_RANGE", 'The value of "range" is out of range. It must be <= 281474976710655. Received ' + range);
        const compute = () => { const b = CN ? CN.randomBytes(6) : rb(6); let r = 0; for (let i = 0; i < 6; i++) r = r * 256 + b[i]; return min + (r % range); };
        if (cb) { const v = compute(); deferCb(() => cb(undefined, v)); return; }
        return compute();
      },
      // crypto.randomFill(buf[, offset][, size], cb) — async randomFillSync.
      randomFill: (buf, offset, size, cb) => {
        if (typeof offset === "function") { cb = offset; offset = undefined; size = undefined; }
        else if (typeof size === "function") { cb = size; size = undefined; }
        // Validation (element-scaled bounds) happens synchronously — a bad
        // offset/size throws before the callback is scheduled, matching node.
        nodeCrypto.randomFillSync(buf, offset || 0, size);
        if (cb) deferCb(() => cb(null, buf));
        return buf;
      },
      // crypto.checkPrimeSync(candidate[, options]) / checkPrime(...): probabilistic
      // primality over a big-endian byte candidate. Uses deterministic Miller-Rabin
      // small-prime bases (correct well past the ranges these APIs see). The candidate
      // bytes are snapshotted before options are read so a mutating options getter
      // cannot change which number is tested (node reads the candidate at call time).
      checkPrimeSync: (candidate, options) => {
        const src = toBytes(candidate);
        let n = 0n; for (let i = 0; i < src.length; i++) n = (n << 8n) | BigInt(src[i]);
        if (options) { const _checks = options.checks; void _checks; }  // read getter exactly once (node reads it at call time)
        return millerRabinPrime(n);
      },
      checkPrime: (candidate, options, cb) => {
        if (typeof options === "function") { cb = options; options = undefined; }
        let result, err = null;
        try { result = nodeCrypto.checkPrimeSync(candidate, options); } catch (e) { err = e; }
        deferCb(() => (err ? cb(err) : cb(null, result)));
      },
      // asymmetric crypto (sign/verify/key objects): shape only — real RSA/EC is
      // out of scope; sign/verify honestly throw so nothing fakes a signature.
      createSign: () => { const c = []; return { update(d) { c.push(d); return this; }, sign() { throw new Error("crypto.Sign: asymmetric signing is not implemented yet in mbun"); } }; },
      createVerify: () => ({ update() { return this; }, verify() { throw new Error("crypto.Verify: asymmetric verification is not implemented yet in mbun"); } }),
      createPrivateKey: (k) => new KeyObject("private", k, "rsa"),
      createPublicKey: (k) => new KeyObject("public", k, "rsa"),
      createSecretKey: (k) => new KeyObject("secret", k),
      generateKeyPairSync: () => ({ publicKey: "", privateKey: "" }),
      // Real RSA/EC keygen is out of scope; the pair is empty-string placeholders
      // (same as generateKeyPairSync). Callback form is (err, publicKey, privateKey);
      // the promisify.custom below resolves to { publicKey, privateKey } like node.
      generateKeyPair: (t, o, cb) => { const fn = cb || o; if (typeof fn === "function") fn(null, "", ""); },
      generateKeySync: () => new KeyObject("secret", Buffer.alloc(32)),
      KeyObject,
      X509Certificate: class X509Certificate { constructor() { this.subject = ""; this.issuer = ""; } },
      createECDH: () => ({ generateKeys: () => Buffer.alloc(0), computeSecret: () => Buffer.alloc(0), getPublicKey: () => Buffer.alloc(0), getPrivateKey: () => Buffer.alloc(0), setPrivateKey() {} }),
      createDiffieHellman: () => ({ generateKeys: () => Buffer.alloc(0), computeSecret: () => Buffer.alloc(0), getPrime: () => Buffer.alloc(0), getGenerator: () => Buffer.alloc(0) }),
      // node/bun throw on a length mismatch (ErrorCode.cpp:1552
      // CRYPTO_TIMING_SAFE_EQUAL_LENGTH), they do not return false.
      timingSafeEqual: (a, b) => { const ok = (x) => ArrayBuffer.isView(x) || x instanceof ArrayBuffer; if (!ok(a) || !ok(b)) { const e = new TypeError('The "buf1" argument must be an instance of ArrayBuffer, Buffer, TypedArray, or DataView.'); e.code = "ERR_INVALID_ARG_TYPE"; throw e; } a = toBytes(a); b = toBytes(b); if (a.length !== b.length) { const e = new RangeError("Input buffers must have the same byte length"); e.code = "ERR_CRYPTO_TIMING_SAFE_EQUAL_LENGTH"; throw e; } let d = 0; for (let i = 0; i < a.length; i++) d |= a[i] ^ b[i]; return d === 0; },
      constants: { RSA_PKCS1_PADDING: 1, RSA_PKCS1_OAEP_PADDING: 4 }, webcrypto: G.crypto,
    };
    // node's generateKeyPair has a custom promisify that resolves to an object
    // { publicKey, privateKey } (not the first callback arg). Mirror that so
    // util.promisify(crypto.generateKeyPair)(...) yields the 2-key object.
    nodeCrypto.generateKeyPair[Symbol.for("nodejs.util.promisify.custom")] =
      (type, options) => Promise.resolve({ publicKey: "", privateKey: "" });
    // node:crypto re-exports the WebCrypto SubtleCrypto as `crypto.subtle`
    // (an alias for `crypto.webcrypto.subtle`). It matters beyond the node API:
    // `mbun -e` exposes builtinModules as globals, and `crypto` is deliberately
    // let through (see api_impl.inc kBuiltinGlobals), so inside `-e` the global
    // `crypto` IS this module — without the alias, `crypto.subtle` reads
    // undefined there while it works in a file. The captured reference is used
    // rather than `globalThis.crypto` precisely because of that shadowing.
    const webCryptoGlobal = G.crypto;
    if (webCryptoGlobal && !("subtle" in nodeCrypto)) {
      Object.defineProperty(nodeCrypto, "subtle", {
        configurable: true, enumerable: true,
        get() { return webCryptoGlobal.subtle; },
        set() {},
      });
    }
    def(["crypto"], nodeCrypto);
    // WebCrypto CryptoKey — a real class so instanceof + structured clone work.
    // Only symmetric key generation/export is modeled (AES-*/HMAC raw bits).
    if (typeof G.CryptoKey === "undefined") {
      G.CryptoKey = class CryptoKey {
        constructor(type, algorithm, extractable, usages, raw) { this.type = type; this.algorithm = algorithm; this.extractable = !!extractable; this.usages = usages ? Array.from(usages) : []; this._raw = raw; }
        get [Symbol.toStringTag]() { return "CryptoKey"; }
      };
    }
    if (G.crypto && typeof G.crypto.subtle === "undefined") G.crypto.subtle = { digest: async (algo, data) => createHash(String(algo.name || algo).toLowerCase().replace("-", "")).update(new Uint8Array(data)).digest().buffer };
    if (G.crypto && G.crypto.subtle && typeof G.crypto.subtle.generateKey === "undefined") {
      G.crypto.subtle.generateKey = async (algorithm, extractable, usages) => {
        const a = typeof algorithm === "string" ? { name: algorithm } : Object.assign({}, algorithm);
        const bits = a.length || 256;
        const raw = rb(Math.ceil(bits / 8));
        return new G.CryptoKey("secret", { name: a.name, length: bits }, extractable, usages, new Uint8Array(raw));
      };
      G.crypto.subtle.exportKey = async (format, key) => { if (!key || !key.extractable) throw new Error("key is not extractable"); return key._raw ? key._raw.buffer.slice(key._raw.byteOffset, key._raw.byteOffset + key._raw.byteLength) : new ArrayBuffer(0); };
      G.crypto.subtle.importKey = async (format, data, algorithm, extractable, usages) => { const a = typeof algorithm === "string" ? { name: algorithm } : Object.assign({}, algorithm); return new G.CryptoKey("secret", a, extractable, usages, new Uint8Array(data instanceof ArrayBuffer ? data : data.buffer ? data.slice().buffer : data)); };
    }
    if (G.crypto && typeof G.crypto.createHash === "undefined") { G.crypto.createHash = createHash; G.crypto.createHmac = createHmac; }
    if (G.Bun && typeof G.Bun.hash === "undefined") { G.Bun.hash = (data) => { const d = sha256bytes(toBytes(data)); let n = 0n; for (let i = 0; i < 8; i++) n = (n << 8n) | BigInt(d[i]); return n; }; G.Bun.CryptoHasher = function (algo) { const hh = createHash(algo || "sha256"); this.algorithm = algo || "sha256"; this.update = (d) => { hh.update(d); return this; }; this.digest = (e) => hh.digest(e); }; G.Bun.CryptoHasher.algorithms = ["sha512", "sha384", "sha256", "sha224", "sha1", "md5"]; }
    // Bun.randomUUIDv5(name, namespace, encoding?) — RFC 4122 v5 (SHA-1) UUID.
    if (G.Bun && typeof G.Bun.randomUUIDv5 === "undefined") {
      const WELL_KNOWN_NS = { dns: "6ba7b810-9dad-11d1-80b4-00c04fd430c8", url: "6ba7b811-9dad-11d1-80b4-00c04fd430c8", oid: "6ba7b812-9dad-11d1-80b4-00c04fd430c8", x500: "6ba7b814-9dad-11d1-80b4-00c04fd430c8" };
      const nsBytes = (ns) => {
        // bun randomUUIDv5: a missing namespace is ERR_INVALID_ARG_TYPE, a
        // malformed one ERR_INVALID_ARG_VALUE (both TypeError).
        if (ns === undefined || ns === null) { const e = new TypeError('The "namespace" argument must be a string or an instance of ArrayBuffer, Buffer or TypedArray'); e.code = "ERR_INVALID_ARG_TYPE"; throw e; }
        if (ns instanceof Uint8Array || ArrayBuffer.isView(ns)) { const u = new Uint8Array(ns.buffer, ns.byteOffset, ns.byteLength); if (u.length !== 16) { const e = new TypeError("namespace must be exactly 16 bytes"); e.code = "ERR_INVALID_ARG_VALUE"; throw e; } return u; }
        if (ns instanceof ArrayBuffer) { const u = new Uint8Array(ns); if (u.length !== 16) { const e = new TypeError("namespace must be exactly 16 bytes"); e.code = "ERR_INVALID_ARG_VALUE"; throw e; } return u; }
        let s = String(ns); s = WELL_KNOWN_NS[s.toLowerCase()] || s;
        const hex = s.replace(/-/g, "");
        if (!/^[0-9a-fA-F]{32}$/.test(hex)) { const e = new TypeError("namespace must be a valid UUID string or 16-byte buffer"); e.code = "ERR_INVALID_ARG_VALUE"; throw e; }
        const u = new Uint8Array(16); for (let i = 0; i < 16; i++) u[i] = parseInt(hex.slice(i * 2, i * 2 + 2), 16); return u;
      };
      G.Bun.randomUUIDv5 = (name, namespace, enc) => {
        const ns = nsBytes(namespace);
        const nb = toBytes(typeof name === "string" ? name : (ArrayBuffer.isView(name) || name instanceof ArrayBuffer ? name : String(name)));
        const msg = new Uint8Array(16 + nb.length); msg.set(ns, 0); msg.set(nb instanceof Uint8Array ? nb : new Uint8Array(nb), 16);
        const d = sha1bytes(msg).slice(0, 16);
        d[6] = (d[6] & 0x0f) | 0x50;   // version 5
        d[8] = (d[8] & 0x3f) | 0x80;   // RFC 4122 variant
        if (enc === undefined || enc === "hex") { let s = ""; for (let i = 0; i < 16; i++) { s += d[i].toString(16).padStart(2, "0"); if (i === 3 || i === 5 || i === 7 || i === 9) s += "-"; } return s; }
        if (enc === "buffer") return Buffer.from(d);
        if (enc === "base64" || enc === "base64url") { let bin = ""; for (const b of d) bin += String.fromCharCode(b); const b64 = G.btoa(bin); return enc === "base64" ? b64 : b64.replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, ""); }
        { const e = new TypeError("Invalid encoding"); e.code = "ERR_UNKNOWN_ENCODING"; throw e; }
      };
    }
  }
  if (G.Bun && typeof G.Bun.wrapAnsi === "undefined") {
    // A non-positive / non-finite `columns` returns the input unchanged (bun
    // wrap_ansi). Without the guard `for (i += width)` with a negative width
    // never terminates and the accumulator eats all memory.
    G.Bun.wrapAnsi = (s, width) => { s = String(s); const w = Math.floor(Number(width)); if (!Number.isFinite(w) || w <= 0 || s.length <= w) return s; const out = []; for (let i = 0; i < s.length; i += w) out.push(s.slice(i, i + w)); return out.join("\n"); };
  }
  // Blob/File attribute backing store. bun keeps every Blob attribute in a
  // native slot and exposes it as a Blob.prototype accessor, so `Object.keys(blob)`
  // is [] and `JSON.stringify(blob)` is "{}" — a Blob that crosses a JSON
  // boundary carries NOTHING with it. Own enumerable data properties would
  // leak type/size/name/lastModified into every JSON.stringify of a Blob
  // (elysia test/type-system/formdata.test.ts round-trips a Bun.file through a
  // handler's JSON response and toEqual's it against the original).
  const blobSlot = (o, k, v) => {
    Object.defineProperty(o, k, { value: v, writable: true, enumerable: false, configurable: true });
  };
  if (typeof G.Blob === "undefined") {
    // Byte-accurate Blob (WHATWG): parts are stored as raw bytes so invalid UTF-8
    // (e.g. CESU-8 surrogates) survives round-trips like structuredClone.
    const partBytes = (p) => {
      if (typeof p === "string") return te.encode(p);
      if (p instanceof Uint8Array) return new Uint8Array(p);
      if (ArrayBuffer.isView(p)) return new Uint8Array(p.buffer.slice(p.byteOffset, p.byteOffset + p.byteLength));
      if (p instanceof ArrayBuffer) return new Uint8Array(p.slice(0));
      if (p && p._u8 instanceof Uint8Array) return new Uint8Array(p._u8); // Blob part
      return te.encode(String(p));
    };
    // bun canonicalises a Blob's `type` at construction: an *exact* (case-sensitive)
    // hit in its MIME table is replaced by that entry's canonical form — which is
    // where the `;charset=utf-8` suffix comes from — and anything else is merely
    // lowercased (a non-ASCII byte clears it to "").
    //
    // ref: bun-ref/src/jsc/rare_data.rs:735 `mime_type_from_string` looks the raw
    //   bytes up in the generated table (exact match, no case folding) and returns
    //   `Compact::from(entry).to_mime_type()`. bun-ref/src/http_types/MimeType.rs:73-102
    //   is that `to_mime_type` if-chain, and :288-300 the canonical constants it
    //   returns. Only these ten inputs rewrite the string; every other table entry
    //   (image/png, …) falls through the chain unchanged (:105-109).
    //
    // Order is load-bearing and is verified against bun 1.3.14:
    //   "text/plain" -> "text/plain;charset=utf-8"  (table hit)
    //   "TEXT/CSS"   -> "text/css"                  (miss -> lowercase only, NOT
    //                                                re-normalised afterwards)
    //   "application/x-www-form-urlencoded"
    //                -> "application/x-www-form-urlencoded;charset=UTF-8"  — the hit
    //      keeps its *uppercase* UTF-8, which is what proves the canonical form is
    //      returned before (never through) the lowercasing step.
    const MIME_CANONICAL = new Map([
      ["application/webassembly", "application/wasm"],                             // WASM       :300
      ["application/javascript", "text/javascript;charset=utf-8"],                 // JAVASCRIPT :291
      ["application/json", "application/json;charset=utf-8"],                      // JSON       :297
      ["application/x-www-form-urlencoded", "application/x-www-form-urlencoded;charset=UTF-8"], // :83
      ["image/vnd.microsoft.icon", "image/vnd.microsoft.icon"],                    // ICO        :293
      ["text/css", "text/css;charset=utf-8"],                                      // CSS        :290
      ["text/html", "text/html;charset=utf-8"],                                    // HTML       :295
      ["text/javascript", "text/javascript;charset=utf-8"],                        // JAVASCRIPT :291
      ["text/jsx", "text/javascript;charset=utf-8"],                               // JAVASCRIPT :291
      ["text/plain", "text/plain;charset=utf-8"]                                   // TEXT       :299
    ]);
    const normalizeMimeType = (raw) => {
      if (raw == null) return "";
      const t = "" + raw;
      const canon = MIME_CANONICAL.get(t);
      if (canon !== undefined) return canon;
      // Verified against bun 1.3.14: a type carrying a non-ASCII byte (U+00E9)
      // becomes "", while one carrying an ASCII control char (BEL) survives
      // verbatim -- so the guard is an all-ASCII test, NOT the WHATWG 0x20-0x7E
      // range check, which would have cleared the control-char case too.
      for (let i = 0; i < t.length; i++) if (t.charCodeAt(i) > 127) return "";
      return t.toLowerCase();
    };
    G.Blob = class Blob {
      get [Symbol.toStringTag]() { return "Blob"; }
      constructor(parts, opts) {
        // bun Blob.rs:3515-3518 iterates the array (per-index [[Get]], consults
        // the prototype) and skips undefined/null — so a sparse array's holes
        // don't become "undefined"/"null" chunks.
        //
        // The parts are kept as a LIST and joined only when someone actually
        // needs one contiguous buffer (see the `_u8` accessor below). Eagerly
        // concatenating made `new Blob([buf, buf, …])` cost the sum of the
        // parts even when every part is the same buffer: the 2 GiB blob in
        // regression/issue/8254 and the 576 MiB one in js/web/fetch/blob-oom
        // were both OOM-killed inside this constructor, before a single byte
        // was ever read.
        //
        // `seen` gives the second half of that: parts are still COPIED (a Blob
        // must not observe later mutations of its source), but each distinct
        // source object is copied ONCE and the copy is shared by every part
        // that refers to it. Both suites lean on exactly that (8254 builds
        // 2049 parts out of 256 buffers).
        //
        // Collected into a NULL-PROTOTYPE object, not an array: `chunks.push(x)`
        // stores through [[Set]], so a user-defined getter-only index accessor
        // on Array.prototype (js/web/fetch/blob-array-fast-path installs one)
        // makes the constructor throw "Attempted to assign to readonly
        // property". bun's Blob.rs collects natively and never consults
        // Array.prototype.
        const chunks = { __proto__: null };
        let count = 0;
        let total = 0;
        const seen = new Map();
        for (const p of (parts || [])) {
          if (p === undefined || p === null) continue;
          let b;
          if (typeof p === "object") {
            b = seen.get(p);
            if (b === undefined) { b = partBytes(p); seen.set(p, b); }
          } else {
            b = partBytes(p);
          }
          if (b.length === 0) continue;
          chunks[count++] = b;
          total += b.length;
        }
        // Array.from(..., mapper) creates each element with CreateDataProperty,
        // so the result is a real (iterable) array without ever going through
        // [[Set]] and the hostile Array.prototype accessor.
        const partList = Array.from({ length: count }, (_unused, i) => chunks[i]);
        // The backing store is not a WHATWG field: bun keeps a Blob's bytes off
        // the object entirely (`Object.keys(blob)` is [] there). Non-enumerable
        // so inspect/JSON.stringify/deep-equal don't walk the bytes one element
        // at a time — Bun.inspect(Bun.file("40mb.mp4")) built a 183MB string
        // and hung the process.
        blobSlot(this, "__parts", partList);
        blobSlot(this, "__size", total);
        // Attributes live in non-enumerable slots behind Blob.prototype
        // accessors (see below); `size` is always derived from `__size`.
        blobSlot(this, "__type", normalizeMimeType(opts && opts.type));
      }
      // Blob.rs guards every string/typed-array materialization against the
      // synthetic allocation limit; arrayBuffer() is exempt (ArrayBuffer has no
      // such cap). Without this a multi-GB blob really decodes and the process
      // is OOM-killed instead of throwing. The guard reads `size` (cheap) so it
      // fires BEFORE the part list is joined, not after.
      text() { G.__mbunCheckAllocLimit(this.size, "text"); return Promise.resolve(td.decode(this._u8)); }
      json() { G.__mbunCheckAllocLimit(this.size, "json"); return Promise.resolve(JSON.parse(td.decode(this._u8))); }
      arrayBuffer() { return Promise.resolve(this._u8.buffer.slice(this._u8.byteOffset, this._u8.byteOffset + this._u8.byteLength)); }
      bytes() { G.__mbunCheckAllocLimit(this.size, "bytes"); return Promise.resolve(new Uint8Array(this._u8)); }
      // Slicing walks the part list and keeps sub-views of the parts it
      // overlaps, so `bigBlob.slice(n, n + 1)` costs one byte, not a join of
      // the whole blob.
      slice(start, end, type) {
        const size = this.size;
        const norm = (v, dflt) => {
          if (v === undefined) return dflt;
          let n = Number(v);
          if (Number.isNaN(n)) n = 0;
          n = Math.trunc(n);
          return n < 0 ? Math.max(size + n, 0) : Math.min(n, size);
        };
        const s = norm(start, 0);
        const e = Math.max(norm(end, size), s);
        const out = [];
        let off = 0;
        for (const c of this.__parts || []) {
          const cs = off, ce = off + c.length;
          off = ce;
          if (ce <= s) continue;
          if (cs >= e) break;
          out.push(c.subarray(Math.max(0, s - cs), Math.min(c.length, e - cs)));
        }
        const b = new G.Blob([], { type: type || "" });
        blobSlot(b, "__parts", out);
        blobSlot(b, "__size", e - s);
        return b;
      }
      // bun: a stream off a Blob carries the blob's type, so readableStreamToBlob
      // (and stream.blob()) round-trip it back onto the resulting Blob.
      stream() { const u8 = new Uint8Array(this._u8); const s = new G.ReadableStream({ start(c) { if (u8.length > 0) c.enqueue(u8); c.close(); } });
        try { Object.defineProperty(s, "__mbunBlobType", { value: this.type || "", enumerable: false, configurable: true }); } catch (e) {}
        // bun ByteBlobLoader fast-path: a blob-backed stream carries its full
        // bytes so readableStreamToArrayBuffer/blob/text can resolve
        // synchronously (Bun.peek.status === "fulfilled").
        try { Object.defineProperty(s, "__mbunBlobBytes", { value: u8, enumerable: false, configurable: true, writable: true }); } catch (e) {}
        return s; }
      // bun exposes Blob.prototype.formData(): decode the blob's own type as the
      // content-type, then run the shared body FormData parser (Body.rs get_form_data).
      formData() {
        const enc = formDataEncodingFromCT(this.type);
        if (enc === null) return Promise.reject(formDataMimeError());
        try { return Promise.resolve(formDataParseBody(new Uint8Array(this._u8), enc)); }
        catch (e) { return Promise.reject(e); }
      }
    };
    // bun exposes name/lastModified on Blob.prototype (not just File's): a
    // Bun.file() IS a plain Blob there — `Object.getPrototypeOf(Bun.file(p)) ===
    // Blob.prototype` — yet it answers .name/.lastModified. Descriptors pinned
    // against real bun 1.3.14: all four enumerable + non-configurable; only
    // `name` has a setter (`type`/`size`/`lastModified` throw on assignment in
    // strict mode).
    const attr = (name, get, set) => {
      Object.defineProperty(G.Blob.prototype, name, set
        ? { get, set, enumerable: true, configurable: false }
        : { get, enumerable: true, configurable: false });
    };
    // `_u8` — the one contiguous view of a Blob's bytes. Everything that wants
    // "the whole blob as one Uint8Array" (Bun.build's file map, the websocket
    // and socket senders, structuredClone, FormData) reads it, so it stays a
    // plain property from the outside; it is an accessor only so the join is
    // deferred to the first such read and then cached back into `__parts`.
    // Assignment (`blob._u8 = bytes`, used by Bun.file and the loaders) still
    // works and simply replaces the part list.
    Object.defineProperty(G.Blob.prototype, "_u8", {
      get() {
        const parts = this.__parts;
        if (!parts) return new Uint8Array(0);
        if (parts.length === 1) return parts[0];
        const out = new Uint8Array(this.__size || 0);
        let o = 0;
        for (const c of parts) { out.set(c, o); o += c.length; }
        blobSlot(this, "__parts", [out]);
        return out;
      },
      set(v) {
        const u = v == null ? new Uint8Array(0)
          : v instanceof Uint8Array ? v
          : ArrayBuffer.isView(v) ? new Uint8Array(v.buffer, v.byteOffset, v.byteLength)
          : v instanceof ArrayBuffer ? new Uint8Array(v)
          : new Uint8Array(0);
        blobSlot(this, "__parts", u.length ? [u] : []);
        blobSlot(this, "__size", u.length);
      },
      enumerable: false,
      configurable: true,
    });
    attr("type", function () { return this.__type || ""; });
    attr("size", function () { return this.__size || 0; });
    attr("name", function () { return this.__name; }, function (v) { blobSlot(this, "__name", v); });
    // A Blob that was never given an mtime reports bun's sentinel (2^52-1).
    attr("lastModified", function () {
      return this.__lastModified === undefined ? 4503599627370495 : this.__lastModified;
    });
  }

  // ---- FormData (WHATWG multi-map of string|File) ----
  // Multipart parser: bytes + boundary -> array of {name, value} where value is a
  // File for parts with a filename, else the decoded string.
  // Raised by parseMultipart. The raw-boundary public APIs (FormData.from,
  // Bun.readableStreamToFormData) both report a plain Error worded
  // "{name} while parsing FormData" (FormData.rs:197 throw_error(e, "while
  // parsing FormData")), so that is the default shape here. The BODY path
  // (Request/Response.formData) words the SAME failure differently -- TypeError
  // "FormData parse error {name}" with code ERR_FORMDATA_PARSE_ERROR
  // (Body.rs:2114) -- so it re-wraps off `__mbunFormDataErrName` rather than
  // this message. Unifying the two surfaces would corrupt one of them; all
  // three verified against bun-rust 1.4.0.
  const formDataParseFailure = (name) => {
    const e = new Error(name + " while parsing FormData");
    e.__mbunFormDataErrName = name;
    return e;
  };
  const parseMultipart = (u8, boundary) => {
    const fd = new G.FormData();
    const dashBoundary = te.encode("--" + boundary);
    // FormData.rs:291 for_each_multipart_entry formats `--{boundary}--` into a
    // fixed 76-byte buffer, so boundary.len() + 4 > 76 is rejected up front; the
    // final boundary must exist (strings::last_index_of) or the whole parse is an
    // error. mbun previously returned an EMPTY FormData for both -- silently
    // wrong rather than loud. Byte length (not UTF-16 .length) matches Rust.
    if (te.encode(boundary).length + 4 > 76) throw formDataParseFailure("boundary is too long");
    const finalBoundary = te.encode("--" + boundary + "--");
    const lastIndexOf = (hay, needle) => {
      outer: for (let i = hay.length - needle.length; i >= 0; i--) {
        for (let j = 0; j < needle.length; j++) if (hay[i + j] !== needle[j]) continue outer;
        return i;
      }
      return -1;
    };
    if (lastIndexOf(u8, finalBoundary) < 0) throw formDataParseFailure("missing final boundary");
    // find each boundary occurrence
    const indexOf = (hay, needle, from) => {
      outer: for (let i = from; i <= hay.length - needle.length; i++) {
        for (let j = 0; j < needle.length; j++) if (hay[i + j] !== needle[j]) continue outer;
        return i;
      }
      return -1;
    };
    let pos = indexOf(u8, dashBoundary, 0);
    if (pos < 0) return fd;
    pos += dashBoundary.length;
    while (pos < u8.length) {
      // after boundary: either "--" (end) or CRLF
      if (u8[pos] === 0x2d && u8[pos + 1] === 0x2d) break;
      if (u8[pos] === 0x0d && u8[pos + 1] === 0x0a) pos += 2;
      // headers until CRLFCRLF
      const headerEnd = indexOf(u8, te.encode("\r\n\r\n"), pos);
      if (headerEnd < 0) break;
      const headerText = td.decode(u8.subarray(pos, headerEnd));
      const bodyStart = headerEnd + 4;
      const next = indexOf(u8, dashBoundary, bodyStart);
      if (next < 0) break;
      // body is [bodyStart, next-2) (strip trailing CRLF before boundary)
      let bodyEnd = next;
      if (bodyEnd >= 2 && u8[bodyEnd - 2] === 0x0d && u8[bodyEnd - 1] === 0x0a) bodyEnd -= 2;
      const body = u8.subarray(bodyStart, bodyEnd);
      // parse Content-Disposition
      let name = null, filename = null, ctype = null;
      for (const line of headerText.split("\r\n")) {
        const lc = line.toLowerCase();
        if (lc.startsWith("content-disposition:")) {
          const nm = /name="([^"]*)"/.exec(line); if (nm) name = nm[1];
          const fn = /filename="([^"]*)"/.exec(line); if (fn) filename = fn[1];
        } else if (lc.startsWith("content-type:")) {
          ctype = line.slice(line.indexOf(":") + 1).trim();
        }
      }
      if (name !== null) {
        if (filename !== null) { const __nm = filename === "" ? undefined : filename; const __ct = ctype && ctype !== "application/octet-stream" ? ctype : ""; const __pf = new G.File([new Uint8Array(body)], __nm === undefined ? "" : __nm, { type: __ct }); if (__nm === undefined) __pf.name = undefined; fd.append(name, __pf); }
        else fd.append(name, td.decode(body));
      }
      pos = next + dashBoundary.length;
    }
    return fd;
  };
  G.__mbunParseFormData = (u8, boundary) => {
    if (!boundary) {
      // URL-encoded fallback
      const fd = new G.FormData();
      const text = td.decode(u8);
      for (const pair of text.split("&")) { if (!pair) continue; const eq = pair.indexOf("="); const k = eq < 0 ? pair : pair.slice(0, eq); const v = eq < 0 ? "" : pair.slice(eq + 1); fd.append(decodeURIComponent(k.replace(/\+/g, " ")), decodeURIComponent(v.replace(/\+/g, " "))); }
      return fd;
    }
    return parseMultipart(u8 instanceof Uint8Array ? u8 : new Uint8Array(u8), boundary);
  };
  if (typeof G.FormData === "undefined" || typeof G.FormData.from !== "function") {
    const FormData = class FormData {
      constructor() { this._e = []; }
      _norm(v, fn) {
        // WHATWG: a Blob value becomes a File; a filename (or default "blob")
        // names it. Strings pass through.
        if (v && (v instanceof G.Blob || (G.File && v instanceof G.File))) {
          // A Blob carrying its own name (Bun.file) keeps it: bun serializes the
          // BunFile's path as the part filename rather than renaming it "blob".
          if (G.File && v instanceof G.File && fn === undefined) return v;
          // A plain Blob (no filename) becomes a File with name undefined, so
          // multipart serializes filename="" (ref bun FormData append).
          const name = fn !== undefined ? String(fn) : (v.name !== undefined ? v.name : undefined);
          const __f = new G.File([new Uint8Array(v._u8 || new Uint8Array(0))], name, { type: v.type || "" });
          if (name === undefined) __f.name = undefined;
          return __f;
        }
        return String(v);
      }
      append(k, v, fn) { this._e.push([String(k), this._norm(v, fn)]); }
      set(k, v, fn) { this.delete(k); this._e.push([String(k), this._norm(v, fn)]); }
      get(k) { const e = this._e.find((x) => x[0] === String(k)); return e ? e[1] : null; }
      getAll(k) { return this._e.filter((x) => x[0] === String(k)).map((x) => x[1]); }
      has(k) { return this._e.some((x) => x[0] === String(k)); }
      delete(k) { this._e = this._e.filter((x) => x[0] !== String(k)); }
      forEach(cb, thisArg) { for (const [k, v] of this._e) cb.call(thisArg, v, k, this); }
      keys() { return this._e.map((x) => x[0])[Symbol.iterator](); }
      values() { return this._e.map((x) => x[1])[Symbol.iterator](); }
      entries() { return this._e.map((x) => [x[0], x[1]])[Symbol.iterator](); }
      [Symbol.iterator]() { return this.entries(); }
      toJSON() {
        // string values as-is; File/Blob as {name,size,data}; duplicates -> arrays.
        const ser = (v) => {
          if (v && (v instanceof G.Blob || (G.File && v instanceof G.File))) {
            return { name: v.name !== undefined ? v.name : "blob", size: v.size, data: Array.from(v._u8 || new Uint8Array(0)) };
          }
          return String(v);
        };
        const out = {};
        for (const [k, v] of this._e) {
          const s = ser(v);
          if (k in out) { if (Array.isArray(out[k])) out[k].push(s); else out[k] = [out[k], s]; }
          else out[k] = s;
        }
        return out;
      }
      static from(input, boundary) {
        let u8;
        if (typeof input === "string") u8 = te.encode(input);
        else if (input instanceof G.Blob) u8 = new Uint8Array(input._u8);
        else if (input instanceof ArrayBuffer) u8 = new Uint8Array(input);
        else if (ArrayBuffer.isView(input)) u8 = new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
        else throw new TypeError("FormData.from expects a string, Blob, ArrayBuffer, or TypedArray");
        // Parse failures already carry FormData.rs:197's "{name} while parsing
        // FormData" wording (a plain Error, no code) — propagate as-is.
        return G.__mbunParseFormData(u8, boundary);
      }
    };
    G.FormData = FormData;
  }

  // ---- FormData serialization (Blob.rs: from_dom_form_data) ----
  // Port of encode_form_data_component (Blob.rs): a name/filename escapes `"` to
  // %22; CR/LF escape (name: normalized to one %0D%0A; filename: %0D / %0A each).
  // A string value never escapes `"` and normalizes CR/LF/CRLF to one CRLF.
  const encodeFDComponent = (s, kind) => {
    const escape = kind !== "value";
    const normalize = kind !== "filename";
    let out = "";
    for (let i = 0; i < s.length; i++) {
      const c = s[i];
      if (c === '"' && escape) { out += "%22"; continue; }
      if (c === "\r" || c === "\n") {
        if (normalize) { out += escape ? "%0D%0A" : "\r\n"; if (c === "\r" && s[i + 1] === "\n") i++; }
        else out += c === "\r" ? "%0D" : "%0A";
        continue;
      }
      out += c;
    }
    return out;
  };
  // "----WebKitFormBoundary" + 32 lowercase hex of a fresh UUID (Blob.rs).
  const fdBoundary = () => {
    const b = new Uint8Array(16);
    if (G.crypto && G.crypto.getRandomValues) G.crypto.getRandomValues(b);
    else for (let i = 0; i < 16; i++) b[i] = Math.floor(Math.random() * 256);
    let hex = "";
    for (let i = 0; i < 16; i++) hex += b[i].toString(16).padStart(2, "0");
    return "----WebKitFormBoundary" + hex;
  };
  const isBlobLike = (v) => !!(v && (v instanceof G.Blob || (G.File && v instanceof G.File)));
  G.__mbunSerializeFormData = (fd) => {
    const boundary = fdBoundary();
    const chunks = [];
    const put = (s) => chunks.push(te.encode(s));
    for (const [name, value] of fd._e) {
      put("--" + boundary + "\r\n");
      put('Content-Disposition: form-data; name="' + encodeFDComponent(String(name), "name"));
      if (isBlobLike(value)) {
        put('"; filename="' + encodeFDComponent(value.name !== undefined ? String(value.name) : "", "filename") + '"\r\n');
        // An empty or CR/LF-bearing blob type falls back to octet-stream (Blob.rs).
        const t = value.type || "";
        put("Content-Type: " + (t && !/[\r\n]/.test(t) ? t : "application/octet-stream") + "\r\n\r\n");
        chunks.push(value._u8 || new Uint8Array(0));  // borrow the blob store; u8.set() copies once at join (no 2x peak)
      } else {
        put('"\r\n\r\n');
        put(encodeFDComponent(String(value), "value"));
      }
      put("\r\n");
    }
    put("--" + boundary + "--\r\n");
    let total = 0; for (const c of chunks) total += c.length;
    const u8 = new Uint8Array(total);
    let off = 0; for (const c of chunks) { u8.set(c, off); off += c.length; }
    return { u8, contentType: "multipart/form-data; boundary=" + boundary };
  };
  // A body init that carries its own type becomes bytes + that content-type;
  // the Request/Response constructor applies it only when the caller set none
  // (Request.rs: content-type is taken from the body blob). bun leaves a plain
  // string/buffer body typeless.
  G.__mbunNormalizeBody = (body) => {
    if (body != null && G.FormData && body instanceof G.FormData) {
      const r = G.__mbunSerializeFormData(body);
      return { body: r.u8, contentType: r.contentType };
    }
    if (body != null && G.URLSearchParams && body instanceof G.URLSearchParams)
      return { body: body.toString(), contentType: "application/x-www-form-urlencoded;charset=UTF-8" };
    if (isBlobLike(body)) return { body, contentType: body.type || null };
    return { body, contentType: null };
  };

  // ---- fetch (data:/blob: resolve locally; network fetch needs an HTTP client
  // — DEFERRED, honest reject rather than undefined so `typeof fetch` is correct) ----
  if (typeof G.fetch === "undefined") {
    G.fetch = function (input, init) {
      const url = typeof input === "object" && input && input.url ? input.url : String(input);
      if (url.startsWith("data:")) {
        const comma = url.indexOf(",");
        const meta = url.slice(5, comma), body = url.slice(comma + 1);
        const isB64 = /;base64$/i.test(meta);
        const text = isB64 ? (G.atob ? G.atob(body) : body) : decodeURIComponent(body);
        return Promise.resolve(new G.Response(text, { status: 200, headers: { "content-type": meta.replace(/;base64$/i, "") || "text/plain" } }));
      }
      return Promise.reject(new Error("fetch: network requests are not implemented yet in mbun (url: " + url + ")"));
    };
  }

  // ---- HTML structured serialize/deserialize ----
  // structuredClone (direct graph clone with memory map + transfer) and
  // bun:jsc serialize/deserialize (binary format: JSC CloneSerializer-compatible
  // for the common tags — Array/Object/Date/String/ObjectReference + string pool —
  // so version<=13 payloads written by bun deserialize correctly; version 14 adds
  // the terminal types and BigInts to the object reference pool like bun does).
  {
    const dce = (m) => new G.DOMException(m || "The object can not be cloned.", "DataCloneError");
    const ERR_CTORS = { Error, EvalError, RangeError, ReferenceError, SyntaxError, TypeError, URIError };
    const MAX_SERIALIZED = 2147483648; // 2GiB serialization buffer cap (matches bun/WebKit)
    const nc = () => M["crypto"] || {};
    const nnet = () => M["net"] || {};
    const isBigIntObject = (v) => { try { BigInt.prototype.valueOf.call(v); return true; } catch (e) { return false; } };
    const isSymbolObject = (v) => { try { Symbol.prototype.valueOf.call(v); return true; } catch (e) { return false; } };
    const copyWithProto = (v) => { const o = Object.create(Object.getPrototypeOf(v)); for (const k of Object.getOwnPropertyNames(v)) { try { o[k] = v[k]; } catch (e) {} } return o; };
    const errorFromParts = (name, message, stack, hasCause, cause, errors) => {
      const Ctor = ERR_CTORS[name] || (name === "AggregateError" && G.AggregateError) || Error;
      const e = Ctor === G.AggregateError ? new Ctor(errors || [], message) : new Ctor(message);
      if (stack !== undefined) e.stack = stack;
      if (hasCause) e.cause = cause;
      return e;
    };

    // -- direct graph clone (structuredClone) --
    const cloneValue = (value, transferSet) => {
      const memory = new Map();
      const clone = (v) => {
        const t = typeof v;
        if (t === "symbol") throw dce("Symbol values cannot be cloned.");
        if (t === "function") throw dce(String(v.name || "Function") + " could not be cloned.");
        if (v === null || t !== "object") return v;
        if (memory.has(v)) return memory.get(v);
        let out;
        if (Array.isArray(v)) {
          out = new Array(v.length); memory.set(v, out);
          for (const k of Object.keys(v)) out[k] = clone(v[k]);
          return out;
        }
        if (v instanceof ArrayBuffer) {
          if (v.detached) throw dce("An ArrayBuffer is detached and could not be cloned.");
          if (v.byteLength >= MAX_SERIALIZED) throw dce("Serialized ArrayBuffer is too large.");
          out = transferSet && transferSet.has(v) ? v.transfer() : v.slice(0);
          memory.set(v, out); return out;
        }
        if (G.SharedArrayBuffer && v instanceof G.SharedArrayBuffer) {
          // SharedArrayBuffer is a distinct type; slice() yields a fresh SAB.
          if (v.byteLength >= MAX_SERIALIZED) throw dce("Serialized ArrayBuffer is too large.");
          out = v.slice(0); memory.set(v, out); return out;
        }
        if (ArrayBuffer.isView(v)) {
          const buf = clone(v.buffer);
          if (v instanceof DataView) out = new DataView(buf, v.byteOffset, v.byteLength);
          else if (G.Buffer && G.Buffer.isBuffer(v)) out = G.Buffer.from(buf, v.byteOffset, v.length);
          else out = new v.constructor(buf, v.byteOffset, v.length);
          memory.set(v, out); return out;
        }
        if (v instanceof Date) { out = new Date(v.getTime()); memory.set(v, out); return out; }
        if (v instanceof RegExp) { out = new RegExp(v.source, v.flags); memory.set(v, out); return out; }
        if (v instanceof Map) { out = new Map(); memory.set(v, out); for (const [k, val] of v) out.set(clone(k), clone(val)); return out; }
        if (v instanceof Set) { out = new Set(); memory.set(v, out); for (const x of v) out.add(clone(x)); return out; }
        if (G.DOMException && v instanceof G.DOMException) { out = new G.DOMException(v.message, v.name); memory.set(v, out); return out; }
        if (v instanceof Error) {
          const name = String(v.name);
          out = errorFromParts(name, v.message === undefined ? undefined : String(v.message), v.stack, false);
          memory.set(v, out);
          if (Object.hasOwn(v, "cause")) out.cause = clone(v.cause);
          if (G.AggregateError && v instanceof G.AggregateError && Object.hasOwn(v, "errors")) out.errors = clone(v.errors);
          return out;
        }
        // Bun.file() IS a Blob, so this must precede the Blob/File arms — the
        // generic Blob clone would drop its .name/.lastModified.
        if (v.__isBunFile) { out = copyWithProto(v); memory.set(v, out); return out; }
        if (G.File && v instanceof G.File) { out = new G.File([v._u8 || ""], v.name, { type: v.type, lastModified: v.lastModified }); memory.set(v, out); return out; }
        if (G.Blob && v instanceof G.Blob) { out = new G.Blob(v._u8 ? [v._u8] : [], { type: v.type }); memory.set(v, out); return out; }
        // A native CryptoKey has NO own properties -- its state lives in
        // webcrypto's keyMetadata WeakMap -- so copyWithProto yields a husk
        // whose every getter throws ERR_INVALID_THIS. Re-mint it instead.
        if (G.CryptoKey && v instanceof G.CryptoKey) {
          out = (G.__mbunCryptoKeyClone && G.__mbunCryptoKeyClone(v)) || copyWithProto(v);
          memory.set(v, out); return out;
        }
        if (nc().KeyObject && v instanceof nc().KeyObject) { out = copyWithProto(v); memory.set(v, out); return out; }
        if (nc().X509Certificate && v instanceof nc().X509Certificate) { out = copyWithProto(v); memory.set(v, out); return out; }
        // node BlockList clones share the underlying rule set (native-handle semantics).
        if (nnet().BlockList && v instanceof nnet().BlockList) { out = Object.create(nnet().BlockList.prototype); out._rules = v._rules; memory.set(v, out); return out; }
        if (v instanceof Boolean) { out = new Boolean(Boolean.prototype.valueOf.call(v)); memory.set(v, out); return out; }
        if (v instanceof Number) { out = new Number(Number.prototype.valueOf.call(v)); memory.set(v, out); return out; }
        if (v instanceof String) { out = new String(String.prototype.valueOf.call(v)); memory.set(v, out); return out; }
        if (isBigIntObject(v)) { out = Object(BigInt.prototype.valueOf.call(v)); memory.set(v, out); return out; }
        if (isSymbolObject(v)) throw dce("Symbol objects cannot be cloned.");
        if (v instanceof Promise || v instanceof WeakMap || v instanceof WeakSet) throw dce("The object can not be cloned.");
        if (G.MessagePort && v instanceof G.MessagePort) throw dce("MessagePort can only be transferred, not cloned.");
        out = {}; memory.set(v, out);
        for (const k of Object.keys(v)) out[k] = clone(v[k]);
        return out;
      };
      return clone(value);
    };

    G.structuredClone = function structuredClone(value, options) {
      if (arguments.length === 0) throw new TypeError("structuredClone requires at least 1 argument");
      if (options !== undefined && options !== null) {
        const ot = typeof options;
        if (ot !== "object" && ot !== "function") throw new TypeError("structuredClone options must be an object");
      }
      const transfer = options == null ? undefined : options.transfer;
      let transferSet = null;
      if (transfer !== undefined) {
        // WebIDL sequence<object>: converted (and possibly throwing) before any
        // serialization, so a rejected call never detaches buffers.
        if (transfer === null || (typeof transfer !== "object" && typeof transfer !== "function") || typeof transfer[Symbol.iterator] !== "function")
          throw new TypeError("transfer must be an iterable of transferable objects");
        const entries = [];
        for (const entry of transfer) {
          const et = typeof entry;
          if (entry === null || (et !== "object" && et !== "function")) throw new TypeError("transfer list contains a value that is not an object");
          entries.push(entry);
        }
        transferSet = new Set();
        for (const entry of entries) {
          if (transferSet.has(entry)) throw dce("transfer list contains a duplicate entry");
          const isAB = entry instanceof ArrayBuffer;
          if (!isAB && !(G.MessagePort && entry instanceof G.MessagePort)) throw dce("The object is not transferable.");
          if (isAB && entry.detached) throw dce("A detached ArrayBuffer cannot be transferred.");
          transferSet.add(entry);
        }
      }
      const out = cloneValue(value, transferSet);
      if (transferSet) for (const entry of transferSet) { if (entry instanceof ArrayBuffer && !entry.detached) { try { entry.transfer(); } catch (e) {} } }
      return out;
    };

    // -- binary serialize/deserialize (bun:jsc) --
    // Tag numbers follow WebKit's CloneSerializer for the shared subset.
    const T = { Array: 1, Object: 2, Undefined: 3, Null: 4, Int: 5, Zero: 6, One: 7, False: 8, True: 9, Double: 10,
                Date: 11, Blob: 15, String: 16, EmptyString: 17, RegExp: 18, ObjRef: 19, ArrayBuffer: 21, View: 22,
                TrueObj: 24, FalseObj: 25, StringObj: 26, NumberObj: 28, MapObj: 30, SetObj: 31,
                BigInt: 60, BigIntObj: 61, Error: 62, DOMException: 63, File: 64, BunFile: 65,
                CryptoKey: 66, KeyObject: 67, X509: 68 };
    const VIEW_TYPES = ["Int8Array", "Uint8Array", "Uint8ClampedArray", "Int16Array", "Uint16Array", "Int32Array",
                        "Uint32Array", "Float32Array", "Float64Array", "BigInt64Array", "BigUint64Array", "DataView", "Buffer", "Float16Array"];
    const CURRENT_VERSION = 14;

    const serializeBytes = (value, forStorage) => {
      let cap = 4096, buf = new Uint8Array(cap), dv = new DataView(buf.buffer), len = 0;
      const ensure = (n) => {
        if (len + n <= cap) return;
        if (len + n > MAX_SERIALIZED) throw dce("Serialized data is too large.");
        let ncap = cap; while (ncap < len + n) ncap *= 2;
        if (ncap > MAX_SERIALIZED) ncap = MAX_SERIALIZED;
        const nb = new Uint8Array(ncap); nb.set(buf.subarray(0, len)); buf = nb; dv = new DataView(buf.buffer); cap = ncap;
      };
      const u8 = (b) => { ensure(1); buf[len++] = b; };
      const u16 = (x) => { ensure(2); dv.setUint16(len, x, true); len += 2; };
      const u32 = (x) => { ensure(4); dv.setUint32(len, x, true); len += 4; };
      const i32 = (x) => { ensure(4); dv.setInt32(len, x, true); len += 4; };
      const f64 = (x) => { ensure(8); dv.setFloat64(len, x, true); len += 8; };
      const bytes = (b) => { ensure(b.length); buf.set(b, len); len += b.length; };
      const str = (s) => {
        s = String(s);
        let latin1 = true;
        for (let i = 0; i < s.length; i++) if (s.charCodeAt(i) > 0xff) { latin1 = false; break; }
        if (latin1) { u32(s.length | 0x80000000); ensure(s.length); for (let i = 0; i < s.length; i++) buf[len++] = s.charCodeAt(i); }
        else { u32(s.length); ensure(s.length * 2); for (let i = 0; i < s.length; i++) { dv.setUint16(len, s.charCodeAt(i), true); len += 2; } }
      };
      const bigintData = (b) => { u8(b < 0n ? 1 : 0); str((b < 0n ? -b : b).toString()); };
      const pool = new Map();
      let poolSize = 0;
      const writeIdx = (idx) => { if (poolSize <= 0xff) u8(idx); else if (poolSize <= 0xffff) u16(idx); else u32(idx); };
      const poolAdd = (v) => { pool.set(v, poolSize++); };
      const write = (v) => {
        switch (typeof v) {
          case "undefined": return u8(T.Undefined);
          case "boolean": return u8(v ? T.True : T.False);
          case "number":
            if (Object.is(v, -0) || !Number.isInteger(v) || v > 0x7fffffff || v < -0x80000000) { u8(T.Double); return f64(v); }
            if (v === 0) return u8(T.Zero);
            if (v === 1) return u8(T.One);
            u8(T.Int); return i32(v);
          case "string": if (v === "") return u8(T.EmptyString); u8(T.String); return str(v);
          case "bigint": u8(T.BigInt); bigintData(v); poolSize++; return; // pooled slot, never referenced
          case "symbol": throw dce("Symbol values cannot be cloned.");
          case "function": throw dce(String(v.name || "Function") + " could not be cloned.");
        }
        if (v === null) return u8(T.Null);
        if (pool.has(v)) { u8(T.ObjRef); return writeIdx(pool.get(v)); }
        if (Array.isArray(v)) {
          poolAdd(v); u8(T.Array); u32(v.length >>> 0);
          const extra = [];
          for (const k of Object.keys(v)) {
            const n = +k;
            if (Number.isInteger(n) && n >= 0 && String(n) === k && n < v.length) { u32(n); write(v[k]); }
            else extra.push(k);
          }
          u32(0xffffffff);
          for (const k of extra) { str(k); write(v[k]); }
          return u32(0xffffffff);
        }
        if (v instanceof ArrayBuffer || (G.SharedArrayBuffer && v instanceof G.SharedArrayBuffer)) {
          if (v instanceof ArrayBuffer && v.detached) throw dce("An ArrayBuffer is detached and could not be cloned.");
          if (v.byteLength >= MAX_SERIALIZED) throw dce("Serialized ArrayBuffer is too large.");
          poolAdd(v); u8(T.ArrayBuffer); u32(v.byteLength); return bytes(new Uint8Array(v));
        }
        if (ArrayBuffer.isView(v)) {
          poolAdd(v); u8(T.View);
          const vt = v instanceof DataView ? "DataView" : (G.Buffer && G.Buffer.isBuffer(v)) ? "Buffer" : (v[Symbol.toStringTag] || "Uint8Array");
          u8(Math.max(0, VIEW_TYPES.indexOf(vt))); u32(v.byteOffset); u32(v instanceof DataView ? v.byteLength : v.length);
          return write(v.buffer);
        }
        if (v instanceof Date) { poolAdd(v); u8(T.Date); return f64(v.getTime()); }
        if (v instanceof RegExp) { poolAdd(v); u8(T.RegExp); str(v.source); return str(v.flags); }
        if (v instanceof Map) { poolAdd(v); u8(T.MapObj); const e = [...v]; u32(e.length); for (const [k, val] of e) { write(k); write(val); } return; }
        if (v instanceof Set) { poolAdd(v); u8(T.SetObj); const e = [...v]; u32(e.length); for (const x of e) write(x); return; }
        if (G.DOMException && v instanceof G.DOMException) { poolAdd(v); u8(T.DOMException); str(v.message); return str(v.name); }
        if (v instanceof Error) {
          poolAdd(v); u8(T.Error); str(v.name);
          const hasCause = Object.hasOwn(v, "cause");
          const hasErrors = !!(G.AggregateError && v instanceof G.AggregateError);
          u8(1 | (v.stack !== undefined ? 2 : 0) | (hasCause ? 4 : 0) | (hasErrors ? 8 : 0));
          str(v.message === undefined ? "" : v.message);
          if (v.stack !== undefined) str(v.stack);
          if (hasCause) write(v.cause);
          if (hasErrors) write(Array.isArray(v.errors) ? v.errors : []);
          return;
        }
        // Bun.file() IS a Blob: keep the BunFile tag (path-backed, re-opened on
        // read) ahead of the Blob/File arms that would inline its bytes instead.
        if (v.__isBunFile) { poolAdd(v); u8(T.BunFile); str(v.name || ""); str(v.type || ""); f64(v.lastModified || 0); return u32(v.size >>> 0); }
        if (G.File && v instanceof G.File) { poolAdd(v); u8(T.File); str(v.name); str(v.type); f64(v.lastModified); const b = v._u8 || new Uint8Array(0); u32(b.length); return bytes(b); }
        if (G.Blob && v instanceof G.Blob) { poolAdd(v); u8(T.Blob); str(v.type); const b = v._u8 || new Uint8Array(0); u32(b.length); return bytes(b); }
        if (G.CryptoKey && v instanceof G.CryptoKey) {
          poolAdd(v); u8(T.CryptoKey);
          str(JSON.stringify({ type: v.type, algorithm: v.algorithm, extractable: v.extractable, usages: v.usages }));
          const raw = v._raw || new Uint8Array(0); u32(raw.length); return bytes(raw);
        }
        if (nc().KeyObject && v instanceof nc().KeyObject) {
          poolAdd(v); u8(T.KeyObject);
          str(JSON.stringify({ type: v.type, asymmetricKeyType: v.asymmetricKeyType || null }));
          let material = "";
          try { const ex = v.type === "secret" ? v.export() : v.export({ format: "pem", type: v.type === "public" ? "spki" : "pkcs8" }); material = typeof ex === "string" ? ex : G.Buffer.from(ex).toString("base64"); } catch (e) {}
          return str(material);
        }
        if (nc().X509Certificate && v instanceof nc().X509Certificate) { poolAdd(v); u8(T.X509); str(v.subject || ""); return str(v.issuer || ""); }
        // Non-storable Bun cloneables (BlockList) serialize as an empty-object
        // placeholder for storage but must still occupy an object pool slot.
        if (nnet().BlockList && v instanceof nnet().BlockList) { poolAdd(v); u8(T.Object); return u32(0xffffffff); }
        if (v instanceof Boolean) { poolAdd(v); return u8(Boolean.prototype.valueOf.call(v) ? T.TrueObj : T.FalseObj); }
        if (v instanceof Number) { poolAdd(v); u8(T.NumberObj); return f64(Number.prototype.valueOf.call(v)); }
        if (v instanceof String) { poolAdd(v); u8(T.StringObj); return str(String.prototype.valueOf.call(v)); }
        if (isBigIntObject(v)) { poolAdd(v); u8(T.BigIntObj); return bigintData(BigInt.prototype.valueOf.call(v)); }
        if (isSymbolObject(v)) throw dce("Symbol objects cannot be cloned.");
        if (v instanceof Promise || v instanceof WeakMap || v instanceof WeakSet) throw dce("The object can not be cloned.");
        if (G.MessagePort && v instanceof G.MessagePort) throw dce("MessagePort can only be transferred, not cloned.");
        poolAdd(v); u8(T.Object);
        for (const k of Object.keys(v)) { str(k); write(v[k]); }
        return u32(0xffffffff);
      };
      u32(CURRENT_VERSION);
      write(value);
      const out = buf.slice(0, len);
      return G.Buffer ? G.Buffer.from(out.buffer, 0, len) : out;
    };

    const deserializeBytes = (input) => {
      let u8v;
      if (input instanceof ArrayBuffer) u8v = new Uint8Array(input);
      else if (ArrayBuffer.isView(input)) u8v = new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
      else throw new TypeError("deserialize expects an ArrayBuffer or TypedArray");
      const dv = new DataView(u8v.buffer, u8v.byteOffset, u8v.byteLength);
      let pos = 0;
      const ru8 = () => u8v[pos++];
      const ru16 = () => { const x = dv.getUint16(pos, true); pos += 2; return x; };
      const ru32 = () => { const x = dv.getUint32(pos, true); pos += 4; return x; };
      const ri32 = () => { const x = dv.getInt32(pos, true); pos += 4; return x; };
      const rf64 = () => { const x = dv.getFloat64(pos, true); pos += 8; return x; };
      const version = ru32();
      const v14 = version >= 14;
      const strPool = [];
      const readIdx = (size) => (size <= 0xff ? ru8() : size <= 0xffff ? ru16() : ru32());
      const readStrData = (first) => {
        const latin1 = !!(first & 0x80000000), n = first & 0x7fffffff;
        let s = "";
        if (latin1) { for (let i = 0; i < n; i++) s += String.fromCharCode(u8v[pos + i]); pos += n; }
        else { for (let i = 0; i < n; i++) s += String.fromCharCode(dv.getUint16(pos + 2 * i, true)); pos += 2 * n; }
        strPool.push(s);
        return s;
      };
      const rstr = () => { const first = ru32(); if (first === 0xfffffffe) return strPool[readIdx(strPool.length)]; return readStrData(first); };
      const readName = () => { const first = ru32(); if (first === 0xffffffff) return null; if (first === 0xfffffffe) return strPool[readIdx(strPool.length)]; return readStrData(first); };
      const rbig = () => { const neg = ru8(); const b = BigInt(rstr()); return neg ? -b : b; };
      const pool = [];
      const read = () => {
        const tag = ru8();
        switch (tag) {
          case T.Undefined: return undefined;
          case T.Null: return null;
          case T.True: return true;
          case T.False: return false;
          case T.Zero: return 0;
          case T.One: return 1;
          case T.Int: return ri32();
          case T.Double: return rf64();
          case T.EmptyString: return "";
          case T.String: return rstr();
          case T.BigInt: { const b = rbig(); if (v14) pool.push(b); return b; }
          case T.ObjRef: return pool[readIdx(pool.length)];
          case T.Array: {
            const n = ru32(); const arr = new Array(n); pool.push(arr);
            for (;;) { const i = ru32(); if (i === 0xffffffff) break; arr[i] = read(); }
            if (v14) for (;;) { const k = readName(); if (k === null) break; arr[k] = read(); }
            return arr;
          }
          case T.Object: {
            const o = {}; pool.push(o);
            for (;;) { const k = readName(); if (k === null) break; o[k] = read(); }
            return o;
          }
          case T.MapObj: { const m = new Map(); pool.push(m); const n = ru32(); for (let i = 0; i < n; i++) { const k = read(); m.set(k, read()); } return m; }
          case T.SetObj: { const s = new Set(); pool.push(s); const n = ru32(); for (let i = 0; i < n; i++) s.add(read()); return s; }
          case T.Date: { const d = new Date(rf64()); if (v14) pool.push(d); return d; }
          case T.RegExp: { const src = rstr(); const r = new RegExp(src, rstr()); if (v14) pool.push(r); return r; }
          case T.ArrayBuffer: { const n = ru32(); const out = u8v.slice(pos, pos + n).buffer; pos += n; pool.push(out); return out; }
          case T.View: {
            const slot = pool.length; pool.push(null);
            const vt = VIEW_TYPES[ru8()] || "Uint8Array"; const off = ru32(); const n = ru32();
            const ab = read();
            let out;
            if (vt === "DataView") out = new DataView(ab, off, n);
            else if (vt === "Buffer" && G.Buffer) out = G.Buffer.from(ab, off, n);
            else out = new (G[vt] || Uint8Array)(ab, off, n);
            pool[slot] = out;
            return out;
          }
          case T.TrueObj: { const o = new Boolean(true); pool.push(o); return o; }
          case T.FalseObj: { const o = new Boolean(false); pool.push(o); return o; }
          case T.NumberObj: { const o = new Number(rf64()); pool.push(o); return o; }
          case T.StringObj: { const o = new String(rstr()); pool.push(o); return o; }
          case T.BigIntObj: { const o = Object(rbig()); pool.push(o); return o; }
          case T.DOMException: { const msg = rstr(); const e = new G.DOMException(msg, rstr()); if (v14) pool.push(e); return e; }
          case T.Error: {
            const name = rstr(); const flags = ru8();
            const message = flags & 1 ? rstr() : undefined;
            const stack = flags & 2 ? rstr() : undefined;
            const e = errorFromParts(name, message, stack, false);
            if (v14) pool.push(e);
            if (flags & 4) e.cause = read();
            if (flags & 8) e.errors = read();
            return e;
          }
          case T.Blob: { const type = rstr(); const n = ru32(); const b = new G.Blob([u8v.slice(pos, pos + n)], { type }); pos += n; if (v14) pool.push(b); return b; }
          case T.File: {
            const name = rstr(); const type = rstr(); const lm = rf64(); const n = ru32();
            const f = new G.File([u8v.slice(pos, pos + n)], name, { type, lastModified: lm }); pos += n;
            if (v14) pool.push(f); return f;
          }
          case T.BunFile: {
            const name = rstr(); const type = rstr(); const lm = rf64(); const size = ru32();
            let f = null;
            try { if (G.Bun && name) f = G.Bun.file(name); } catch (e) {}
            if (!f) f = {};
            // Blob attributes are getter-only accessors, so restore the slots
            // they read (a plain-object fallback still takes data properties).
            if (f instanceof G.Blob) {
              blobSlot(f, "__name", name || undefined); blobSlot(f, "__type", type); blobSlot(f, "__lastModified", lm);
            } else {
              f.name = name || undefined; f.type = type; f.lastModified = lm; f.size = size;
            }
            if (v14) pool.push(f); return f;
          }
          case T.CryptoKey: {
            const meta = JSON.parse(rstr()); const n = ru32(); const raw = u8v.slice(pos, pos + n); pos += n;
            const k = G.CryptoKey ? new G.CryptoKey(meta.type, meta.algorithm, meta.extractable, meta.usages, raw) : meta;
            if (v14) pool.push(k); return k;
          }
          case T.KeyObject: {
            const meta = JSON.parse(rstr()); const material = rstr();
            const KO = nc().KeyObject;
            const brand = nc().__koBrand;
            let k;
            if (KO && brand) { k = new KO(brand, meta.type, meta.type === "secret" ? (G.Buffer ? G.Buffer.from(material, "base64") : material) : material, ""); }
            else if (KO && KO.from) { k = meta; }
            else k = meta;
            if (v14) pool.push(k); return k;
          }
          case T.X509: {
            const X = nc().X509Certificate;
            const subject = rstr(); const issuer = rstr();
            const c = X ? Object.assign(Object.create(X.prototype), { subject, issuer }) : { subject, issuer };
            if (v14) pool.push(c); return c;
          }
          default: throw new TypeError("deserialize: unknown tag " + tag + " at offset " + (pos - 1));
        }
      };
      return read();
    };

    const jscMod = M["bun:jsc"];
    if (jscMod) {
      jscMod.serialize = (v, opts) => serializeBytes(v, !!(opts && opts.forStorage));
      jscMod.deserialize = (b) => deserializeBytes(b);
    }
  }

  // ---- AbortController / AbortSignal (WHATWG) ----
  if (typeof G.AbortSignal === "undefined") {
    const kResistStopPropagation = Symbol.for("nodejs.event_target.resist_stop_propagation");
    const kStopImmediate = Symbol("kStopImmediate");
    class AbortSignal {
      get [Symbol.toStringTag]() { return "AbortSignal"; }
      constructor() { this.aborted = false; this.reason = undefined; this._l = []; this.onabort = null; }
      // `_l` holds {cb, once} records so `{ once: true }` registrations drop
      // themselves after firing — events.getEventListeners(signal, "abort")
      // must report 0 once the signal has been raised (node semantics).
      addEventListener(t, cb, opts) { if (t !== "abort" || typeof cb !== "function") return; for (const r of this._l) if (r.cb === cb) return; this._l.push({ cb, once: !!(opts && opts.once), resistStopPropagation: !!opts?.[kResistStopPropagation] }); }
      removeEventListener(t, cb) { if (t !== "abort") return; this._l = this._l.filter((x) => x.cb !== cb); }
      dispatchEvent(e) { if (e && e.type === "abort") this._fire(); return true; }
      throwIfAborted() { if (this.aborted) throw this.reason || new G.DOMException("signal is aborted without reason", "AbortError"); }
      // A listener that throws must NOT abort the dispatch or escape into
      // abort()'s caller: node's EventTarget reports it as an uncaught
      // exception on the next tick and carries on with the remaining
      // listeners (internal/event_target.js emitUncaughtException).
      _fire() {
        const ev = {
          type: "abort", target: this, currentTarget: this, cancelBubble: false,
          stopPropagation() { this.cancelBubble = true; },
          stopImmediatePropagation() { this.cancelBubble = true; this[kStopImmediate] = true; },
        };
        const report = (err) => {
          const p = G.process;
          if (p && typeof p.nextTick === "function") p.nextTick(() => { throw err; });
          else throw err;
        };
        if (typeof this.onabort === "function") { try { this.onabort.call(this, ev); } catch (err) { report(err); } }
        for (const r of this._l.slice()) {
          if (ev[kStopImmediate] && !r.resistStopPropagation) continue;
          if (r.once) this.removeEventListener("abort", r.cb);
          try { r.cb.call(this, ev); } catch (err) { report(err); }
        }
        ev.currentTarget = null;
      }
      static abort(reason) { const s = new AbortSignal(); s.aborted = true; s.reason = reason !== undefined ? reason : new G.DOMException("The operation was aborted.", "AbortError"); return s; }
      // `__mbunAbortAt` records the deadline as a wall-clock instant. A purely
      // synchronous native that has to honour a signal (Bun.spawnSync) cannot
      // run the timer that would fire this signal, so it reads the deadline
      // directly and applies it as its own timeout instead.
      static timeout(ms) {
        const s = new AbortSignal();
        Object.defineProperty(s, "__mbunAbortAt", {
          value: Date.now() + (Number(ms) || 0),
          enumerable: false, configurable: true, writable: true,
        });
        if (G.setTimeout) {
          // Node's timeout signal is not retained by its timer: otherwise an
          // otherwise-unreachable signal cannot be collected, and a long
          // timeout keeps an unrelated process alive.
          const signalRef = new G.WeakRef(s);
          const timer = G.setTimeout(() => {
            const signal = signalRef.deref();
            if (!signal || signal.aborted) return;
            signal.aborted = true;
            signal.reason = new G.DOMException("The operation was aborted due to timeout", "TimeoutError");
            signal._fire();
          }, ms);
          if (timer && typeof timer.unref === "function") timer.unref();
        }
        return s;
      }
      static any(signals) { const s = new AbortSignal(); for (const sig of signals) { if (sig.aborted) { s.aborted = true; s.reason = sig.reason; return s; } sig.addEventListener("abort", () => { if (!s.aborted) { s.aborted = true; s.reason = sig.reason; s._fire(); } }); } return s; }
      // WebCore AbortSignal::memoryCost() includes m_algorithms.sizeInBytes();
      // mbun's algorithm list is `_l` (std::pair<uint32_t, Function> ≈ 16 bytes
      // per entry on 64-bit). Read by bun:jsc's estimateShallowMemoryUsageOf so
      // an abort-algorithm leak is observable exactly as it is in bun.
      [Symbol.for("mbun.memoryCost")]() { return this._l.length * 16; }
    }
    // Non-enumerable introspection hook mirroring EventTarget.prototype.listeners:
    // events.getEventListeners(signal, "abort") probes for a callable `listeners`.
    Object.defineProperty(AbortSignal.prototype, "listeners", {
      value: function listeners(type) { return String(type) === "abort" ? this._l.map((r) => r.cb) : []; },
      writable: true, configurable: true, enumerable: false,
    });
    G.AbortSignal = AbortSignal;
  }
  if (typeof G.AbortController === "undefined") {
    G.AbortController = class AbortController {
      get [Symbol.toStringTag]() { return "AbortController"; }
      constructor() { this.signal = new G.AbortSignal(); }
      abort(reason) { if (this.signal.aborted) return; this.signal.aborted = true; this.signal.reason = reason !== undefined ? reason : new G.DOMException("The operation was aborted.", "AbortError"); this.signal._fire(); }
    };
  }

  // SharedArrayBuffer: enabled as a real, distinct JSC type (useSharedArrayBuffer
  // option, set at engine init) so it is no longer aliased to ArrayBuffer — node
  // assert deepEqual and util.types can tell the two apart. The runtime is still
  // single-threaded, so Atomics.wait is emulated below. The alias remains only as a
  // fallback for a JSC build where the option is unavailable.
  if (typeof G.SharedArrayBuffer === "undefined") G.SharedArrayBuffer = G.ArrayBuffer;
  if (typeof G.Atomics !== "undefined" && typeof G.Atomics.waitAsync !== "function") { try { G.Atomics.waitAsync = (ta, index, value, timeout) => (ta[index] !== value ? { async: false, value: "not-equal" } : { async: true, value: Promise.resolve("ok") }); } catch (e) {} }
  // Atomics.wait: SharedArrayBuffer is aliased to ArrayBuffer (single-threaded), so native wait
  // rejects the non-shared backing store. Emulate: mismatched value → "not-equal"; a matching value
  // can never be notified in a single-threaded runtime → "timed-out".
  if (typeof G.Atomics !== "undefined") { try { G.Atomics.wait = (ta, index, value, timeout) => (ta[index] !== value ? "not-equal" : "timed-out"); } catch (e) {} }

  // ---- MessageChannel / MessagePort globals (in-process, synchronous delivery) ----
  if (typeof G.MessagePort === "undefined") {
    G.MessagePort = class MessagePort extends EventEmitter {
      constructor() { super(); this._other = null; this.onmessage = null; }
      postMessage(data, transfer) { if (Array.isArray(transfer)) for (const e of transfer) { if (e instanceof ArrayBuffer && !e.detached) { try { e.transfer(); } catch (er) {} } } const o = this._other; if (o) G.queueMicrotask(() => { const ev = { data, type: "message" }; if (typeof o.onmessage === "function") o.onmessage(ev); o.emit("message", ev); }); }
      start() {} close() { this.emit("close"); } ref() { return this; } unref() { return this; }
      addEventListener(t, cb) { this.on(t, cb); } removeEventListener(t, cb) { this.off(t, cb); }
    };
  }
  if (typeof G.MessageChannel === "undefined") {
    G.MessageChannel = class MessageChannel { constructor() { this.port1 = new G.MessagePort(); this.port2 = new G.MessagePort(); this.port1._other = this.port2; this.port2._other = this.port1; } };
  }

  // ---- File (WHATWG: Blob + name/lastModified) ----
  if (typeof G.File === "undefined") {
    G.File = class File extends G.Blob {
      // name/lastModified go to the Blob.prototype-backed slots. bun has NO
      // webkitRelativePath on File at all (it is undefined there), so defining
      // it would both diverge and leak an own key into JSON.stringify.
      constructor(parts, name, opts) { super(parts, opts); blobSlot(this, "__name", String(name)); blobSlot(this, "__lastModified", opts && opts.lastModified !== undefined ? Number(opts.lastModified) : Date.now()); }
      get [Symbol.toStringTag]() { return "File"; }
    };
  }

  // ---- DOMException (WHATWG: name → legacy code + standard constants) ----
  if (typeof G.DOMException === "undefined") {
    const CODES = { INDEX_SIZE_ERR: 1, DOMSTRING_SIZE_ERR: 2, HIERARCHY_REQUEST_ERR: 3, WRONG_DOCUMENT_ERR: 4, INVALID_CHARACTER_ERR: 5, NO_DATA_ALLOWED_ERR: 6, NO_MODIFICATION_ALLOWED_ERR: 7, NOT_FOUND_ERR: 8, NOT_SUPPORTED_ERR: 9, INUSE_ATTRIBUTE_ERR: 10, INVALID_STATE_ERR: 11, SYNTAX_ERR: 12, INVALID_MODIFICATION_ERR: 13, NAMESPACE_ERR: 14, INVALID_ACCESS_ERR: 15, VALIDATION_ERR: 16, TYPE_MISMATCH_ERR: 17, SECURITY_ERR: 18, NETWORK_ERR: 19, ABORT_ERR: 20, URL_MISMATCH_ERR: 21, QUOTA_EXCEEDED_ERR: 22, TIMEOUT_ERR: 23, INVALID_NODE_TYPE_ERR: 24, DATA_CLONE_ERR: 25 };
    const NAME_TO_CODE = { IndexSizeError: 1, HierarchyRequestError: 3, WrongDocumentError: 4, InvalidCharacterError: 5, NoModificationAllowedError: 7, NotFoundError: 8, NotSupportedError: 9, InUseAttributeError: 10, InvalidStateError: 11, SyntaxError: 12, InvalidModificationError: 13, NamespaceError: 14, InvalidAccessError: 15, TypeMismatchError: 17, SecurityError: 18, NetworkError: 19, AbortError: 20, URLMismatchError: 21, QuotaExceededError: 22, TimeoutError: 23, InvalidNodeTypeError: 24, DataCloneError: 25 };
    class DOMException extends Error {
      constructor(message, name) {
        let nm = name, hasCause = false, cause;
        if (name !== null && typeof name === "object") { nm = name.name; if ("cause" in name) { hasCause = true; cause = name.cause; } }
        super(message);
        this.name = nm ? String(nm) : "Error";
        this.message = message === undefined ? "" : String(message);
        this.code = NAME_TO_CODE[this.name] || 0;
        if (hasCause) this.cause = cause;
        this.stack = undefined;  // bun/WHATWG DOMException has no stack trace (yet)
      }
      get [Symbol.toStringTag]() { return "DOMException"; }
    }
    for (const k of Object.keys(CODES)) { Object.defineProperty(DOMException, k, { value: CODES[k], enumerable: true }); Object.defineProperty(DOMException.prototype, k, { value: CODES[k], enumerable: true }); }
    G.DOMException = DOMException;
  }

  // ---- QuotaExceededError (WHATWG webidl §QuotaExceededError) ----
  // A DOMException subclass with a fixed name/code plus the nullable `quota`
  // and `requested` telemetry attributes. `crypto.getRandomValues` and the
  // storage APIs throw it, and the corpus asserts `err instanceof
  // QuotaExceededError`, so it has to be a real global constructor rather than
  // a DOMException carrying the name.
  if (typeof G.QuotaExceededError === "undefined" && typeof G.DOMException === "function") {
    const kQuota = Symbol("quota");
    const kRequested = Symbol("requested");
    // `double?`: absent/undefined is null; anything else is a number that must
    // not be negative (webidl throws RangeError, not TypeError, for that).
    const toQuotaDouble = (value, label) => {
      if (value === undefined || value === null) return null;
      const n = Number(value);
      if (!(n >= 0)) throw new RangeError(`${label} must be a non-negative number`);
      return n;
    };
    class QuotaExceededError extends G.DOMException {
      constructor(message, options) {
        super(message === undefined ? "" : message, "QuotaExceededError");
        let quota = null, requested = null;
        if (options !== undefined && options !== null) {
          if (typeof options !== "object" && typeof options !== "function")
            throw new TypeError("QuotaExceededErrorOptions is not an object");
          quota = toQuotaDouble(options.quota, "options.quota");
          requested = toQuotaDouble(options.requested, "options.requested");
        }
        Object.defineProperty(this, kQuota, { value: quota });
        Object.defineProperty(this, kRequested, { value: requested });
      }
      get quota() { return this[kQuota]; }
      get requested() { return this[kRequested]; }
      get [Symbol.toStringTag]() { return "QuotaExceededError"; }
    }
    G.QuotaExceededError = QuotaExceededError;
  }

  // ---- V8 stack-trace API (Error.captureStackTrace / prepareStackTrace / CallSite) ----
  // JSC stacks are "name@file:line:col"; V8 (node/bun) are "    at name (file:line:col)".
  // Parse JSC lines into CallSite objects, re-format V8-style, and route through
  // Error.prepareStackTrace when the user replaces it (V8 contract). JSC ships a
  // native captureStackTrace but it emits JSC format and ignores
  // prepareStackTrace — override it. Instance `.stack` stays JSC-format (own
  // property materialized at construction; needs native work — DEFERRED).
  {
    class CallSite {
      constructor(name, file, line, col, kind) {
        this._n = name || null; this._f = file || null; this._l = line; this._c = col;
        this._k = kind || "";
        // JSC spells a method frame "Type.method"; V8 splits it across
        // getTypeName()/getMethodName(). Nothing else about the receiver
        // survives into a JSC stack string, so anything we cannot read off the
        // frame name stays null rather than being invented.
        const dot = this._n === null ? -1 : this._n.lastIndexOf(".");
        this._t = dot > 0 ? this._n.slice(0, dot) : null;
        this._m = dot > 0 ? this._n.slice(dot + 1) : null;
      }
      get [Symbol.toStringTag]() { return "CallSite"; }
      getFunctionName() { return this._n; }
      getMethodName() { return this._m; }
      getTypeName() { return this._t; }
      getFileName() { return this._f; }
      getScriptNameOrSourceURL() { return this._f; }
      getLineNumber() { return this._l; }
      getColumnNumber() { return this._c; }
      getEnclosingLineNumber() { return this._l; }
      getEnclosingColumnNumber() { return this._c; }
      getEvalOrigin() { return undefined; }
      // V8 hands the formatter the frame's receiver; a JSC stack string does not
      // carry it (and V8 itself reports undefined for a strict-mode frame), so
      // undefined is the honest answer for every frame rather than a fake.
      getThis() { return undefined; }
      getFunction() { return undefined; }
      getPosition() { return 0; }
      getScriptHash() { return ""; }
      getPromiseIndex() { return null; }
      isEval() { return this._k === "eval"; }
      isNative() { return this._k === "native"; }
      isConstructor() { return false; }
      isAsync() { return false; }
      isPromiseAll() { return false; }
      isToplevel() { return this._n === null; }
      toString() {
        const loc = this._f ? (this._l != null ? this._f + ":" + this._l + ":" + this._c : this._f) : "unknown";
        return this._n ? this._n + " (" + loc + ")" : loc;
      }
    }
    const parseFrames = (raw) => {
      const out = [];
      for (const ln of String(raw || "").split("\n")) {
        if (!ln) continue;
        const at = ln.lastIndexOf("@");
        if (at < 0) continue;
        let name = ln.slice(0, at);
        let kind = "";
        if (name === "eval code") { kind = "eval"; name = ""; }
        else if (name === "global code" || name === "module code") name = "";
        const loc = ln.slice(at + 1);
        if (loc === "[native code]" || loc === "native") kind = "native";
        const m = loc.match(/^(.*):(\d+):(\d+)$/);
        if (m) out.push(new CallSite(name, m[1], +m[2], +m[3], kind));
        else out.push(new CallSite(name, loc || null, undefined, undefined, kind));
      }
      const limit = Error.stackTraceLimit;
      return typeof limit === "number" && limit >= 0 && out.length > limit ? out.slice(0, limit) : out;
    };
    const header = (err) => {
      let name = "Error", msg = "";
      try { name = err && err.name !== undefined ? String(err.name) : "Error"; } catch (e) {}
      try { msg = err && err.message ? String(err.message) : ""; } catch (e) {}
      return msg ? name + ": " + msg : name;
    };
    const defaultPrepare = function (err, frames) {
      let s = header(err);
      if (Array.isArray(frames)) for (const f of frames) s += "\n    at " + String(f);
      return s;
    };
    // ── Error.prepareStackTrace ────────────────────────────────────────────
    // V8 calls the formatter LAZILY, on the first read of `.stack`, and its
    // return value BECOMES `.stack`. This JSC gives every error an own `stack`
    // DATA property at construction — there is no `Error.prototype.stack`
    // accessor to wrap — so laziness has to be installed per instance, at
    // construction, by re-defining that own property as a getter.
    //
    // Doing that for every error unconditionally would tax the whole runtime
    // for a V8-ism almost nothing uses, and would silently move mbun off the
    // JSC-format `.stack` it deliberately keeps (see the note above). So the
    // machinery is DORMANT until someone actually installs a formatter:
    // `Error.prepareStackTrace` is an accessor whose setter arms it once. Code
    // that never touches prepareStackTrace sees byte-identical behaviour.
    const kRaw = Symbol("mbunRawStack");
    const ownStackDesc = (obj) => {
      try { return Object.getOwnPropertyDescriptor(obj, "stack"); } catch (e) { return undefined; }
    };
    // The raw JSC stack of an error, WITHOUT running any user formatter — used
    // by everything internal that needs frames (captureStackTrace, the
    // node_modules call-site probe) so a user formatter can neither observe nor
    // break mbun's own captures.
    const rawStack = (err) => {
      try {
        if (err !== null && typeof err === "object" && kRaw in err) return err[kRaw];
        const d = ownStackDesc(err);
        return d && typeof d.get !== "function" ? d.value : undefined;
      } catch (e) { return undefined; }
    };
    // Published so other builtins that must walk frames WITHOUT triggering a
    // user formatter can do so (util.getCallSites is contractually one of them:
    // test-util-getcallsites-preparestacktrace asserts it never calls it).
    try { G[Symbol.for("mbun.rawErrorStack")] = rawStack; } catch (e) {}
    // Turn the own data `stack` into V8's lazy accessor. The formatter runs at
    // most once per error and its result is cached, exactly as V8 memoises.
    const armLazyStack = (err) => {
      const d = ownStackDesc(err);
      if (!d || !d.configurable || typeof d.get === "function") return err;
      const raw = d.value;
      let cached, computed = false;
      try {
        Object.defineProperty(err, kRaw, { value: raw, configurable: true });
        Object.defineProperty(err, "stack", {
          configurable: true,
          enumerable: false,
          get() {
            if (computed) return cached;
            const prep = Error.prepareStackTrace;
            // No formatter (or the built-in one): keep the JSC-format string
            // mbun reports everywhere else. Only a user formatter changes shape.
            if (typeof prep !== "function" || prep === defaultPrepare) return raw;
            // Latch BEFORE calling out: the formatter may read `.stack` again
            // (directly, or via console/inspect), and V8 does not re-enter.
            computed = true;
            cached = raw;
            // A throwing formatter propagates, as in V8; the cached JSC string
            // stays in place so the next read cannot re-enter the thrower.
            cached = prep(this, parseFrames(raw));
            return cached;
          },
          set(v) { computed = true; cached = v; },
        });
      } catch (e) { /* frozen/sealed error: leave it alone */ }
      return err;
    };
    // Swapping the global error constructors for construct-trapping proxies
    // preserves identity that a hand-written wrapper would not: `.prototype`,
    // `instanceof`, `class X extends Error`, and every static (including
    // captureStackTrace and prepareStackTrace itself) forward to the original.
    let armed = false;
    const armErrorConstructors = () => {
      if (armed) return;
      armed = true;
      const handler = {
        construct(target, args, newTarget) {
          return armLazyStack(Reflect.construct(target, args, newTarget));
        },
        apply(target, thisArg, args) {
          const r = Reflect.apply(target, thisArg, args);
          return r !== null && typeof r === "object" ? armLazyStack(r) : r;
        },
      };
      for (const name of ["Error", "EvalError", "RangeError", "ReferenceError",
                          "SyntaxError", "TypeError", "URIError", "AggregateError"]) {
        const ctor = G[name];
        if (typeof ctor !== "function") continue;
        try { G[name] = new Proxy(ctor, handler); } catch (e) { /* non-writable global */ }
      }
    };
    let prepareValue = defaultPrepare;
    Object.defineProperty(Error, "prepareStackTrace", {
      configurable: true,
      enumerable: false,
      get() { return prepareValue; },
      set(v) {
        prepareValue = v;
        if (typeof v === "function" && v !== defaultPrepare) armErrorConstructors();
      },
    });
    Error.captureStackTrace = function (obj, skip) {
      // capture the raw JSC stack directly (never through a lazy getter, so a
      // user formatter cannot recurse into or hijack this internal capture)
      let raw = "", frames = [];
      try { raw = rawStack(new Error()); } catch (e) {}
      frames = parseFrames(raw).slice(1);  // drop the captureStackTrace frame itself
      if (typeof skip === "function" && skip.name) {
        const i = frames.findIndex((f) => f.getFunctionName() === skip.name);
        if (i >= 0) frames = frames.slice(i + 1);
      }
      const prep = Error.prepareStackTrace;
      const val = typeof prep === "function" ? prep(obj, frames) : defaultPrepare(obj, frames);
      Object.defineProperty(obj, "stack", { value: val, writable: true, configurable: true, enumerable: false });
    };
    Error.stackTraceLimit = Error.stackTraceLimit || 10;
  }

)JS";

}  // namespace mbun::jsc::builtins::detail
