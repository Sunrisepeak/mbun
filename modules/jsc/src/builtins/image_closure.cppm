// Image and IIFE closure payload partition; keep raw bytes aligned with js_builtins.cppm lines 7696-7872.
export module mbun.jsc.js_builtins:image_closure;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kImageClosureJS = R"JS(  // ---- Bun.Image (builder over __mbunImageNative; Image.rs port) ----
  // Sharp-shaped: chainable setters each write one slot (calling twice
  // overwrites, not repeats); terminals run one native pipeline call.
  if (G.Bun && typeof G.Bun.Image === "undefined" && G.__mbunImageNative) {
    const IN = G.__mbunImageNative;
    const iU8ToB64 = (u8) => { let s = ""; for (let i = 0; i < u8.length; i += 8192) s += String.fromCharCode.apply(null, u8.subarray(i, Math.min(i + 8192, u8.length))); return G.btoa(s); };
    const iB64ToU8 = (b) => { const s = G.atob(b); const u = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) u[i] = s.charCodeAt(i); return u; };
    // coerceInt semantics from Image.rs: NaN → lo, ±Inf → matching bound.
    const iCoerce = (x, lo, hi) => { x = Number(x); return Number.isNaN(x) ? lo : Math.min(Math.max(x, lo), hi); };
    // Native throws are strings; a "SOME_CODE: message" prefix becomes err.code.
    const iErr = (e) => {
      if (e instanceof Error) return e;
      const s = String(e);
      const m = /^([A-Z][A-Z0-9_]{4,}): (.*)$/s.exec(s);
      if (m) { const err = new Error(m[2]); err.code = m[1]; return err; }
      return new Error(s);
    };
    const iMime = { png: "image/png", jpeg: "image/jpeg", webp: "image/webp", heic: "image/heic", avif: "image/avif", gif: "image/gif", bmp: "image/bmp", tiff: "image/tiff" };
    const iFilters = new Set(["box", "bilinear", "linear", "lanczos3", "mitchell", "nearest", "cubic", "lanczos2", "mks2013", "mks2021"]);
    const iReadBin = (p) => {
      const st = G.__mbunFsNative.stat(p); // throws ENOENT for missing files
      const FD = G.__mbunFdNative;
      const fd = FD.open(p, "r", 0o666);
      try { const u = new Uint8Array(st.size); FD.read(fd, u, 0, u.byteLength, -1); return u; } finally { FD.close(fd); }
    };
    let iBackend = "bun"; // linux default; macOS/Windows would default "system"
    class BunImage {
      constructor(input, opts) {
        this._max = 0x3FFF * 0x3FFF; this._autoOrient = true;
        this._rotate = 0; this._flip = false; this._flop = false;
        this._resize = null; this._modulate = null; this._out = null;
        this._w = -1; this._h = -1;
        if (input === undefined || input === null) throw new Error("Image() expects a path, ArrayBuffer, TypedArray, Blob or data: URL");
        if (opts !== undefined && opts !== null && typeof opts === "object") {
          const mp = opts.maxPixels; // a throwing getter must surface
          if (typeof mp === "number") this._max = iCoerce(mp, 0, 1e15);
          if (opts.autoOrient !== undefined) this._autoOrient = !!opts.autoOrient;
        }
        if (typeof input === "string") {
          if (input.startsWith("data:")) {
            const comma = input.indexOf(",");
            if (comma < 0) throw new Error("Image(): malformed data: URL (no comma)");
            if (input.slice(0, comma).indexOf(";base64") < 0) throw new Error("Image(): only base64 data: URLs are supported");
            this._srcU8 = iB64ToU8(input.slice(comma + 1));
          } else this._srcPath = input;
        } else if (ArrayBuffer.isView(input)) this._srcU8 = new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
        else if (input instanceof ArrayBuffer) this._srcU8 = new Uint8Array(input);
        else if (typeof G.Blob !== "undefined" && input instanceof G.Blob) this._srcBlob = input;
        else throw new Error("Image() input must be a path string, data: URL, ArrayBuffer, TypedArray or Blob");
      }
      get width() { return this._w; }
      get height() { return this._h; }
      resize(w, h, o) {
        if (typeof w !== "number") throw new Error("resize(width, height?, options?)");
        const r = { w: iCoerce(w, 1, 0x3FFFF), h: typeof h === "number" ? iCoerce(h, 0, 0x3FFFF) : 0, filter: "lanczos3", fitInside: false, withoutEnlargement: false };
        if (o !== undefined && o !== null && typeof o === "object") {
          if (o.filter !== undefined) { const f = String(o.filter); if (!iFilters.has(f)) throw new Error("filter must be 'box', 'bilinear', 'linear', 'lanczos3', 'mitchell', 'nearest', 'cubic', 'lanczos2', 'mks2013' or 'mks2021'"); r.filter = f; }
          if (o.fit !== undefined) { const f = String(o.fit); if (f !== "fill" && f !== "inside") throw new Error("fit must be 'fill' or 'inside'"); r.fitInside = f === "inside"; }
          if (o.withoutEnlargement !== undefined) r.withoutEnlargement = !!o.withoutEnlargement;
        }
        this._resize = r; return this;
      }
      rotate(d) {
        if (typeof d !== "number") throw new Error("rotate(degrees) expects 90, 180 or 270");
        const deg = ((Math.trunc(iCoerce(d, -1e15, 1e15)) % 360) + 360) % 360;
        if (deg !== 0 && deg !== 90 && deg !== 180 && deg !== 270) throw new Error("rotate: only multiples of 90 are supported");
        this._rotate = deg; return this;
      }
      flip() { this._flip = true; return this; }
      flop() { this._flop = true; return this; }
      modulate(o) {
        const m = this._modulate || { brightness: 1, saturation: 1 };
        if (o !== undefined && o !== null && typeof o === "object") {
          if (typeof o.brightness === "number") m.brightness = Number.isFinite(o.brightness) ? Math.min(Math.max(o.brightness, 0), 1e4) : 1;
          if (typeof o.saturation === "number") m.saturation = Number.isFinite(o.saturation) ? Math.min(Math.max(o.saturation, 0), 1e4) : 1;
        }
        this._modulate = m; return this;
      }
      _fmt(fmt, o) {
        const e = this._out || { format: fmt, quality: 80, lossless: false, compressionLevel: -1, palette: false, colors: 256, dither: false, progressive: false };
        e.format = fmt;
        if (o !== undefined && o !== null && typeof o === "object") {
          if (typeof o.quality === "number") e.quality = iCoerce(o.quality, 1, 100);
          if (o.lossless !== undefined) e.lossless = !!o.lossless;
          if (typeof o.compressionLevel === "number") e.compressionLevel = iCoerce(o.compressionLevel, 0, 9);
          if (o.palette !== undefined) e.palette = !!o.palette;
          if (typeof o.colors === "number") e.colors = iCoerce(o.colors, 2, 256);
          if (o.dither !== undefined) e.dither = !!o.dither;
          if (o.progressive !== undefined) e.progressive = !!o.progressive;
        }
        this._out = e; return this;
      }
      png(o) { return this._fmt("png", o); }
      jpeg(o) { return this._fmt("jpeg", o); }
      webp(o) { return this._fmt("webp", o); }
      heic(o) { return this._fmt("heic", o); }
      avif(o) { return this._fmt("avif", o); }
      async _src() {
        if (this._srcU8) return this._srcU8;
        if (this._srcBlob) { this._srcU8 = new Uint8Array(await this._srcBlob.arrayBuffer()); return this._srcU8; }
        this._srcU8 = iReadBin(this._srcPath); return this._srcU8;
      }
      async _run() {
        const src = await this._src();
        const out = this._out;
        const fmt = out ? out.format : "";
        if (fmt === "heic" || fmt === "avif") {
          // Static codecs never cover HEVC/AV1 — the bun backend rejects with
          // a stable code on every platform (Image.rs Linux contract).
          const err = new Error("Image: " + fmt + " encode requires macOS or Windows");
          err.code = "ERR_IMAGE_FORMAT_UNSUPPORTED";
          throw err;
        }
        let r;
        try {
          r = IN.pipeline(iU8ToB64(src), {
            maxPixels: this._max, rotate: this._rotate, flip: this._flip, flop: this._flop,
            hasResize: !!this._resize,
            rw: this._resize ? this._resize.w : 0, rh: this._resize ? this._resize.h : 0,
            filter: this._resize ? this._resize.filter : "lanczos3",
            fitInside: this._resize ? this._resize.fitInside : false,
            withoutEnlargement: this._resize ? this._resize.withoutEnlargement : false,
            hasModulate: !!this._modulate,
            brightness: this._modulate ? this._modulate.brightness : 1,
            saturation: this._modulate ? this._modulate.saturation : 1,
            output: fmt, palette: out ? out.palette : false, colors: out ? out.colors : 256,
            dither: out ? out.dither : false, compressionLevel: out ? out.compressionLevel : -1,
            quality: out ? out.quality : 80, progressive: out ? out.progressive : false,
            lossless: out ? out.lossless : false,
          });
        } catch (e) { throw iErr(e); }
        this._w = r.width; this._h = r.height;
        return { u8: iB64ToU8(r.b64), format: r.format };
      }
      async bytes() { return (await this._run()).u8; }
      async buffer() { const u = (await this._run()).u8; return G.Buffer ? G.Buffer.from(u.buffer, 0, u.byteLength) : u; }
      toBuffer() { return this.buffer(); }
      async blob() { const r = await this._run(); return new G.Blob([r.u8], { type: iMime[r.format] || "application/octet-stream" }); }
      async toBase64() { return iU8ToB64((await this._run()).u8); }
      async dataurl() { const r = await this._run(); return "data:" + (iMime[r.format] || "application/octet-stream") + ";base64," + iU8ToB64(r.u8); }
      async write(dest) { const r = await this._run(); return G.Bun.write(dest, r.u8); }
      async metadata() {
        const src = await this._src();
        let m;
        try { m = IN.metadata(iU8ToB64(src), this._max); } catch (e) { throw iErr(e); }
        this._w = m.width; this._h = m.height;
        return { width: m.width, height: m.height, format: m.format };
      }
      placeholder(as) {
        // Sync arg validation (bun throws before the async pipeline starts).
        if (as !== undefined && as !== "dataurl") throw new Error("placeholder(): only 'dataurl' is supported");
        return this._placeholder();
      }
      async _placeholder() {
        const src = await this._src();
        let r;
        try { r = IN.placeholder(iU8ToB64(src), this._max); } catch (e) { throw iErr(e); }
        return "data:image/png;base64," + r.b64;
      }
    }
    Object.defineProperty(BunImage, "backend", {
      get: () => iBackend,
      set: (v) => { if (v !== "bun" && v !== "system") throw new TypeError("Bun.Image.backend must be 'bun' or 'system'"); iBackend = v; },
      configurable: true,
    });
    BunImage.hasClipboardImage = () => false; // linux: no clipboard backend
    BunImage.clipboardChangeCount = () => -1;
    BunImage.fromClipboard = () => null;
    G.Bun.Image = BunImage;
    if (typeof G.Blob !== "undefined" && typeof G.Blob.prototype.image === "undefined") {
      G.Blob.prototype.image = function (opts) { return new BunImage(this, opts); };
    }
  }

  // ---- catch-all: every name in BUILTIN_LIST resolves (node stub parity) ----
  for (const n of BUILTIN_LIST) if (!(n in M)) def([n], {});
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
