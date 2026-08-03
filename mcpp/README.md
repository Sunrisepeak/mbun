# mbun 本地 mcpp 包索引

存放官方 [mcpp-index](https://github.com/mcpplibs/mcpp-index) 尚未收录、但 mbun 需要的第三方库描述符（bun 依赖的 mimalloc、boringssl、usockets、simdutf、lolhtml、c-ares 等）。

## 目录格式（与官方 mcpp-index 一致）

```
mcpp/
├── index.toml                 # [index] spec/min_mcpp 版本契约
└── pkgs/<首字母>/<完整包名>.lua  # 首字母 = 完整包名首字符（mbun.mimalloc → m/）
```

## 使用方式

项目根 `mcpp.toml` 已挂载本索引：

```toml
[indices]
mbun = { path = "mcpp" }      # ⚠️ key 必须与描述符的 namespace 一致

[dependencies.mbun]
mimalloc = "2.1.7+mbun.1"
```

## 添加新包 SOP

1. 调研上游库的 release tarball URL，下载后 `sha256sum` 计算校验和。
2. 参照 `pkgs/m/mbun.mimalloc.lua` 编写描述符：`namespace = "mbun"`、`name = "mbun.<短名>"`，文件放 `pkgs/<name 首字母>/mbun.<短名>.lua`；三平台（linux/macosx/windows）的 `xpm` 条目都要声明。
3. 用 `mcpp xpkg parse mcpp/pkgs/<x>/<file>.lua` 严格校验（未知键会报错）。
4. 在 `mcpp.toml` 的 `[dependencies.mbun]` 声明后运行 `mcpp build` 验证下载与编译。
5. 包会安装到项目级沙盒 `.mcpp/`（已 gitignore），不污染全局环境。

GitHub release 归档通常带顶层目录。描述符应写精确的解压后路径（例如
`mimalloc-2.1.7/include`），禁止用 `*/include` 一类宽通配符；宽通配符可能把归档中
不相关的 GCC/glibc 头目录传播给 LLVM。仅修订构建 recipe 而上游版本不变时，使用
`+mbun.N` 版本并同步成员 manifest 与 `mcpp.lock`，避免复用旧包缓存。

## 约束

- 描述符中禁止本机绝对路径；URL 必须是公网可下载的 tarball，`sha256` 必填。
- 构建配置遵循「最新 LLVM 与 GCC 16 的 C++26 特性交集」跨平台约束。
- 若官方 mcpp-index 后续收录了同名库，优先切换到官方索引并删除本地描述符。
