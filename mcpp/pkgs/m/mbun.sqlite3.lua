-- mbun.sqlite3 — SQLite3 amalgamation（bun 内置的 SQLite，官方 mcpp-index 未
-- 收录，收入 mbun 本地索引）。
--
-- 选型：用官方 amalgamation 单文件 sqlite3.c 从源码编译，而非链接系统
-- libsqlite3。理由——① 与 bun 对齐：bun 同样内置编译 sqlite3 amalgamation，
-- 版本/编译开关自控；② 跨平台可复现：不依赖各平台是否装了 libsqlite3-dev；
-- ③ 与 mimalloc 同款「下载源码 tarball + 编译 lib」范式，mcpp 一致处理。
--
-- 版本 3.45.1 = amalgamation 3450100（2024-01-31）。三平台同一份 amalgamation
-- zip（纯 C，平台无关），链接所需系统库按 OS 隔离到 ldflags。
package = {
    spec        = "1",
    namespace   = "mbun",
    name        = "mbun.sqlite3",
    description = "SQLite3: self-contained SQL database engine (amalgamation, as bundled by bun)",
    licenses    = { "blessing" },  -- SQLite is public domain (SPDX: blessing)
    repo        = "https://www.sqlite.org",
    type        = "package",

    xpm = {
        linux = {
            ["3.45.1+mbun.1"] = {
                url    = "https://www.sqlite.org/2024/sqlite-amalgamation-3450100.zip",
                sha256 = "5592243caf28b2cdef41e6ab58d25d653dfc53deded8450eb66072c929f030c4",
            },
        },
        macosx = {
            ["3.45.1+mbun.1"] = {
                url    = "https://www.sqlite.org/2024/sqlite-amalgamation-3450100.zip",
                sha256 = "5592243caf28b2cdef41e6ab58d25d653dfc53deded8450eb66072c929f030c4",
            },
        },
        windows = {
            ["3.45.1+mbun.1"] = {
                url    = "https://www.sqlite.org/2024/sqlite-amalgamation-3450100.zip",
                sha256 = "5592243caf28b2cdef41e6ab58d25d653dfc53deded8450eb66072c929f030c4",
            },
        },
    },

    mcpp = {
        schema       = "0.1",
        language     = "c++23",
        import_std   = false,  -- 纯 C 单编译单元，无 C++/import std
        c_standard   = "c11",
        include_dirs = { "sqlite-amalgamation-3450100" },
        sources      = { "sqlite-amalgamation-3450100/sqlite3.c" },
        -- 编译开关对齐 bun 的常用能力集，避免运行期缺特性（FTS5/RTREE/JSON/
        -- 列元数据）。线程安全串行模式（THREADSAFE=1）与 bun 一致。
        cflags = {
            "-DSQLITE_THREADSAFE=1",
            "-DSQLITE_ENABLE_COLUMN_METADATA=1",
            "-DSQLITE_ENABLE_FTS5=1",
            "-DSQLITE_ENABLE_RTREE=1",
            "-DSQLITE_ENABLE_JSON1=1",
            "-DSQLITE_ENABLE_DBSTAT_VTAB=1",
            "-DSQLITE_ENABLE_MATH_FUNCTIONS=1",
        },
        targets      = { ["sqlite3"] = { kind = "lib" } },
        deps         = { },

        -- amalgamation 静态库对系统库的依赖，交由最终链接补齐。
        linux = {
            ldflags = { "-lpthread", "-ldl", "-lm" },
        },
        macosx = {
            ldflags = { "-lpthread", "-lm" },
        },
        windows = {
            -- MSVC/clang-cl：sqlite3 amalgamation 无额外系统库依赖（winsqlite 由
            -- 内置实现），无需追加。
            ldflags = { },
        },
    },
}
