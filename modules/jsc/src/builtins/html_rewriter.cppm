// HTMLRewriter payload partition. The user-facing class over the native
// __mbunHTMLRewriterNative.transform (runtime/html_rewriter.inc), which drives
// the vendored Lexbor engine. Blueprint: bun's HTMLRewriter surface
// (bun-ref packages/bun-types/html-rewriter.d.ts + src/runtime/api/html_rewriter.rs).
//
// new HTMLRewriter().on(sel,{element,comments,text})
//                   .onDocument({doctype,comments,text,end})
//                   .transform(string|ArrayBuffer|TypedArray|Response|Blob)
//
// - string  -> string (native, synchronous)
// - ArrayBuffer/TypedArray -> ArrayBuffer (native, synchronous)
// - Response/Blob/Bun.file -> Response whose body is the rewritten bytes
//   (built lazily via a ReadableStream so transform() returns synchronously,
//   matching bun's Response-returning overload).
export module mbun.jsc.js_builtins:html_rewriter;

import std;

export namespace mbun::jsc::builtins::detail {

// Appended AFTER the master builtins IIFE (image_closure), so this is a
// self-contained IIFE that re-binds G = globalThis and installs the global.
inline constexpr std::string_view kHTMLRewriterJS = R"JS(
(function () {
  const G = globalThis;
  const N = G.__mbunHTMLRewriterNative;
  if (!N || typeof N.transform !== "function") return;

  const TD = G.TextDecoder ? new G.TextDecoder() : null;
  const decodeBytes = (u8) => (TD ? TD.decode(u8) : String.fromCharCode.apply(null, u8));
  const encodeUtf8 = (s) => (G.TextEncoder ? new G.TextEncoder().encode(s)
                                            : Uint8Array.from(unescape(encodeURIComponent(s)), (c) => c.charCodeAt(0)));

  // Extract the input body as a string *synchronously* when it is already in
  // memory (a string/Uint8Array-backed Response, or an in-memory Blob); returns
  // undefined for file/stream-backed bodies (which must be read asynchronously).
  const syncBodyString = (input) => {
    // Response stores its byte body on `_b` (string | Uint8Array | Blob-like);
    // a Bun.file-backed Blob is flagged __isBunFile (async — skip).
    let b = input && input._b !== undefined ? input._b : input;
    if (b == null) {
      // A Response with no body (or a null body) rewrites the empty string.
      if (input && input._b === undefined && !(G.Blob && input instanceof G.Blob)) return undefined;
      return "";
    }
    if (typeof b === "string") return b;
    if (b instanceof Uint8Array) return decodeBytes(b);
    if (b.__isBunFile) return undefined;                 // Bun.file → async
    if (b._u8 instanceof Uint8Array) return decodeBytes(b._u8); // in-memory Blob
    if (b instanceof ArrayBuffer) return decodeBytes(new Uint8Array(b));
    if (ArrayBuffer.isView(b)) return decodeBytes(new Uint8Array(b.buffer, b.byteOffset, b.byteLength));
    return undefined;
  };

  // Bind a handler method to its handler object so `this` is the handler inside
  // the callback (bun invokes handlers as methods; a class-instance handler's
  // `element(el){ ...this.content... }` relies on it).
  const bindFn = (obj, name) => {
    const f = obj[name];
    return typeof f === "function" ? f.bind(obj) : f;
  };

  const respInit = (input) => {
    const init = {};
    if (input.status !== undefined) init.status = input.status;
    if (input.statusText !== undefined && input.statusText !== "") init.statusText = input.statusText;
    if (input.headers !== undefined) init.headers = input.headers;
    return init;
  };

  class HTMLRewriter {
    constructor() {
      // Handler spec entries handed to the native transform(); one per on()/
      // onDocument() call, in registration order (Lexbor applies them in order).
      Object.defineProperty(this, "_handlers", { value: [], enumerable: false, writable: false });
    }

    on(selector, handlers) {
      if (typeof selector !== "string") selector = String(selector);
      if (handlers == null || typeof handlers !== "object")
        throw new TypeError("Expected object as the second argument to HTMLRewriter.on");
      // bun rejects invalid selectors eagerly at .on() (not lazily at transform).
      if (typeof N.validateSelector === "function") N.validateSelector(selector);
      this._handlers.push({
        selector,
        element: bindFn(handlers, "element"),
        comments: bindFn(handlers, "comments"),
        text: bindFn(handlers, "text"),
      });
      return this;
    }

    onDocument(handlers) {
      if (handlers == null || typeof handlers !== "object")
        throw new TypeError("Expected object as the argument to HTMLRewriter.onDocument");
      this._handlers.push({
        document: true,
        doctype: bindFn(handlers, "doctype"),
        comments: bindFn(handlers, "comments"),
        text: bindFn(handlers, "text"),
        end: bindFn(handlers, "end"),
      });
      return this;
    }

    transform(input) {
      const handlers = this._handlers;

      if (typeof input === "string") {
        return N.transform(handlers, input);
      }

      const isAB = input instanceof ArrayBuffer;
      const isView = ArrayBuffer.isView(input);
      if (isAB || isView) {
        const u8 = isAB ? new Uint8Array(input)
                        : new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
        const out = N.transform(handlers, decodeBytes(u8));
        return encodeUtf8(out).buffer;
      }

      if (input === null || input === undefined || typeof input !== "object") {
        throw new TypeError("Expected Response or Body");
      }
      if (!G.Response) {
        throw new TypeError("HTMLRewriter.transform: unsupported input (Response unavailable)");
      }

      // Response | Blob | Bun.file. bun processes an in-memory body eagerly (so a
      // throwing handler throws synchronously, matching its Response overload) and
      // an async body (Bun.file / a live stream) lazily. Detect the sync case by
      // peeking the Response's stored byte body (this._b) or a Blob's bytes.
      const syncStr = syncBodyString(input);
      const init = respInit(input);
      if (syncStr !== undefined) {
        const out = N.transform(handlers, syncStr); // handler exceptions propagate
        return new G.Response(out, init);
      }

      // Async body: rewrite lazily inside a ReadableStream so transform() returns
      // synchronously; a throwing handler surfaces as a stream error (not a sync
      // throw), matching bun's behaviour for file/stream-backed inputs.
      if (!G.ReadableStream) {
        throw new TypeError("HTMLRewriter.transform: unsupported streaming input (ReadableStream unavailable)");
      }
      const rs = new G.ReadableStream({
        async start(controller) {
          try {
            const buf = await input.arrayBuffer();
            const out = N.transform(handlers, decodeBytes(new Uint8Array(buf)));
            controller.enqueue(encodeUtf8(out));
            controller.close();
          } catch (e) {
            controller.error(e);
          }
        },
      });
      return new G.Response(rs, init);
    }
  }

  Object.defineProperty(HTMLRewriter, "name", { value: "HTMLRewriter", configurable: true });
  G.HTMLRewriter = HTMLRewriter;
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
