-- mbun.libwebp — WebP 编解码库（libwebp + 内置 sharpyuv），官方 mcpp-index 未
-- 收录，收入 mbun 本地索引。
--
-- 选型：与 bun 对齐，从上游 release 源码 tarball 编译，而非链接系统
-- libwebp-dev（后者会把 /usr/lib 引入链接搜索路径，遮蔽 bun-webkit 自带的
-- ICU 75，导致 app/cli 链接期 utext_setup_75 等符号未定义）。范式同
-- mbun.zlib/zstd/brotli/sqlite3：下载源码 tarball + 编译静态 lib，跨平台
-- 可复现、不依赖 host 第三方库。
--
-- 版本 1.5.0（2024-12，webmproject/libwebp v1.5.0）。编译 dec+enc+dsp+utils
-- + sharpyuv 全量 .c 打成单一静态库 `webp`（sharpyuv 归并进同一 archive，
-- 编码器 picture_csp_enc 依赖之）。dsp 的 SSE2/SSE41/NEON/MIPS/MSA 变体各文件
-- 由 src/dsp/cpu.h 依 __SSE2__/__SSE4_1__/__aarch64__ 等编译器内建宏自门控：
-- 不匹配当前 target 的变体编译为空 TU，运行期 CPU 检测走标量回退，功能不减。
-- 未定义 HAVE_CONFIG_H，故 SSE2（x86-64 基线）/NEON（aarch64 基线）自动启用；
-- SSE4.1 需 per-file -msse4.1，本包不加（保持产物不强制 SSE4.1 CPU），对应
-- dsp 走 SSE2/标量路径 —— DEFERRED：SSE4.1 dsp 优化路径。
--
-- demux/mux（WebPDemux*/WebPMux* 动图/元数据）未纳入，与 webp_native.cppm 的
-- DEFERRED 一致（Image 管线只跑 WebPDecodeRGBA / WebPEncode(Lossless)RGBA 的
-- 扁平 RGBA 往返）。
--
-- 内部源码用 root 相对包含（#include "src/...", "sharpyuv/..."），故 include
-- root 目录；消费者 include <webp/decode.h> 需 src 目录 —— 两个 include_dirs
-- 都随包传播给依赖方。
package = {
    spec        = "1",
    namespace   = "mbun",
    name        = "mbun.libwebp",
    description = "libWebP: WebP image codec (decode + encode + sharpyuv), compiled from upstream source as bun bundles it",
    licenses    = { "BSD-3-Clause" },
    repo        = "https://github.com/webmproject/libwebp",
    type        = "package",

    xpm = {
        linux = {
            ["1.5.0+mbun.1"] = {
                url    = "https://github.com/webmproject/libwebp/archive/refs/tags/v1.5.0.tar.gz",
                sha256 = "668c9aba45565e24c27e17f7aaf7060a399f7f31dba6c97a044e1feacb930f37",
            },
        },
        macosx = {
            ["1.5.0+mbun.1"] = {
                url    = "https://github.com/webmproject/libwebp/archive/refs/tags/v1.5.0.tar.gz",
                sha256 = "668c9aba45565e24c27e17f7aaf7060a399f7f31dba6c97a044e1feacb930f37",
            },
        },
        windows = {
            ["1.5.0+mbun.1"] = {
                url    = "https://github.com/webmproject/libwebp/archive/refs/tags/v1.5.0.tar.gz",
                sha256 = "668c9aba45565e24c27e17f7aaf7060a399f7f31dba6c97a044e1feacb930f37",
            },
        },
    },

    mcpp = {
        schema       = "0.1",
        language     = "c++23",
        import_std   = false,  -- 纯 C，无 C++/import std
        c_standard   = "c11",
        -- root：内部 src/... 与 sharpyuv/... 包含；src：消费者 <webp/*.h>。
        include_dirs = { "libwebp-1.5.0", "libwebp-1.5.0/src" },
        sources      = {
            "libwebp-1.5.0/src/dec/alpha_dec.c",
            "libwebp-1.5.0/src/dec/buffer_dec.c",
            "libwebp-1.5.0/src/dec/frame_dec.c",
            "libwebp-1.5.0/src/dec/idec_dec.c",
            "libwebp-1.5.0/src/dec/io_dec.c",
            "libwebp-1.5.0/src/dec/quant_dec.c",
            "libwebp-1.5.0/src/dec/tree_dec.c",
            "libwebp-1.5.0/src/dec/vp8_dec.c",
            "libwebp-1.5.0/src/dec/vp8l_dec.c",
            "libwebp-1.5.0/src/dec/webp_dec.c",
            "libwebp-1.5.0/src/enc/alpha_enc.c",
            "libwebp-1.5.0/src/enc/analysis_enc.c",
            "libwebp-1.5.0/src/enc/backward_references_cost_enc.c",
            "libwebp-1.5.0/src/enc/backward_references_enc.c",
            "libwebp-1.5.0/src/enc/config_enc.c",
            "libwebp-1.5.0/src/enc/cost_enc.c",
            "libwebp-1.5.0/src/enc/filter_enc.c",
            "libwebp-1.5.0/src/enc/frame_enc.c",
            "libwebp-1.5.0/src/enc/histogram_enc.c",
            "libwebp-1.5.0/src/enc/iterator_enc.c",
            "libwebp-1.5.0/src/enc/near_lossless_enc.c",
            "libwebp-1.5.0/src/enc/picture_csp_enc.c",
            "libwebp-1.5.0/src/enc/picture_enc.c",
            "libwebp-1.5.0/src/enc/picture_psnr_enc.c",
            "libwebp-1.5.0/src/enc/picture_rescale_enc.c",
            "libwebp-1.5.0/src/enc/picture_tools_enc.c",
            "libwebp-1.5.0/src/enc/predictor_enc.c",
            "libwebp-1.5.0/src/enc/quant_enc.c",
            "libwebp-1.5.0/src/enc/syntax_enc.c",
            "libwebp-1.5.0/src/enc/token_enc.c",
            "libwebp-1.5.0/src/enc/tree_enc.c",
            "libwebp-1.5.0/src/enc/vp8l_enc.c",
            "libwebp-1.5.0/src/enc/webp_enc.c",
            "libwebp-1.5.0/src/dsp/alpha_processing.c",
            "libwebp-1.5.0/src/dsp/alpha_processing_mips_dsp_r2.c",
            "libwebp-1.5.0/src/dsp/alpha_processing_neon.c",
            "libwebp-1.5.0/src/dsp/alpha_processing_sse2.c",
            "libwebp-1.5.0/src/dsp/alpha_processing_sse41.c",
            "libwebp-1.5.0/src/dsp/cost.c",
            "libwebp-1.5.0/src/dsp/cost_mips32.c",
            "libwebp-1.5.0/src/dsp/cost_mips_dsp_r2.c",
            "libwebp-1.5.0/src/dsp/cost_neon.c",
            "libwebp-1.5.0/src/dsp/cost_sse2.c",
            "libwebp-1.5.0/src/dsp/cpu.c",
            "libwebp-1.5.0/src/dsp/dec.c",
            "libwebp-1.5.0/src/dsp/dec_clip_tables.c",
            "libwebp-1.5.0/src/dsp/dec_mips32.c",
            "libwebp-1.5.0/src/dsp/dec_mips_dsp_r2.c",
            "libwebp-1.5.0/src/dsp/dec_msa.c",
            "libwebp-1.5.0/src/dsp/dec_neon.c",
            "libwebp-1.5.0/src/dsp/dec_sse2.c",
            "libwebp-1.5.0/src/dsp/dec_sse41.c",
            "libwebp-1.5.0/src/dsp/enc.c",
            "libwebp-1.5.0/src/dsp/enc_mips32.c",
            "libwebp-1.5.0/src/dsp/enc_mips_dsp_r2.c",
            "libwebp-1.5.0/src/dsp/enc_msa.c",
            "libwebp-1.5.0/src/dsp/enc_neon.c",
            "libwebp-1.5.0/src/dsp/enc_sse2.c",
            "libwebp-1.5.0/src/dsp/enc_sse41.c",
            "libwebp-1.5.0/src/dsp/filters.c",
            "libwebp-1.5.0/src/dsp/filters_mips_dsp_r2.c",
            "libwebp-1.5.0/src/dsp/filters_msa.c",
            "libwebp-1.5.0/src/dsp/filters_neon.c",
            "libwebp-1.5.0/src/dsp/filters_sse2.c",
            "libwebp-1.5.0/src/dsp/lossless.c",
            "libwebp-1.5.0/src/dsp/lossless_enc.c",
            "libwebp-1.5.0/src/dsp/lossless_enc_mips32.c",
            "libwebp-1.5.0/src/dsp/lossless_enc_mips_dsp_r2.c",
            "libwebp-1.5.0/src/dsp/lossless_enc_msa.c",
            "libwebp-1.5.0/src/dsp/lossless_enc_neon.c",
            "libwebp-1.5.0/src/dsp/lossless_enc_sse2.c",
            "libwebp-1.5.0/src/dsp/lossless_enc_sse41.c",
            "libwebp-1.5.0/src/dsp/lossless_mips_dsp_r2.c",
            "libwebp-1.5.0/src/dsp/lossless_msa.c",
            "libwebp-1.5.0/src/dsp/lossless_neon.c",
            "libwebp-1.5.0/src/dsp/lossless_sse2.c",
            "libwebp-1.5.0/src/dsp/lossless_sse41.c",
            "libwebp-1.5.0/src/dsp/rescaler.c",
            "libwebp-1.5.0/src/dsp/rescaler_mips32.c",
            "libwebp-1.5.0/src/dsp/rescaler_mips_dsp_r2.c",
            "libwebp-1.5.0/src/dsp/rescaler_msa.c",
            "libwebp-1.5.0/src/dsp/rescaler_neon.c",
            "libwebp-1.5.0/src/dsp/rescaler_sse2.c",
            "libwebp-1.5.0/src/dsp/ssim.c",
            "libwebp-1.5.0/src/dsp/ssim_sse2.c",
            "libwebp-1.5.0/src/dsp/upsampling.c",
            "libwebp-1.5.0/src/dsp/upsampling_mips_dsp_r2.c",
            "libwebp-1.5.0/src/dsp/upsampling_msa.c",
            "libwebp-1.5.0/src/dsp/upsampling_neon.c",
            "libwebp-1.5.0/src/dsp/upsampling_sse2.c",
            "libwebp-1.5.0/src/dsp/upsampling_sse41.c",
            "libwebp-1.5.0/src/dsp/yuv.c",
            "libwebp-1.5.0/src/dsp/yuv_mips32.c",
            "libwebp-1.5.0/src/dsp/yuv_mips_dsp_r2.c",
            "libwebp-1.5.0/src/dsp/yuv_neon.c",
            "libwebp-1.5.0/src/dsp/yuv_sse2.c",
            "libwebp-1.5.0/src/dsp/yuv_sse41.c",
            "libwebp-1.5.0/src/utils/bit_reader_utils.c",
            "libwebp-1.5.0/src/utils/bit_writer_utils.c",
            "libwebp-1.5.0/src/utils/color_cache_utils.c",
            "libwebp-1.5.0/src/utils/filters_utils.c",
            "libwebp-1.5.0/src/utils/huffman_encode_utils.c",
            "libwebp-1.5.0/src/utils/huffman_utils.c",
            "libwebp-1.5.0/src/utils/palette.c",
            "libwebp-1.5.0/src/utils/quant_levels_dec_utils.c",
            "libwebp-1.5.0/src/utils/quant_levels_utils.c",
            "libwebp-1.5.0/src/utils/random_utils.c",
            "libwebp-1.5.0/src/utils/rescaler_utils.c",
            "libwebp-1.5.0/src/utils/thread_utils.c",
            "libwebp-1.5.0/src/utils/utils.c",
            "libwebp-1.5.0/sharpyuv/sharpyuv.c",
            "libwebp-1.5.0/sharpyuv/sharpyuv_cpu.c",
            "libwebp-1.5.0/sharpyuv/sharpyuv_csp.c",
            "libwebp-1.5.0/sharpyuv/sharpyuv_dsp.c",
            "libwebp-1.5.0/sharpyuv/sharpyuv_gamma.c",
            "libwebp-1.5.0/sharpyuv/sharpyuv_neon.c",
            "libwebp-1.5.0/sharpyuv/sharpyuv_sse2.c",
        },
        targets      = { ["webp"] = { kind = "lib" } },
        deps         = { },

        -- 静态库对系统 C 运行时的依赖，交由最终链接补齐（dsp 用 libm；
        -- 线程池路径用 pthread）。均属工具链 sysroot 组件，非 host 第三方库。
        linux = {
            ldflags = { "-lpthread", "-lm" },
        },
        macosx = {
            ldflags = { "-lpthread", "-lm" },
        },
        windows = {
            ldflags = { },
        },
    },
}
