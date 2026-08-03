-- mbun.openssl — OpenSSL 3.1.5 静态后端（TLS/crypto），官方 mcpp-index 未收录
-- 独立 lib 包，收入 mbun 本地索引。
--
-- 选型：modules/tls 的 TlsChannel（src/openssl.cpp）直接调 OpenSSL 3.x C API
-- （OPENSSL_init_ssl / TLS_client_method / EVP_RSA_gen / SSL_set1_host / memory-BIO
-- 握手环回…），行为对齐 bun 的 uSockets TLS 路径。铁律：构建不得依赖任何 host
-- 库（不能从 /usr 或工具链 sysroot 隐式解析 -l:libssl.a），只能用 mcpp 生态库 /
-- 自建本地 index / 自己实现，才能跨平台 & CI 可复现。
--
-- 路线（对照任务优先级）：
--   ① 注册表 compat 包：mcpp 注册表有 compat.mbedtls，但无 compat.openssl /
--      compat.boringssl；mbedtls 的 API 与 OpenSSL 完全不同（非 drop-in，换它要
--      重写整个后端），排除。
--   ② 预编译静态库：mcpp 生态的 xim 索引自带 `xim:openssl@3.1.5` —— 一个
--      预编译 OpenSSL（libssl.a/libcrypto.a + 生成好的头），版本正好等于本仓
--      openssl.cpp 编写针对的 3.1.5，非 host /usr。**采用此路**：本包把该预编译
--      产物 vendored 进本地 index 的 lib 目标（范式同 mbun.jsc-prebuilt 之
--      「xim:gcc build dep → install() 拷 libstdc++.a」，只是这里 vendored 的是
--      OpenSSL 静态库+头）。
--   ③ 源码编译：OpenSSL 需 Perl `Configure` 生成 opensslconf.h/buildinf.h/汇编，
--      而 xim 索引无 perl（亦无 nasm），mbedtls 式「编全部 *.c」不适用 OpenSSL；
--      BoringSSL 需 cmake+go 且缺 EVP_RSA_gen 等 OpenSSL-3 API（要改后端）。较重，
--      不取。
--
-- 纯 vendored-prebuilt 包：linux 由 install() 从 build dep `xim:openssl` 拷入
-- libssl.a/libcrypto.a 与 include/openssl（并写出 anchor TU —— anchor 的缺失正是
-- 触发 mcpp 运行 install() 的机制，同 mbun.jsc-prebuilt）。xpm 的源码 tarball 仅
-- 提供版本溯源/许可（不参与编译；真实链接全部经 ldflags 指向 vendored 静态库）。
-- macosx/windows：xim:openssl 仅 linux，故非 linux 平台的 vendored-prebuilt 后端
-- 标 DEFERRED，anchor 走 generated_files 让包可解析（不提供 ssl ldflags）。
package = {
    spec        = "1",
    namespace   = "mbun",
    name        = "mbun.openssl",
    description = "OpenSSL 3.1.5 static TLS/crypto backend, vendored from the mcpp xim:openssl prebuilt (no host libssl)",
    licenses    = { "Apache-2.0" },
    repo        = "https://github.com/openssl/openssl",
    type        = "package",

    xpm = {
        linux = {
            -- libssl.a/libcrypto.a + 头来自 build dep xim:openssl@3.1.5（预编译）。
            deps = { "xim:openssl@3.1.5" },
            ["3.1.5+mbun.3"] = {
                url    = "https://github.com/openssl/openssl/releases/download/openssl-3.1.5/openssl-3.1.5.tar.gz",
                sha256 = "6ae015467dabf0469b139ada93319327be24b98251ffaeceda0221848dc09262",
            },
        },
        macosx = {
            ["3.1.5+mbun.1"] = {
                url    = "https://github.com/openssl/openssl/releases/download/openssl-3.1.5/openssl-3.1.5.tar.gz",
                sha256 = "6ae015467dabf0469b139ada93319327be24b98251ffaeceda0221848dc09262",
            },
        },
        windows = {
            ["3.1.5+mbun.1"] = {
                url    = "https://github.com/openssl/openssl/releases/download/openssl-3.1.5/openssl-3.1.5.tar.gz",
                sha256 = "6ae015467dabf0469b139ada93319327be24b98251ffaeceda0221848dc09262",
            },
        },
    },

    mcpp = {
        schema       = "0.1",
        language     = "c++23",
        import_std   = false,  -- 仅一个 C anchor TU，无 C++ 源
        c_standard   = "c17",
        -- install() 落地的头（include/openssl/*.h），供 <openssl/ssl.h> 解析。
        include_dirs = { "include" },
        -- anchor TU：满足「sources 非空」并给 lib 目标一个可编译单元。
        -- linux 由 install() 写出（缺失触发 install()）；macosx/windows 走
        -- generated_files。
        sources      = { "mcpp_openssl_anchor.c" },
        targets      = { ["openssl"] = { kind = "lib" } },
        deps         = { },

        -- 链接参数按平台隔离（`-L` 相对路径由 mcpp 重写为包安装目录下的绝对路径）。
        linux = {
            ldflags = {
                "-Llib",
                -- 顺序：libssl 依赖 libcrypto，ssl 在前。-l:<档案名> 强制静态、
                -- 直达 ld（不经驱动的 -lssl→共享库解析），无 libssl.so.3 rpath 依赖。
                "-l:libssl.a",
                "-l:libcrypto.a",
                -- OpenSSL 静态库对系统运行时库的依赖（dlopen 引擎/线程）。
                "-ldl",
                "-lpthread",
            },
        },
        -- DEFERRED(non-linux): xim:openssl 仅 linux 有预编译产物，macOS/Windows 的
        -- vendored-prebuilt 后端待排期（届时换各平台预编译 tarball 或 compat 包）。
        -- 此处仅提供 anchor 让包可解析，不给 ssl ldflags。
        macosx = {
            generated_files = {
                ["mcpp_openssl_anchor.c"] = "int mcpp_mbun_openssl_anchor(void) { return 0; }\n",
            },
        },
        windows = {
            generated_files = {
                ["mcpp_openssl_anchor.c"] = "int mcpp_mbun_openssl_anchor(void) { return 0; }\n",
            },
        },
    },
}

import("xim.libxpkg.pkginfo")
import("xim.libxpkg.log")

-- linux：把 build dep xim:openssl 的预编译 libssl.a/libcrypto.a 与 include/openssl
-- 拷入本包安装目录（lib/、include/openssl/），并写出 anchor TU —— anchor 缺失正是
-- 让 mcpp 在构建前运行本 install() 的触发器（mbun.jsc-prebuilt 同款机制）。
-- macosx/windows：anchor 来自 generated_files，无需本钩子（见 DEFERRED 注）。
function install()
    if os.host() ~= "linux" then
        return true
    end

    local prefix = pkginfo.install_dir()

    local ssl = pkginfo.build_dep("xim:openssl") or pkginfo.build_dep("openssl")
    if not ssl then
        log.error("mbun.openssl: build dep xim:openssl unavailable (prebuilt libssl.a/libcrypto.a)")
        return false
    end
    -- xim:openssl 的静态库在 lib64/，头在 include/openssl/。
    local srclib = path.join(ssl.path, "lib64")
    local libssl = path.join(srclib, "libssl.a")
    local libcrypto = path.join(srclib, "libcrypto.a")
    if not os.isfile(libssl) or not os.isfile(libcrypto) then
        log.error("mbun.openssl: prebuilt static archives not found under %s", srclib)
        return false
    end

    local libdir = path.join(prefix, "lib")
    os.mkdir(libdir)
    os.cp(libssl, path.join(libdir, "libssl.a"))
    os.cp(libcrypto, path.join(libdir, "libcrypto.a"))

    local srcinc = path.join(ssl.path, "include", "openssl")
    if not os.isdir(srcinc) then
        log.error("mbun.openssl: prebuilt headers not found under %s", srcinc)
        return false
    end
    -- Copy the complete generated header tree. Copying the `openssl` directory
    -- into an already-created `include/openssl` directory would create the
    -- wrong include/openssl/openssl/*.h layout.
    local incroot = path.join(ssl.path, "include")
    os.cp(incroot, path.join(prefix, "include"), { force = true })

    io.writefile(path.join(prefix, "mcpp_openssl_anchor.c"),
                 "int mcpp_mbun_openssl_anchor(void) { return 0; }\n")
    return true
end
