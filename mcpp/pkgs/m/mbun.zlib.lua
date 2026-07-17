-- mbun.zlib — zlib 压缩库（RFC 1950/1951/1952：zlib/deflate/gzip），官方
-- mcpp-index 未收录，收入 mbun 本地索引。
--
-- 选型：与 bun 对齐，从上游源码 tarball 编译（bun 同样内置编译 zlib，版本/
-- 编译开关自控），而非链接系统 libz。理由同 mbun.sqlite3——① 跨平台可复现，
-- 不依赖各平台是否装了 zlib1g-dev；② 与 mimalloc/sqlite3 同款「下载源码
-- tarball + 编译 lib」范式。
--
-- 版本 1.3.1（2024-01-22）。发布 tarball 自带预生成的 zconf.h/zlib.h，无需
-- ./configure。15 个 .c 是 zlib 的完整核心（deflate/inflate/gzip/adler32/
-- crc32/trees…），node:zlib 与 Bun.gzip*/Bun.deflate* 全部依赖之。
package = {
    spec        = "1",
    namespace   = "mbun",
    name        = "mbun.zlib",
    description = "zlib: lossless data compression (DEFLATE/zlib/gzip), compiled from upstream source as bun bundles it",
    licenses    = { "Zlib" },
    repo        = "https://zlib.net",
    type        = "package",

    xpm = {
        linux = {
            ["1.3.1+mbun.1"] = {
                url    = "https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz",
                sha256 = "9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23",
            },
        },
        macosx = {
            ["1.3.1+mbun.1"] = {
                url    = "https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz",
                sha256 = "9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23",
            },
        },
        windows = {
            ["1.3.1+mbun.1"] = {
                url    = "https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz",
                sha256 = "9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23",
            },
        },
    },

    mcpp = {
        schema       = "0.1",
        language     = "c++23",
        import_std   = false,  -- 纯 C，无 C++/import std
        c_standard   = "c11",
        include_dirs = { "zlib-1.3.1" },
        -- gz*.c (gzFILE file I/O) omitted: they need POSIX unistd.h and mbun
        -- only uses the in-memory deflate/inflate codec (gzip framing is done
        -- via inflate/deflate windowBits, not gzopen/gzread).
        sources      = {
            "zlib-1.3.1/adler32.c",
            "zlib-1.3.1/compress.c",
            "zlib-1.3.1/crc32.c",
            "zlib-1.3.1/deflate.c",
            "zlib-1.3.1/infback.c",
            "zlib-1.3.1/inffast.c",
            "zlib-1.3.1/inflate.c",
            "zlib-1.3.1/inftrees.c",
            "zlib-1.3.1/trees.c",
            "zlib-1.3.1/uncompr.c",
            "zlib-1.3.1/zutil.c",
        },
        targets      = { ["z"] = { kind = "lib" } },
        deps         = { },
    },
}
