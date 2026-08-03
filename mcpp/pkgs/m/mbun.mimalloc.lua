-- compat.mimalloc — bun 使用的通用内存分配器（官方 mcpp-index 未收录，收入 mbun 本地索引）
package = {
    spec        = "1",
    namespace   = "mbun",
    name        = "mbun.mimalloc",
    description = "mimalloc: a compact general purpose allocator with excellent performance (used by bun)",
    licenses    = { "MIT" },
    repo        = "https://github.com/microsoft/mimalloc",
    type        = "package",

    xpm = {
        linux = {
            ["2.1.7+mbun.1"] = {
                url    = "https://github.com/microsoft/mimalloc/archive/refs/tags/v2.1.7.tar.gz",
                sha256 = "0eed39319f139afde8515010ff59baf24de9e47ea316a315398e8027d198202d",
            },
        },
        macosx = {
            ["2.1.7+mbun.1"] = {
                url    = "https://github.com/microsoft/mimalloc/archive/refs/tags/v2.1.7.tar.gz",
                sha256 = "0eed39319f139afde8515010ff59baf24de9e47ea316a315398e8027d198202d",
            },
        },
        windows = {
            ["2.1.7+mbun.1"] = {
                url    = "https://github.com/microsoft/mimalloc/archive/refs/tags/v2.1.7.tar.gz",
                sha256 = "0eed39319f139afde8515010ff59baf24de9e47ea316a315398e8027d198202d",
            },
        },
    },

    mcpp = {
        schema       = "0.1",
        language     = "c++23",
        c_standard   = "c17",
        include_dirs = { "mimalloc-2.1.7/include" },
        sources      = { "mimalloc-2.1.7/src/static.c" },
        targets      = { ["mimalloc"] = { kind = "lib" } },
        deps         = { },
    },
}
