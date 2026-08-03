-- mbun.zstd — Zstandard 压缩库，官方 mcpp-index 未收录，收入 mbun 本地索引。
--
-- 选型：与 bun 对齐，从上游源码 tarball 编译（bun 内置 zstd 源码），而非链接
-- 系统 libzstd。范式同 mbun.zlib/sqlite3。
--
-- 版本 1.5.6（2024-03）。编译核心三组：common(8)+compress(13)+decompress(4)。
-- 关闭历史遗留：-DZSTD_LEGACY_SUPPORT=0 免掉 lib/legacy 的 7 个 v01..v07 文件；
-- -DZSTD_DISABLE_ASM=1 免掉 huf_decompress_amd64.S（走 C 回退，跨平台一致）；
-- 不含 dictBuilder（ZDICT 未用）。单线程构建（不定义 ZSTD_MULTITHREAD）。
package = {
    spec        = "1",
    namespace   = "mbun",
    name        = "mbun.zstd",
    description = "Zstandard: fast real-time compression, compiled from upstream source as bun bundles it",
    licenses    = { "BSD-3-Clause" },  -- zstd is BSD-3-Clause (dual GPL-2.0)
    repo        = "https://facebook.github.io/zstd",
    type        = "package",

    xpm = {
        linux = {
            ["1.5.6+mbun.1"] = {
                url    = "https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz",
                sha256 = "8c29e06cf42aacc1eafc4077ae2ec6c6fcb96a626157e0593d5e82a34fd403c1",
            },
        },
        macosx = {
            ["1.5.6+mbun.1"] = {
                url    = "https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz",
                sha256 = "8c29e06cf42aacc1eafc4077ae2ec6c6fcb96a626157e0593d5e82a34fd403c1",
            },
        },
        windows = {
            ["1.5.6+mbun.1"] = {
                url    = "https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz",
                sha256 = "8c29e06cf42aacc1eafc4077ae2ec6c6fcb96a626157e0593d5e82a34fd403c1",
            },
        },
    },

    mcpp = {
        schema       = "0.1",
        language     = "c++23",
        import_std   = false,
        c_standard   = "c11",
        include_dirs = {
            "zstd-1.5.6/lib",
            "zstd-1.5.6/lib/common",
            "zstd-1.5.6/lib/compress",
            "zstd-1.5.6/lib/decompress",
        },
        cflags = {
            "-DZSTD_LEGACY_SUPPORT=0",
            "-DZSTD_DISABLE_ASM=1",
        },
        sources = {
            "zstd-1.5.6/lib/common/debug.c",
            "zstd-1.5.6/lib/common/entropy_common.c",
            "zstd-1.5.6/lib/common/error_private.c",
            "zstd-1.5.6/lib/common/fse_decompress.c",
            "zstd-1.5.6/lib/common/pool.c",
            "zstd-1.5.6/lib/common/threading.c",
            "zstd-1.5.6/lib/common/xxhash.c",
            "zstd-1.5.6/lib/common/zstd_common.c",
            "zstd-1.5.6/lib/compress/fse_compress.c",
            "zstd-1.5.6/lib/compress/hist.c",
            "zstd-1.5.6/lib/compress/huf_compress.c",
            "zstd-1.5.6/lib/compress/zstd_compress.c",
            "zstd-1.5.6/lib/compress/zstd_compress_literals.c",
            "zstd-1.5.6/lib/compress/zstd_compress_sequences.c",
            "zstd-1.5.6/lib/compress/zstd_compress_superblock.c",
            "zstd-1.5.6/lib/compress/zstd_double_fast.c",
            "zstd-1.5.6/lib/compress/zstd_fast.c",
            "zstd-1.5.6/lib/compress/zstd_lazy.c",
            "zstd-1.5.6/lib/compress/zstd_ldm.c",
            "zstd-1.5.6/lib/compress/zstdmt_compress.c",
            "zstd-1.5.6/lib/compress/zstd_opt.c",
            "zstd-1.5.6/lib/decompress/huf_decompress.c",
            "zstd-1.5.6/lib/decompress/zstd_ddict.c",
            "zstd-1.5.6/lib/decompress/zstd_decompress_block.c",
            "zstd-1.5.6/lib/decompress/zstd_decompress.c",
        },
        targets      = { ["zstd"] = { kind = "lib" } },
        deps         = { },
    },
}
