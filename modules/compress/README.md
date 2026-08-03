# mbun.compress

Bun's compression subsystem, split by the Bun source boundaries:

- `deflate.cppm`: RFC 1951 raw DEFLATE (encode/decode);
- `zlib.cppm` and `gzip.cppm`: RFC 1950/1952 framing, header validation, and
  compress/decompress;
- `brotli.cppm` and `zstd.cppm`: Brotli and Zstandard codecs;
- `libdeflate_sys.cppm`: the narrow raw/zlib/gzip seam (zlib-backed);
- `zlib_native.cppm`: the shared zlib backend (owns the `<zlib.h>` include);
- `types.cppm`: shared byte/result/error vocabulary.

## Native backends (wired)

The codecs are backed by real native libraries compiled from upstream source
via the mbun local index (`mcpp/pkgs/m/mbun.{zlib,zstd,brotli}.lua`), the same
download-source-and-compile pattern as `mbun.sqlite3`:

- **zlib 1.3.1** — raw DEFLATE (`windowBits -15`), zlib (`15`), gzip (`31`).
  The `gz*.c` file-I/O units are omitted; only the in-memory codec is used.
- **zstd 1.5.6** — `ZSTD_compress` + streaming `ZSTD_decompressStream`
  (decodes concatenated multi-frame input).
- **brotli 1.1.0** — `BrotliEncoderCompress` + streaming decode.

`modules/core`'s hand-written `mbun.core.compress` (gzip inflate used by
`install`) is a separate compatibility implementation and is intentionally not
touched by this module.
