---
name: issue-reporting
description: 向本仓提交 issue / 发起讨论时用。规定一份好问题反馈的 SOP——软件/版本、报错信息、初步分析、相关资料,并去除本地隐私(用户名/token 等替换)。凡要创建 issue、反馈 bug、发起讨论,先读本 skill 再动手。
---

# issue-reporting —— 提交问题的 SOP

目标:一份能被**直接复现和分诊**的问题反馈,不来回追问。适用于 bug 反馈、行为疑问、
新功能提案的初始 issue。开发流程见 [`dev-process`](../dev-process/SKILL.md)。

## 必备要素(缺一不可)

1. **环境**:OS + 架构;mcpp / 工具链版本;mbun 构建版本(`mcpp run -- --version`);
   相关依赖版本(如某 npm 包)。
2. **复现**:最小复现步骤 + 确切命令 + 输入(附最小脚本/仓库更佳)。
3. **报错信息**:完整 stderr / backtrace。崩溃/hang 经沙箱抓,见
   [`mbun-runtime-debugging`](../mbun-runtime-debugging/SKILL.md)(`tools/integration/safe-test.sh`)。
4. **初步分析**:你或 agent 的判断——可疑的 API、源码位置、与预期的差异。
5. **相关资料**:上游 bun/node 的对照行为、相关 issue/PR、文档链接。

## 去隐私(提交前必做)

- 绝对路径里的**用户名 / 主机名**替换为占位符:`/home/<user>/…`、`<host>`。
- 删除 **token / 密钥 / 密码 / 私有邮箱 / 内网地址**。
- 环境变量、日志片段同样清洗。

## 分诊

- issue 打**类型**标签(`特性|feature` / `缺陷|bug` / …);开放认领的加
  `待认领|available`。标签体系见 [`hagent/labels.md`](../../../hagent/labels.md)。

## 模板骨架(可复制)

```markdown
### 环境
- OS/架构:
- mcpp/工具链:
- mbun 版本(mcpp run -- --version):
- 依赖版本:

### 复现步骤
1.
命令:
输入/脚本:

### 期望 vs 实际
期望:
实际:

### 报错信息
```
<完整 stderr / backtrace,已去隐私>
```

### 初步分析
- 可疑 API / 源码位置:

### 相关资料
- 上游对照 / issue / 文档:
```

## 禁止

- ❌ 只说「跑不了」而无版本 / 命令 / 报错。
- ❌ 贴含用户名 / token / 内网地址的原始日志。
- ❌ 在 issue 里堆无关的完整日志——只贴相关片段。
