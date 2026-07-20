# mbun | [Rewrite Bun in MC++](https://github.com/Sunrisepeak/mbun/pull/1) - Just for Fun

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

## 贡献与协作 —— AI Agent 开源协作规范(hagent)

mbun 的目标 #3 是探索 AI-agent 驱动的开发与协作。这套模式被成文为 **hagent** ——
一套 **AI Agent 开源协作规范**(**h**uman + **agent**),位于 [`hagent/`](hagent/zh/README.md)
并在本仓 dogfood。核心命题:**agent 是执行引擎,构建者(运行 agent 的人)负责创作
与判断**(架构、决策、品位、规范)。所有操作归属到构建者,任何改动没有可复现证据不算
「完成」。

### 参与贡献

一切都经你的编码 agent —— 在仓库根目录启动它,它会自动加载 `AGENTS.md` 与相关
skill,已经知道本仓规范。选一个适合你的层级,开 PR 前务必 review agent 的产出。

1. **报告与讨论** —— 遇到 bug 或有想法?让 agent 按提问 SOP 建 issue / 讨论:软件
   版本、报错信息、初步分析、相关资料 —— 并去除本地隐私(用户名、token)。
   → [`issue-reporting`](.agents/skills/issue-reporting/SKILL.md)
2. **验证与审查** —— 复现已报告的 bug、确认是否真实并补充信息;参与 issue 与 PR 的
   验证;review 他人的 PR。
3. **开发** —— 选一个任务开发,issue 先行(bugfix / 优化 / 新功能 —— 新功能需经
   issue 讨论并在 `.agents/docs/` 落地设计方案)。
   → [`dev-process`](.agents/skills/dev-process/SKILL.md)、
   [`tdd-workflow`](.agents/skills/tdd-workflow/SKILL.md)

更多:[hagent 总览](hagent/zh/README.md) · [宪法](hagent/zh/charter.md) ·
[贡献](hagent/zh/contributing.md) · [agents](hagent/zh/agents.md) ·
[标签](hagent/zh/labels.md)。

### 项目维护者

信任是一条阶梯 —— 人与 agent 以同一规则获得(宪法
[§4](hagent/zh/charter.md#4-权限层级))。committer 以上的层级仍在完善。

| 角色 | 权限 | 如何申请 | 持有者 |
| --- | --- | --- | --- |
| **Triager 分诊** | 分诊、打标签、验证 issue 与 PR、跑 CI | 合入 ≥ 3 个 PR 后自助 | —— |
| **Committer 提交者** | 写权限(push / merge) | 模块背景 + 重要贡献;维护者批准 | —— |
| **模块维护者** | 某模块的审查与决策 | —— | —— |
| **项目维护者** | 治理、受保护面 | —— | [@sunrisepeak](https://github.com/sunrisepeak) |

## 兼容性数据

以下是源码快照（2026-07-21）针对 `compat/` 下 submodule 固定的上游测试集的测量结果，由 `tools/integration/` 中的 runner 产出。未支持项不会被伪造成通过，数据也不是 release 保证：

| 测试对象 | 结果 | 通过率 |
| --- | ---: | ---: |
| Bun 原生测试集（`compat/bun/test`） | 1,902 个文件中 874 个全绿 | 46.0% |
| Bun 原生测试（按测试计） | 52,175 个运行，31,818 通过 / 17,501 失败 | 60.9% |
| Node.js 原生测试（`compat/node/test/parallel`） | 4,433 个文件中 1,811 个通过（直接执行） | 40.9% |
| Elysia 测试套件 | 1,522 通过 / 3 失败 | 99.8% |

文件级"全绿"要求文件内所有执行的测试全部通过、且文件未报告测试之外的错误，比 API 清单严格，因此低于测试级通过率。上游本就没有可运行测试的文件、全部被 skip 的文件、以及需要本机不具备的服务（MySQL、Redis、npm registry）的文件各自单独归类，一律不算通过。Node.js 文件通过 mbun 直接执行（退出码 0 记为通过），不模拟 Node 自身 harness 提供的服务，所以该数据是诚实的文件级覆盖，不等同 API 完成度。崩溃被修复后测试级通过率会下降——此前段错误或被 OOM 杀掉的文件现在能跑完并报出真实失败。细节与复现方法见 [`compat/README.md`](compat/README.md)。

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
