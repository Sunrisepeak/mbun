# mbun | Rewrite Bun in MC++ - Just for Fun

[English](README.md)

`mbun` 是使用 [mcpp](https://github.com/mcpp-community/mcpp) 构建的模块化 C++26 JavaScript 运行时——用 MC++ 重写 Bun，同时兼容 Node.js 和 Bun：Bun API 与 `bun:test`、`node:*` 内置模块、CommonJS/ESM 以及 npm 包生态，全部运行在同一个可执行文件里。

**为什么用 MC++ 重写 Bun？**

1. **Rewrite行为艺术复刻**——[Rewrite Bun in Rust](https://github.com/oven-sh/bun/pull/30412)
2. **[mcpp](https://github.com/mcpp-community/mcpp) 构建工具 + C++模块化工程验证**——把 [mcpp](https://github.com/mcpp-community/mcpp) 构建工具、C++26 与 C++ Modules 放进一个 90+ 模块成员的真实工程里压测可用性。
3. **验证和探索 AI Agent 驱动项目开发&协作**——AI Agent 全流程参与项目开发与协作，做一次端到端实验：验证想法、暴露真实会遇到的问题、摸索人与 Agent 在大型代码库上的协作方式。

> [!CAUTION]
> mbun 是实验性项目，目前以源码和实验性验证为主，请勿用于生产环境。
> 目标不是堆积 API 清单，而是让真实应用逐步可用，并通过 Bun、Node.js 原生测试集持续测量结果。

## 快速开始

### 1. 源码构建 + 最小示例

先安装 [xlings](https://github.com/d2learn/xlings)，再用它安装 mcpp（工具链会自动下载到隔离沙箱，不污染系统）：

```bash
curl -fsSL https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.sh | bash
xlings install mcpp -y
```

构建 mbun 并运行最小示例：

```bash
git clone https://github.com/Sunrisepeak/mbun
cd mbun
git submodule update --init --recursive
mcpp build
mcpp run -- --version   # mbun 2026.07.18.0 + 兼容的 bun/node 版本
mcpp run -- examples/common/hello.ts
```

### 2. 最小 Node 和 Bun 示例

同一个可执行文件原生运行两套 API：

```bash
# Node.js：node:http 内置模块 + CommonJS
mcpp run -- examples/node/http-server.js

# Bun：Bun.serve + TypeScript
mcpp run -- examples/bun/http-server.ts
```

两者都会在 <http://127.0.0.1:3000/> 启动一个服务。

### 3. 知名项目示例：Express 和 Elysia

[Express](https://github.com/expressjs/express) 是 Node.js 最流行的 Web 框架，mbun 可以自己安装 npm 依赖并运行它：

```bash
mcpp run -- --cwd examples/node/express install
mcpp run -- --cwd examples/node/express server.js
```

[Elysia](https://github.com/elysiajs/elysia) 是知名的 Bun 原生 Web 框架，跑在同一个可执行文件上：

```bash
mcpp run -- --cwd examples/bun/elysia install
mcpp run -- --cwd examples/bun/elysia server.ts
```

打开 <http://127.0.0.1:3000/> 即可看到对应示例页面。

## 兼容性数据

以下是源码快照（2026-07-18）针对 `compat/` 下 submodule 固定的上游测试集的测量结果，由 `tools/integration/` 中的 runner 产出。未支持项不会被伪造成通过，数据也不是 release 保证：

| 测试对象 | 结果 | 通过率 |
| --- | ---: | ---: |
| Bun 原生测试集（`compat/bun/test`） | 1,902 个文件中 679 个全绿 | 35.7% |
| Bun 原生测试（按测试计） | 44,250 个运行，26,853 通过 / 15,350 失败 | 60.7% |
| Node.js 原生测试（`compat/node/test/parallel`） | 4,433 个文件中 1,527 个通过（直接执行） | 34.4% |
| Elysia 测试套件 | 1,522 通过 / 3 失败 | 99.8% |

文件级"全绿"要求文件内所有执行的测试全部通过，比 API 清单严格，因此低于测试级通过率。Node.js 文件通过 mbun 直接执行（退出码 0 记为通过），不模拟 Node 自身 harness 提供的服务，所以该数据是诚实的文件级覆盖，不等同 API 完成度。细节与复现方法见 [`compat/README.md`](compat/README.md)。

## 相关开源项目

- [mcpp](https://github.com/mcpp-community/mcpp) — mbun 使用的模块化 C++26 构建与包管理工具
- [Bun](https://github.com/oven-sh/bun) — mbun 对标的 JavaScript 运行时（Bun API、`bun:test`、原生测试集）（[bun.com](https://bun.com)）
- [Node.js](https://github.com/nodejs/node) — mbun 对标的 `node:*` 内置模块、模块系统与原生测试集（[nodejs.org](https://nodejs.org)）
- [Express](https://github.com/expressjs/express) — `examples/node/express` 使用的 Node.js Web 框架（[expressjs.com](https://expressjs.com)）
- [Elysia](https://github.com/elysiajs/elysia) — `examples/bun/elysia` 使用的 Bun 原生 Web 框架（[elysiajs.com](https://elysiajs.com)）
- [WebKit JavaScriptCore](https://github.com/WebKit/WebKit/tree/main/Source/JavaScriptCore) — mbun（与 Bun 相同）内嵌的 JavaScript 引擎

## 重要限制

- 当前主要验证目标是 Linux x86_64。
- JavaScriptCore 和原生依赖通过 mcpp 包构建。
- 兼容性会因 API、依赖包和运行模式变化，示例通过不代表完整兼容 Bun 或 Node.js。
- 当前没有预编译二进制或稳定 release 包。

## License

mbun 代码使用 [MIT](LICENSE) 许可证——与 Node.js 和 Bun 相同。第三方组件（JavaScriptCore、OpenSSL 等）以及 Bun / Node.js 测试集保留各自的上游许可证，统一列在 [LICENSE](LICENSE) 文件末尾。
