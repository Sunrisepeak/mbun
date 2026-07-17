-- mbun.brotli — Brotli 压缩库（enc + dec），官方 mcpp-index 未收录，收入 mbun
-- 本地索引。
--
-- 选型：与 bun 对齐，从上游源码 tarball 编译（bun 内置 brotli 源码），而非链接
-- 系统 libbrotli*。范式同 mbun.zlib/zstd/sqlite3。
--
-- 版本 1.1.0（2023-08）。编译 c/common(6)+c/dec(4)+c/enc(21)=31 个 .c，公共头
-- 在 c/include/brotli/{encode,decode,types,...}.h。node:zlib 的 brotli* 系列与
-- Bun 的 brotli 编解码依赖之。
package = {
    spec        = "1",
    namespace   = "mbun",
    name        = "mbun.brotli",
    description = "Brotli: general-purpose lossless compression (encoder + decoder), compiled from upstream source as bun bundles it",
    licenses    = { "MIT" },
    repo        = "https://github.com/google/brotli",
    type        = "package",

    xpm = {
        linux = {
            ["1.1.0+mbun.1"] = {
                url    = "https://github.com/google/brotli/archive/refs/tags/v1.1.0.tar.gz",
                sha256 = "e720a6ca29428b803f4ad165371771f5398faba397edf6778837a18599ea13ff",
            },
        },
        macosx = {
            ["1.1.0+mbun.1"] = {
                url    = "https://github.com/google/brotli/archive/refs/tags/v1.1.0.tar.gz",
                sha256 = "e720a6ca29428b803f4ad165371771f5398faba397edf6778837a18599ea13ff",
            },
        },
        windows = {
            ["1.1.0+mbun.1"] = {
                url    = "https://github.com/google/brotli/archive/refs/tags/v1.1.0.tar.gz",
                sha256 = "e720a6ca29428b803f4ad165371771f5398faba397edf6778837a18599ea13ff",
            },
        },
    },

    mcpp = {
        schema       = "0.1",
        language     = "c++23",
        import_std   = false,
        c_standard   = "c11",
        include_dirs = { "brotli-1.1.0/c/include" },
        sources = {
            "brotli-1.1.0/c/common/constants.c",
            "brotli-1.1.0/c/common/context.c",
            "brotli-1.1.0/c/common/dictionary.c",
            "brotli-1.1.0/c/common/platform.c",
            "brotli-1.1.0/c/common/shared_dictionary.c",
            "brotli-1.1.0/c/common/transform.c",
            "brotli-1.1.0/c/dec/bit_reader.c",
            "brotli-1.1.0/c/dec/decode.c",
            "brotli-1.1.0/c/dec/huffman.c",
            "brotli-1.1.0/c/dec/state.c",
            "brotli-1.1.0/c/enc/backward_references.c",
            "brotli-1.1.0/c/enc/backward_references_hq.c",
            "brotli-1.1.0/c/enc/bit_cost.c",
            "brotli-1.1.0/c/enc/block_splitter.c",
            "brotli-1.1.0/c/enc/brotli_bit_stream.c",
            "brotli-1.1.0/c/enc/cluster.c",
            "brotli-1.1.0/c/enc/command.c",
            "brotli-1.1.0/c/enc/compound_dictionary.c",
            "brotli-1.1.0/c/enc/compress_fragment.c",
            "brotli-1.1.0/c/enc/compress_fragment_two_pass.c",
            "brotli-1.1.0/c/enc/dictionary_hash.c",
            "brotli-1.1.0/c/enc/encode.c",
            "brotli-1.1.0/c/enc/encoder_dict.c",
            "brotli-1.1.0/c/enc/entropy_encode.c",
            "brotli-1.1.0/c/enc/fast_log.c",
            "brotli-1.1.0/c/enc/histogram.c",
            "brotli-1.1.0/c/enc/literal_cost.c",
            "brotli-1.1.0/c/enc/memory.c",
            "brotli-1.1.0/c/enc/metablock.c",
            "brotli-1.1.0/c/enc/static_dict.c",
            "brotli-1.1.0/c/enc/utf8_util.c",
        },
        targets      = { ["brotli"] = { kind = "lib" } },
        deps         = { },
    },
}
