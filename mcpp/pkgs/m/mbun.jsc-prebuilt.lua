-- mbun.jsc-prebuilt — bun fork 的 JavaScriptCore 预编译静态库（oven-sh/WebKit
-- autobuild 产物，官方 mcpp-index 未收录，收入 mbun 本地索引）。
--
-- 版本号 = autobuild 日期（bun pin 的 WEBKIT_VERSION 对应 release：
-- autobuild-c9ad5813fd23bd8b98b0738abc3d037ec716aa92，2026-07-06）。
-- 设计与实测结论见 mbun docs/design/20260710-jsc-integration.md：
--   * Linux 产物 clang 构建但用 libstdc++（Itanium ABI），gcc16 直链无障碍；
--   * mcpp 的 llvm 工具链为封闭 libc++（-stdlib=libc++ / compiler-rt），不含
--     libstdc++ 运行时 → 产物引用的 libstdc++ 外联符号（std::filesystem、
--     condition_variable、_Rb_tree、__throw_* 等）无处解析。解法：声明
--     xim:gcc 构建依赖，install() 把 libstdc++.a 拷入 bun-webkit/lib，
--     ldflags 追加 -lstdc++ —— llvm 下静态解析缺失符号（libc++/libstdc++
--     符号命名空间不同不冲突，跨界只经 JSC C API）；gcc16 下命中同版本
--     静态库，与工具链隐式 -static-libstdc++ 一致，无重复运行时；
--   * 禁用 -lto 变体（clang bitcode，gcc 不可链）；开 sanitizer 需换 -asan 变体；
--   * 静态库间存在循环引用 → GNU ld 需 --start-group/--end-group；
--   * JSC 内部原子操作依赖 -latomic。
--
-- 纯预编译包：tarball 统一解包到 bun-webkit/{include,lib,bin}，无源码可编译。
-- mcpp 的 mcpp 段要求 sources 非空，故提供一个 anchor TU（同官方索引
-- compat.openblas 的先例）：linux 由 install() 写出（缺失的 anchor 正是触发
-- mcpp 运行 install() 的机制，libstdc++.a 拷贝借此完成）；macosx/windows 无
-- install() 需求，anchor 走 generated_files。实际链接全部经 ldflags 指向
-- 预编译静态库。
package = {
    spec        = "1",
    namespace   = "mbun",
    name        = "mbun.jsc-prebuilt",
    description = "JavaScriptCore prebuilt static libs from bun's WebKit fork (oven-sh/WebKit autobuild)",
    licenses    = { "LGPL-2.1", "BSD-2-Clause" },  -- JSC: LGPL-2.1; WTF/bmalloc: BSD
    repo        = "https://github.com/oven-sh/WebKit",
    type        = "package",

    xpm = {
        linux = {
            -- libstdc++.a 的来源（版本与 mbun CI 的 gcc 工具链对齐）。
            deps = { "xim:gcc@16.1.0" },
            ["20260706"] = {
                url    = {
                    GLOBAL = "https://github.com/oven-sh/WebKit/releases/download/autobuild-c9ad5813fd23bd8b98b0738abc3d037ec716aa92/bun-webkit-linux-amd64.tar.gz",
                    CN     = "https://gitcode.com/mcpp-res/mbun.jsc-prebuilt/releases/download/mbun.jsc-prebuilt-20260706/bun-webkit-linux-amd64.tar.gz",
                },
                sha256 = "8602e5abca24b0b4b241a7d4aec627ccfcea770870976b7dadbce8163860acf7",
            },
        },
        macosx = {
            ["20260706"] = {
                url    = {
                    GLOBAL = "https://github.com/oven-sh/WebKit/releases/download/autobuild-c9ad5813fd23bd8b98b0738abc3d037ec716aa92/bun-webkit-macos-arm64.tar.gz",
                    CN     = "https://gitcode.com/mcpp-res/mbun.jsc-prebuilt/releases/download/mbun.jsc-prebuilt-20260706/bun-webkit-macos-arm64.tar.gz",
                },
                sha256 = "f29d66c62ac8901f8d708dbc0bd57be2dd3bef8cb017e2e4d72500c410c18c43",
            },
        },
        windows = {
            ["20260706"] = {
                url    = {
                    GLOBAL = "https://github.com/oven-sh/WebKit/releases/download/autobuild-c9ad5813fd23bd8b98b0738abc3d037ec716aa92/bun-webkit-windows-amd64.tar.gz",
                    CN     = "https://gitcode.com/mcpp-res/mbun.jsc-prebuilt/releases/download/mbun.jsc-prebuilt-20260706/bun-webkit-windows-amd64.tar.gz",
                },
                sha256 = "e70f57da53381b1b85063d312eaecf69c2c37ab6fca62b20b50ed5817571889b",
            },
        },
    },

    mcpp = {
        schema       = "0.1",
        language     = "c++23",
        import_std   = false,  -- 仅一个 C anchor TU，无 C++ 源
        c_standard   = "c17",
        -- tarball 解包出的 bun-webkit/include 同时含 JSC 公有头、PrivateHeaders
        -- 与 ICU 75.1 头（unicode/）。
        include_dirs = { "bun-webkit/include" },
        -- anchor TU：满足「sources 非空」约束并给 lib 目标一个可编译单元。
        -- linux 上由 install() 写出（见文件头注释），macosx/windows 走
        -- generated_files（per-OS 块）。
        sources      = { "mcpp_jsc_prebuilt_anchor.c" },
        targets      = { ["jsc-prebuilt"] = { kind = "lib" } },
        deps         = { },

        -- 链接参数按平台隔离（`-L` 相对路径由 mcpp 重写为包安装目录下的绝对路径）。
        linux = {
            ldflags = {
                "-Lbun-webkit/lib",
                "-Wl,--start-group",
                "-lJavaScriptCore", "-lWTF", "-lbmalloc",
                "-licui18n", "-licuuc", "-licudata",
                "-Wl,--end-group",
                "-latomic",
                -- install() 拷入的 bun-webkit/lib/libstdc++.a（见文件头）。
                -- 用 -l:libstdc++.a 而非 -lstdc++：clang 驱动会把字面 -lstdc++
                -- 翻译成其配置的 C++ 标准库（libc++），根本到不了链接器；
                -- -l: 显式档案名直达 ld/lld，且强制静态。
                "-l:libstdc++.a",
            },
        },
        -- macosx/windows：xpm 产物 URL/sha256 已就位；链接参数按产物 lib/ 实际
        -- 内容（tar 清单实查）推导，端到端验证归 T3.1b（设计文档 §3.3）。
        macosx = {
            -- 产物 lib/ 仅含 JSC/WTF/bmalloc（无 ICU 静态库）——macOS 与上游
            -- WebKit 同策略，链接系统 ICU（libicucore）。ld64 默认多遍解析，
            -- 无 --start-group；无需 -latomic。TODO(T3.1b): macOS 实机验证。
            ldflags = {
                "-Lbun-webkit/lib",
                "-lJavaScriptCore", "-lWTF", "-lbmalloc",
                "-licucore",
            },
            generated_files = {
                ["mcpp_jsc_prebuilt_anchor.c"] = "int mcpp_mbun_jsc_prebuilt_anchor(void) { return 0; }\n",
            },
        },
        windows = {
            -- 产物 lib/：JavaScriptCore.lib/WTF.lib/bmalloc.lib + 静态 ICU
            -- （sicuin/sicuuc/sicudt）。clang MSVC 驱动将 -lX 映射为 X.lib。
            -- TODO(T3.1b): 静态 CRT (/MT) 对齐验证后启用。
            ldflags = {
                "-Lbun-webkit/lib",
                "-lJavaScriptCore", "-lWTF", "-lbmalloc",
                "-lsicuin", "-lsicuuc", "-lsicudt",
            },
            generated_files = {
                ["mcpp_jsc_prebuilt_anchor.c"] = "int mcpp_mbun_jsc_prebuilt_anchor(void) { return 0; }\n",
            },
        },
    },
}

import("xim.libxpkg.pkginfo")
import("xim.libxpkg.log")

-- linux：把构建依赖 xim:gcc 的 libstdc++.a 拷入 bun-webkit/lib（供 ldflags
-- 的 -lstdc++ 解析，路径可移植），并写出 anchor TU——anchor 的缺失正是让
-- mcpp 在构建前运行本 install() 的触发器（compat.openblas 同款机制）。
-- macosx/windows：anchor 来自 generated_files，mcpp 自足，无需本钩子。
function install()
    if os.host() ~= "linux" then
        return true
    end

    local prefix = pkginfo.install_dir()
    local wkdir  = path.join(prefix, "bun-webkit")

    -- xim 把 tarball 解包在下载运行目录（install_file 同目录）；无 hook 的包
    -- 由 xim 事后回填 install_dir，但定义了 install() 的包需自行落位 payload
    -- （compat.openblas 同款处理：hook 负责把解包产物移入 install_dir）。
    if not os.isdir(wkdir) then
        local ifile = pkginfo.install_file()
        local extracted = ifile and path.join(path.directory(ifile), "bun-webkit")
        if extracted and os.isdir(extracted) then
            os.mv(extracted, wkdir)
        end
    end

    local libdir = path.join(wkdir, "lib")
    if not os.isdir(libdir) then
        log.error("mbun.jsc-prebuilt: bun-webkit/lib not found after extraction (%s)", libdir)
        return false
    end

    local gcc = pkginfo.build_dep("xim:gcc") or pkginfo.build_dep("gcc")
    if not gcc then
        log.error("mbun.jsc-prebuilt: build dep xim:gcc unavailable (needed for libstdc++.a)")
        return false
    end
    local archive = path.join(gcc.path, "lib64", "libstdc++.a")
    if not os.isfile(archive) then
        log.error("mbun.jsc-prebuilt: libstdc++.a not found under %s", gcc.path)
        return false
    end
    os.cp(archive, path.join(libdir, "libstdc++.a"))

    io.writefile(path.join(prefix, "mcpp_jsc_prebuilt_anchor.c"),
                 "int mcpp_mbun_jsc_prebuilt_anchor(void) { return 0; }\n")
    return true
end
