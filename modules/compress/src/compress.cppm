// compress.cppm — compression subsystem aggregator.
// Native backends are wired in: zlib (deflate/inflate/gzip), zstd, and brotli
// are compiled from upstream source via the mbun local index.
export module mbun.compress;

export import mbun.compress.types;
export import mbun.compress.zlib_native;
export import mbun.compress.deflate;
export import mbun.compress.zlib;
export import mbun.compress.gzip;
export import mbun.compress.brotli;
export import mbun.compress.zstd;
export import mbun.compress.stream;
export import mbun.compress.libdeflate_sys;
