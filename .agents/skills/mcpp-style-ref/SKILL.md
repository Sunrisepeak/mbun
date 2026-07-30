---
name: mcpp-style-ref
description: 为 mcpp 项目应用 Modern/Module C++ (C++23) 编码风格。适用于编写或审查带模块的 C++ 代码、命名标识符、组织 .cppm/.cpp 文件，或用户提及 mcpp、module C++、现代 C++ 风格时。
---

# mcpp-style-ref

mcpp 项目的 Modern/Module C++ 风格参考。C++23 起适用（本仓库为 **C++26**），使用 `import std`。

> 本仓库落地示例：`modules/html_rewriter/`（独立 workspace 成员，`src/*.cppm` 模块 + `tests/` 脱离 JS 引擎的单测）。

## 快速参考

### 命名

| 种类 | 风格 | 示例 |
|------|------|------|
| 类型/类 | PascalCase（大驼峰） | `StyleRef`, `HttpServer` |
| 对象/成员 | camelCase（小驼峰） | `fileName`, `configText` |
| 函数 | snake_case（下划线） | `load_config_file()`, `parse_()` |
| 私有 | `_` 后缀 | `fileName_`, `parse_()` |
| 常量 | UPPER_SNAKE | `MAX_SIZE`, `DEFAULT_TIMEOUT` |
| 全局 | `g` 前缀 | `gStyleRef` |
| 命名空间 | 全小写 | `mcpplibs`, `mylib` |

### 模块基础

- 使用 `import std` 替代 `#include <print>` 和 `#include <xxx>`
- 使用 `.cppm` 作为模块接口；分离实现时用 `.cpp`
- `export module module_name;` — 模块声明
- `export import :partition;` — 导出分区
- `import :partition;` — 内部分区（不导出）

### 模块结构

```
// .cppm
export module a;

export import a.b;
export import :a2;   // 可导出分区

import std;
import :a1;          // 内部分区
```

### 模块命名

- 模块：`topdir.subdir.filename`（如 `a.b`, `a.c`）
- 分区：`module_name:partition`（如 `a:a1`, `a.b:b1`）
- 用目录路径区分同名：`a/c.cppm` → `a.c`，`b/c.cppm` → `b.c`

### 类布局

```cpp
class StyleRef {
private:
    std::string fileName_;  // 数据成员带 _ 后缀

public:  // Big Five
    StyleRef() = default;
    StyleRef(const StyleRef&) = default;
    // ...

public:  // 公有接口
    void load_config_file(std::string fileName);  // 函数 snake_case，参数 camelCase

private:
    void parse_(std::string config);  // 私有函数以 _ 结尾
};
```

### 实践规则

- **初始化**：用 `{}` — `int n { 42 }`，`std::vector<int> v { 1, 2, 3 }`
- **字符串**：只读参数用 `std::string_view`
- **错误**：用 `std::optional` / `std::expected` 替代 int 错误码
- **内存**：用 `std::unique_ptr`、`std::shared_ptr`；避免裸 `new`/`delete`
- **RAII**：将资源与对象生命周期绑定
- **auto**：用于迭代器、lambda、复杂类型；需要明确表达意图时保留显式类型
- **宏**：优先用 `constexpr`、`inline`、`concept` 替代宏

### 分层原则：JS 只做最薄的接口层（核心规则）

**能用 C++ 实现的,一律用 C++ 实现。JS 侧只保留最薄的绑定/接口层。**

mbun 的 JS builtin payload（`modules/jsc/src/builtins/*.cppm` 里的 raw string）
存在的理由是**接到 JSC**,不是承载实现。任何有实质逻辑的东西 —— 解析、状态机、
数据结构、编解码、路由、扫描 —— 属于独立的 workspace 成员（`modules/<name>/src/*.cppm`,
`module mbun.<name>`）,由 JS 侧薄薄地调过去。

**判定顺序（按此顺序问自己）:**

1. **仓库里已经有对应的 C++ 模块吗?** 有就接线它。
   *反例,真实发生过:* `modules/router/` 有 358 行 `FileSystemRouter` 且**零引用**,
   一条 lane 没有接线它,而是另写了 ~190 行 JS 架在 `node:fs` 上。测试从 0 到 29 绿,
   但仓库现在有两份路由实现、其中一份没有任何调用者。**这正是本规则要禁止的形状。**
   理由「那个模块缺目录扫描和 JSC 绑定,接线等于重写」不成立 —— 补齐缺的部分就是接线,
   另写一份 JS 是绕过。
2. **能新写成 C++ 模块吗?** 能就写成模块,JS 只留绑定。纯逻辑模块要配 `tests/` 单测
   （脱离 JS 引擎可测),这也是它比 JS payload 更该被选择的原因之一。
3. **只有真正必须在 JS 里的才留在 JS**:JSC 对象语义（原型链、访问器、
   `Symbol.*`、IDL 形状)、node/bun 的 JS 层协议（`internal/*` 的符号身份、
   CJS/ESM 互操作),以及从真实源码 1:1 移植过来的 JS（见下）。

**与「直接移植」的关系,两者不冲突,但顺序要说清:**

node 的 `lib/**` 是 JS,把它 1:1 机械移植进 JS payload 是**当前阶段获取覆盖率
最快的路径**,这是被明确授权的（覆盖优先,性能后置)。**但移植进来的 JS 是有债的**:
它应当在头部标注 `1:1 translation of ...`,并被视为**后续下沉到 C++ 的候选**,
而不是终局形态。规则的次序是:

- 已有 C++ 模块 → **必须**接线,不得另写 JS 绕过（无例外）
- 有 vendored 真源码且当前是手写 JS → **先 1:1 移植**（覆盖优先),标注,记为下沉候选
- 两者都没有,且逻辑有实质分量 → **写成 C++ 模块**,不要新增 JS 实现

**性能是这条规则的理由,但不是当前的门禁。** 现阶段不因为性能挡下移植;
但也不能因为「性能后期再做」就把新的实质逻辑写进 JS —— 那是在制造后期要还的债,
而接线一个已存在的 C++ 模块从来不是性能优化,是不重复造轮子。

### 架构原则：通用内核 + 各方言的薄兼容层（核心规则）

**mbun 是一个通用运行时内核,外面套 node 和 bun 两层薄兼容层。当两个语料对同一个
调用要求不同的行为时,那是**兼容层要分派**的工作项,不是不可达的上限。**

方言由**自动识别**决定,并且**允许用户显式指定**覆盖。

**这条规则的反面已经真实发生过,代价是把工作项误判成天花板。** 多条 lane 各自撞上
同一堵墙后,得出的结论是「同一个二进制无法同时满足,所以 100% 不可达」。已确认的
矛盾至少有 6 处:

| 矛盾 | node 要求 | bun 要求 |
| --- | --- | --- |
| 私钥解码错误 | OpenSSL-3 `DECODER routines::unsupported` | BoringSSL `06000066`(`crypto.key-objects.test.ts:343` 钉死且现绿) |
| `crypto.getCurves()` | 完整 OpenSSL 曲线表 | 实测实现后 node −1、bun −1 |
| HTTP/1.0 framing | 尊重 `TE: chunked`(`test-http-1.0-keep-alive.js`) | 从不 chunk-frame 1.0(`regression/34415`) |
| 入口找不到 | `Cannot find module` + `MODULE_NOT_FOUND` | `error: Module not found "x"`(`bun-run.test.ts` 用 `toBe` 钉死) |
| OKP JWK 校验 | `x` 必须与 `d` 的公钥半匹配 | `web-crypto.test.ts:554` 期望不匹配也成功 |
| `getRandomValues` 配额 | 65536 字节上限 + `QuotaExceededError` | 无配额(其语料把该检查注释掉了) |

这些**每一条都是兼容层的分派点**,按上面的原则处理,不许记为 `COSTS_GREEN` 了事。

**现状(2026-07-31,写下时的实测):方言分派根本不存在。**
`set_argv0`(`modules/jsc/src/runtime/api_impl.inc:12`)已定义但 `app/` 里没有任何
调用点;没有任何地方按 `argv0 == "node"` 分支;`api_impl.inc:224` 注释里提到的
「`node`-argv0 emulation」只是个入口名字,后面没有实现。**先看现状再动手,别假设它已经有了。**

**实现时的次序:**

1. **先归类,再分派。** 每处矛盾要判断它需要什么粒度:
   (a) **进程级**即可(整个进程是 node 味还是 bun 味 —— argv0、入口文件扩展名、
   显式 flag/env、有无 bun 专属配置);(b) 需要**比进程更细**的粒度(同一进程合法地
   两边都做,必须按调用模块或 API 面分派);(c) 真正不可约 —— 必须写清为什么进程级
   选择服务不了它。
2. **默认行为不能因为引入分派而改变。** 分派是在现有行为之外**增加**一条分支,
   不是把现状换成另一种。任何一处分派落地都要在**两个语料**上做门禁,并且比对
   **断言数**而不只是绿文件数 —— 共享面的改动会在不改变文件分类的情况下移动断言
   （crypto 那次实测 368p/1f → 363p/6f 而分类纹丝不动,按绿文件数比的门禁连报两次干净)。
3. **分派点属于 C++,不属于 JS。** 这是上一条「JS 只做最薄接口层」的直接推论:
   方言解析是进程级基础设施,应当在 C++ 侧解析一次并暴露给 JS 层查询,
   而不是在每个 builtin 的 JS payload 里各自嗅探。

### 接口与实现

两种写法均支持。

**写法 A：合并** — 接口与实现同在一个 `.cppm` 中：

```cpp
// mylib.cppm
export module mylib;

export int add(int a, int b) {
    return a + b;
}
```

**写法 B：分离** — 接口在 `.cppm`，实现在 `.cpp`（编译期隐藏实现）：

```cpp
// error.cppm（接口）
export module error;

export struct Error {
    void test();
};
```

```cpp
// error.cpp（实现）
module error;

import std;

void Error::test() {
    std::println("Hello");
}
```

简单模块用写法 A；需隐藏实现或减少编译依赖时用写法 B。

## 本仓库（mbun）环境

工具链由 mcpp 自动解析（GCC 16），无需手动安装编译器：

```bash
xlings install mcpp -y   # 一次性安装 mcpp
mcpp build               # 根目录构建全部
mcpp test                # 在成员目录内跑该成员单测（tests/*.cpp 自动发现）
```

新模块的落地方式：新建 `modules/<name>/`（`mcpp.toml` + `src/<file>.cppm` → 模块 `mbun.<name>`），注册进根 `mcpp.toml` 的 `[workspace] members`；消费方在其 `mcpp.toml` 声明 path 依赖。注意：mcpp 不把非导出分区放进 provider 图——用多个完整模块代替分区。

## 适用场景

- 编写新的 C++ 模块代码（`.cppm`、`.cpp`）
- 审查或重构 mcpp 项目中的 C++ 代码
- 用户询问「mcpp 风格」「module C++ 风格」或「现代 C++ 惯例」

## 更多资源

- 完整参考：[reference.md](reference.md)
- mcpp-style-ref 仓库：[github.com/mcpp-community/mcpp-style-ref](https://github.com/mcpp-community/mcpp-style-ref)
- xlings 包管理器：[github.com/d2learn/xlings](https://github.com/d2learn/xlings)
